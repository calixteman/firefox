/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/ipc/ForkServer.h"

#include "base/eintr_wrapper.h"
#include "chrome/common/chrome_switches.h"
#include "ipc/IPCMessageUtilsSpecializations.h"
#include "mozilla/BlockingResourceBase.h"
#include "mozilla/GeckoArgs.h"
#include "mozilla/Logging.h"
#include "mozilla/Omnijar.h"
#include "mozilla/ProcessType.h"
#include "mozilla/ipc/FileDescriptor.h"
#include "mozilla/ipc/IPDLParamTraits.h"
#include "mozilla/ipc/ProcessUtils.h"
#include "mozilla/ipc/ProtocolMessageUtils.h"
#include "mozilla/ipc/SetProcessTitle.h"
#include "nsTraceRefcnt.h"

#include <fcntl.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(XP_LINUX) && defined(MOZ_SANDBOX)
#  include "mozilla/SandboxLaunch.h"
#endif

#include <algorithm>

namespace mozilla {
namespace ipc {

LazyLogModule gForkServiceLog("ForkService");

static int gSignalPipe = -1;
static void HandleSigChld(int aSignal) {
  MOZ_ASSERT(aSignal == SIGCHLD);
  const char msg = 0;
  HANDLE_EINTR(write(gSignalPipe, &msg, 1));
}

ForkServer::ForkServer(int* aArgc, char*** aArgv) : mArgc(aArgc), mArgv(aArgv) {
  SetThisProcessName("forkserver");

  Maybe<UniqueFileHandle> ipcHandle = geckoargs::sIPCHandle.Get(*aArgc, *aArgv);
  if (!ipcHandle) {
    MOZ_CRASH("forkserver missing ipcHandle argument");
  }

  // Hold our IPC FD while our MiniTransceiver is alive.
  mIpcFd = ipcHandle.extract();
  mTcver = MakeUnique<MiniTransceiver>(mIpcFd.get(),
                                       DataBufferClear::AfterReceiving);

  auto signalPipe = geckoargs::sSignalPipe.Get(*aArgc, *aArgv);
  if (signalPipe) {
    gSignalPipe = signalPipe->release();
    signal(SIGCHLD, HandleSigChld);
  } else {
    signal(SIGCHLD, SIG_IGN);
  }
}

/**
 * Preload any resources that the forked child processes might need,
 * and which might change incompatibly or become unavailable by the
 * time they're started.  For example: the omnijar files, or certain
 * shared libraries.
 */
static void ForkServerPreload(int& aArgc, char** aArgv) {
  Omnijar::ChildProcessInit(aArgc, aArgv);
}

/**
 * Start providing the service at the IPC channel.
 */
bool ForkServer::HandleMessages() {
  while (true) {
    UniquePtr<IPC::Message> msg;
    if (!mTcver->Recv(msg)) {
      break;
    }

    switch (msg->type()) {
      case Msg_ForkNewSubprocess__ID:
        if (HandleForkNewSubprocess(std::move(msg))) {
          // New process - child
          return false;
        }
        break;
      case Msg_WaitPid__ID:
        HandleWaitPid(std::move(msg));
        break;
      default:
        MOZ_LOG(gForkServiceLog, LogLevel::Verbose,
                ("unknown message type %d\n", msg->type()));
    }
  }
  // Stop the server
  return true;
}

template <class P>
static void ReadParamInfallible(IPC::MessageReader* aReader, P* aResult,
                                const char* aCrashMessage) {
  if (!IPC::ReadParam(aReader, aResult)) {
    MOZ_CRASH_UNSAFE(aCrashMessage);
  }
}

/**
 * Parse a Message to obtain a `LaunchOptions` and the attached fd
 * that the child will use to receive its `SubprocessExecInfo`.
 */
static bool ParseForkNewSubprocess(IPC::Message& aMsg,
                                   UniqueFileHandle* aExecFd,
                                   base::LaunchOptions* aOptions) {
  // The type was already checked in HandleMessages
  MOZ_ASSERT(aMsg.type() == Msg_ForkNewSubprocess__ID);
  IPC::MessageReader reader(aMsg);

#if defined(XP_LINUX) && defined(MOZ_SANDBOX)
  ReadParamInfallible(&reader, &aOptions->fork_flags,
                      "Error deserializing 'int'");
  ReadParamInfallible(&reader, &aOptions->sandbox_chroot_server,
                      "Error deserializing 'UniqueFileHandle'");
#endif
  ReadParamInfallible(&reader, aExecFd,
                      "Error deserializing 'UniqueFileHandle'");
  reader.EndRead();

  return true;
}

/**
 * Parse a `Message`, in the forked child process, to get the argument
 * and environment strings.
 */
static bool ParseSubprocessExecInfo(IPC::Message& aMsg,
                                    geckoargs::ChildProcessArgs* aArgs,
                                    base::environment_map* aEnv) {
  if (aMsg.type() != Msg_SubprocessExecInfo__ID) {
    MOZ_LOG(gForkServiceLog, LogLevel::Verbose,
            ("unknown message type %d (!= %d)\n", aMsg.type(),
             Msg_SubprocessExecInfo__ID));
    return false;
  }

  IPC::MessageReader reader(aMsg);

  ReadParamInfallible(&reader, aEnv, "Error deserializing 'env_map'");
  ReadParamInfallible(&reader, &aArgs->mArgs, "Error deserializing 'mArgs'");
  ReadParamInfallible(&reader, &aArgs->mFiles, "Error deserializing 'mFiles'");
  reader.EndRead();

  return true;
}

// Run in the forked child process. Receives a message on `aExecFd` containing
// the new process configuration, and updates the environment, command line, and
// passed file handles to reflect the new process.
static void ForkedChildProcessInit(int aExecFd, int* aArgc, char*** aArgv) {
  // Remove the fork-server-specific SIGCHLD handler.
  signal(SIGCHLD, SIG_DFL);
  // This process is currently single-threaded, so the fd used by the
  // signal handler can be safely closed once the handler is removed.
  if (gSignalPipe >= 0) {
    close(gSignalPipe);
    gSignalPipe = -1;
  }

  // Content process
  MiniTransceiver execTcver(aExecFd);
  UniquePtr<IPC::Message> execMsg;
  if (!execTcver.Recv(execMsg)) {
    // Crashing here isn't great, because the crash reporter isn't
    // set up, but we don't have a lot of options currently.  Also,
    // receive probably won't fail unless the parent also crashes.
    printf_stderr("ForkServer: SubprocessExecInfo receive error\n");
    MOZ_CRASH();
  }

  geckoargs::ChildProcessArgs args;
  base::environment_map env;
  if (!ParseSubprocessExecInfo(*execMsg, &args, &env)) {
    printf_stderr("ForkServer: SubprocessExecInfo parse error\n");
    MOZ_CRASH();
  }

  // Set environment variables as specified in env_map.
  for (auto& elt : env) {
    setenv(elt.first.c_str(), elt.second.c_str(), 1);
  }

  // Initialize passed file handles.
  geckoargs::SetPassedFileHandles(std::move(args.mFiles));

  // Change argc & argv of main() with the arguments passing
  // through IPC.
  char** argv = new char*[args.mArgs.size() + 1];
  char** p = argv;
  for (auto& elt : args.mArgs) {
    *p++ = strdup(elt.c_str());
  }
  *p = nullptr;
  *aArgv = argv;
  *aArgc = args.mArgs.size();
  mozilla::SetProcessTitle(args.mArgs);
}

/**
 * Extract parameters from the |Message| to create a
 * |base::AppProcessBuilder| as |mAppProcBuilder|.
 *
 * It will return in both the fork server process and the new content
 * process.  |mAppProcBuilder| is null for the fork server.
 */
bool ForkServer::HandleForkNewSubprocess(UniquePtr<IPC::Message> aMessage) {
  UniqueFileHandle execFd;
  base::LaunchOptions options;
  if (!ParseForkNewSubprocess(*aMessage, &execFd, &options)) {
    return false;
  }

#if defined(MOZ_MEMORY) && defined(DEBUG)
  jemalloc_stats_t stats;
  jemalloc_stats(&stats);
  // What we actually want to assert is that there are 0 thread-local arenas
  // (threads may exist but thread-local arenas are opt-in) that would be leaked
  // (because the threads wont exist in the new process), and 0 private
  // main-thread-only arenas and this is not the main thread (as those might be
  // inconsistent in the new process).  Instead we check that there's exactly
  // one arena - the default public arena).
  MOZ_ASSERT(stats.narenas == 1,
             "ForkServer before fork()/clone() should have a single arena.");
#endif

#if defined(XP_LINUX) && defined(MOZ_SANDBOX)
  mozilla::SandboxLaunch launcher;
  if (!launcher.Prepare(&options)) {
    MOZ_CRASH("SandboxLaunch::Prepare failed");
  }
#else
  struct {
    pid_t Fork() { return fork(); }
  } launcher;
#endif

  // Avoid any contents of buffered stdout/stderr being sent by forked
  // children.
  fflush(stdout);
  fflush(stderr);

  pid_t pid = launcher.Fork();
  if (pid < 0) {
    MOZ_CRASH("failed to fork");
  }

  // NOTE: After this point, if pid == 0, we're in the newly forked child
  // process.

  if (pid == 0) {
    // Re-configure to a child process, and return to our caller.
    ForkedChildProcessInit(execFd.get(), mArgc, mArgv);
    return true;
  }

  // Fork server process

  IPC::Message reply(MSG_ROUTING_CONTROL, Reply_ForkNewSubprocess__ID);
  IPC::MessageWriter writer(reply);
  WriteIPDLParam(&writer, nullptr, pid);
  mTcver->SendInfallible(reply, "failed to send a reply message");

  return false;
}

void ForkServer::HandleWaitPid(UniquePtr<IPC::Message> aMessage) {
  MOZ_ASSERT(aMessage->type() == Msg_WaitPid__ID);
  IPC::MessageReader reader(*aMessage);

  pid_t pid;
  bool block;
  ReadParamInfallible(&reader, &pid, "Error deserializing 'pid_t'");
  ReadParamInfallible(&reader, &block, "Error deserializing 'bool'");

  // It's safe to use plain waitpid here (and not the waitid/WNOWAIT
  // contraption used in the parent process) because this process is
  // single-threaded so there's no possibility of another thread
  // trying to ptrace the same child process.
  int status;
  pid_t rv = HANDLE_EINTR(waitpid(pid, &status, block ? 0 : WNOHANG));
  // Three possibilities here:
  //  Terminated: rv > 0; return {false, status}
  //     Running: rv = 0; return {true, 0}
  //       Error: rv < 0; return {true, errno}
  bool isErr = rv <= 0;
  int err = rv < 0 ? errno : 0;
  MOZ_ASSERT(isErr || rv == pid);

  IPC::Message reply(MSG_ROUTING_CONTROL, Reply_WaitPid__ID);
  IPC::MessageWriter writer(reply);
  WriteParam(&writer, isErr);
  WriteParam(&writer, isErr ? err : status);
  mTcver->SendInfallible(reply, "failed to send a reply message");
}

/**
 * Setup and run a fork server at the main thread.
 *
 * This function returns for two reasons:
 *  - the fork server is stopped normally, or
 *  - a new process is forked from the fork server and this function
 *    returned in the child, the new process.
 *
 * For the later case, aArgc and aArgv are modified to pass the
 * arguments from the chrome process.
 */
bool ForkServer::RunForkServer(int* aArgc, char*** aArgv) {
  MOZ_ASSERT(XRE_IsForkServerProcess(), "fork server process only");

#ifdef DEBUG
  if (getenv("MOZ_FORKSERVER_WAIT_GDB")) {
    printf(
        "Waiting for 30 seconds."
        "  Attach the fork server with gdb %s %d\n",
        (*aArgv)[0], base::GetCurrentProcId());
    sleep(30);
  }
  bool sleep_newproc = !!getenv("MOZ_FORKSERVER_WAIT_GDB_NEWPROC");
#endif

  SetProcessTitleInit(*aArgv);

  // Do this before NS_LogInit() to avoid log files taking lower
  // FDs.
  ForkServer forkserver(aArgc, aArgv);

  NS_LogInit();
  mozilla::LogModule::Init(0, nullptr);
  ForkServerPreload(*aArgc, *aArgv);
  MOZ_LOG(gForkServiceLog, LogLevel::Verbose, ("Start a fork server"));
  {
    DebugOnly<base::ProcessHandle> forkserver_pid = base::GetCurrentProcId();
    if (forkserver.HandleMessages()) {
      // In the fork server process
      // The server has stopped.
      MOZ_LOG(gForkServiceLog, LogLevel::Verbose,
              ("Terminate the fork server"));
      Omnijar::CleanUp();
      NS_LogTerm();
      return true;
    }
    // Now, we are running in a content process just forked from
    // the fork server process.
    MOZ_ASSERT(base::GetCurrentProcId() != forkserver_pid);
    MOZ_LOG(gForkServiceLog, LogLevel::Verbose, ("Fork a new content process"));
  }
#ifdef DEBUG
  if (sleep_newproc) {
    printf(
        "Waiting for 30 seconds."
        "  Attach the new process with gdb %s %d\n",
        (*aArgv)[0], base::GetCurrentProcId());
    sleep(30);
  }
#endif
  NS_LogTerm();

  nsTraceRefcnt::CloseLogFilesAfterFork();

  // Update our GeckoProcessType and GeckoChildID, removing the arguments.
  if (*aArgc < 2) {
    MOZ_CRASH("forked process missing process type and childid arguments");
  }
  SetGeckoProcessType((*aArgv)[--*aArgc]);
  SetGeckoChildID((*aArgv)[--*aArgc]);
  MOZ_ASSERT(!XRE_IsForkServerProcess(),
             "fork server created another fork server?");

  // This is now a child process, and it may even be a Content process.
  // It is required that the PRNG at least is re-initialized so the same state
  // is not shared accross all child processes, and in case of a Content process
  // it is also required that the small allocation are not being randomized ;
  // failing to do so will lead to performance regressions, e.g. as in
  // bug 1912262.
#if defined(MOZ_MEMORY)
  jemalloc_reset_small_alloc_randomization(
      /* aRandomizeSmall */ !XRE_IsContentProcess());
#endif

  // Open log files again with right names and the new PID.
  nsTraceRefcnt::ReopenLogFilesAfterFork(XRE_GetProcessTypeString());

  return false;
}

}  // namespace ipc
}  // namespace mozilla
