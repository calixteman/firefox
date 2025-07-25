/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef jsshell_js_h
#define jsshell_js_h

#include "mozilla/Atomics.h"
#include "mozilla/Maybe.h"
#include "mozilla/TimeStamp.h"

#include "builtin/MapObject.h"
#include "js/CompileOptions.h"
#include "js/GCVector.h"
#include "shell/ModuleLoader.h"
#include "threading/ConditionVariable.h"
#include "threading/LockGuard.h"
#include "threading/Mutex.h"
#include "threading/Thread.h"
#include "vm/GeckoProfiler.h"
#include "vm/Monitor.h"

// Some platform hooks must be implemented for single-step profiling.
#if defined(JS_SIMULATOR_ARM) || defined(JS_SIMULATOR_MIPS64) || \
    defined(JS_SIMULATOR_ARM64) || defined(JS_SIMULATOR_LOONG64)
#  define SINGLESTEP_PROFILING
#endif

namespace js {
namespace shell {

// Define use of application-specific slots on the shell's global object.
enum GlobalAppSlot { GlobalAppSlotModuleRegistry, GlobalAppSlotCount };
static_assert(GlobalAppSlotCount <= JSCLASS_GLOBAL_APPLICATION_SLOTS,
              "Too many applications slots defined for shell global");

enum JSShellErrNum {
#define MSG_DEF(name, count, exception, format) name,
#include "jsshell.msg"
#undef MSG_DEF
  JSShellErr_Limit
};

const JSErrorFormatString* my_GetErrorMessage(void* userRef,
                                              const unsigned errorNumber);

void WarningReporter(JSContext* cx, JSErrorReport* report);

class MOZ_STACK_CLASS AutoReportException {
  JSContext* cx;

 public:
  explicit AutoReportException(JSContext* cx) : cx(cx) {}
  ~AutoReportException();
};

bool GenerateInterfaceHelp(JSContext* cx, JS::HandleObject obj,
                           const char* name);

JSString* FileAsString(JSContext* cx, JS::HandleString pathnameStr);

class AutoCloseFile {
 private:
  FILE* f_;

 public:
  explicit AutoCloseFile(FILE* f) : f_(f) {}
  ~AutoCloseFile() { (void)release(); }
  bool release() {
    bool success = true;
    if (f_ && f_ != stdin && f_ != stdout && f_ != stderr) {
      success = !fclose(f_);
    }
    f_ = nullptr;
    return success;
  }
};

// Reference counted file.
struct RCFile {
  FILE* fp;
  uint32_t numRefs;

  RCFile() : fp(nullptr), numRefs(0) {}
  explicit RCFile(FILE* fp) : fp(fp), numRefs(0) {}

  void acquire() { numRefs++; }

  // Starts out with a ref count of zero.
  static RCFile* create(JSContext* cx, const char* filename, const char* mode);

  void close();
  bool isOpen() const { return fp; }
  bool release();
};

// Shell command-line arguments and count.
extern int sArgc;
extern char** sArgv;

// Shell state set once at startup.
extern const char* selfHostedXDRPath;
extern bool encodeSelfHostedCode;
extern bool enableCodeCoverage;
extern bool enableDisassemblyDumps;
extern bool offthreadBaselineCompilation;
extern bool offthreadIonCompilation;
extern JS::DelazificationOption defaultDelazificationMode;
extern bool enableAsmJS;
extern bool enableWasm;
extern bool enableSharedMemory;
extern bool enableWasmBaseline;
extern bool enableWasmOptimizing;
extern bool enableTestWasmAwaitTier2;
extern bool enableSourcePragmas;
extern bool enableAsyncStacks;
extern bool enableAsyncStackCaptureDebuggeeOnly;
extern bool enableWeakRefs;
extern bool enableToSource;
extern bool enablePropertyErrorMessageFix;
extern bool enableIteratorHelpers;
extern bool enableShadowRealms;
extern bool enableArrayGrouping;
extern bool enableWellFormedUnicodeStrings;
extern bool enableArrayBufferTransfer;
extern bool enableArrayBufferResizable;
extern bool enableSymbolsAsWeakMapKeys;
extern bool enableNewSetMethods;
extern bool enableImportAttributes;
extern bool enableDestructuringFuse;
#ifdef JS_GC_ZEAL
extern uint32_t gZealBits;
extern uint32_t gZealFrequency;
#endif
extern bool printTiming;
extern RCFile* gErrFile;
extern RCFile* gOutFile;
extern bool reportWarnings;
extern bool compileOnly;
extern bool disableOOMFunctions;
extern bool defaultToSameCompartment;

#ifdef DEBUG
extern bool dumpEntrainedVariables;
extern bool OOM_printAllocationCount;
#endif

extern bool useFdlibmForSinCosTan;

extern UniqueChars processWideModuleLoadPath;

// Alias the global dstName to namespaceObj.srcName. For example, if dstName is
// "snarf", namespaceObj represents "os.file", and srcName is "readFile", then
// this is equivalent to the JS code:
//
//   snarf = os.file.readFile;
//
// This provides a mechanism for namespacing the various JS shell helper
// functions without breaking backwards compatibility with things that use the
// global names.
bool CreateAlias(JSContext* cx, const char* dstName,
                 JS::HandleObject namespaceObj, const char* srcName);

class NonshrinkingGCObjectVector
    : public GCVector<HeapPtr<JSObject*>, 0, SystemAllocPolicy> {
 public:
  bool traceWeak(JSTracer* trc) {
    for (HeapPtr<JSObject*>& obj : *this) {
      TraceWeakEdge(trc, &obj, "NonshrinkingGCObjectVector element");
    }
    return true;
  }
};

using MarkBitObservers = JS::WeakCache<NonshrinkingGCObjectVector>;

#ifdef SINGLESTEP_PROFILING
using StackChars = Vector<char16_t, 0, SystemAllocPolicy>;
#endif

class OffThreadJob;

// Per-context shell state.
struct ShellContext {
  enum IsWorkerEnum { Worker = true, MainThread = false };

  explicit ShellContext(JSContext* cx, IsWorkerEnum isWorker_);
  bool registerWithCx(JSContext* cx);
  ~ShellContext();

  JSContext* cx_;

  const IsWorkerEnum isWorker;
  bool lastWarningEnabled;

  // Track promise rejections and report unhandled rejections.
  bool trackUnhandledRejections;

  double timeoutInterval;
  double startTime;
  mozilla::Atomic<bool> serviceInterrupt;
  mozilla::Atomic<bool> haveInterruptFunc;
  JS::PersistentRootedValue interruptFunc;
  JS::PersistentRootedValue lastWarning;
  JS::PersistentRootedValue promiseRejectionTrackerCallback;

  // Rejected promises that are not yet handled. Added when rejection
  // happens, and removed when rejection is handled. This uses SetObject to
  // report unhandled rejections in the rejected order.
  JS::PersistentRooted<SetObject*> unhandledRejectedPromises;

#ifdef SINGLESTEP_PROFILING
  Vector<StackChars, 0, SystemAllocPolicy> stacks;
#endif

  /*
   * Watchdog thread state.
   */
  js::Mutex watchdogLock MOZ_UNANNOTATED;
  js::ConditionVariable watchdogWakeup;
  mozilla::Maybe<js::Thread> watchdogThread;
  mozilla::Maybe<mozilla::TimeStamp> watchdogTimeout;

  js::ConditionVariable sleepWakeup;

  int exitCode;
  bool quitting;

  JS::UniqueChars readLineBuf;
  size_t readLineBufPos;

  js::shell::RCFile** errFilePtr;
  js::shell::RCFile** outFilePtr;

  UniquePtr<ProfilingStack> geckoProfilingStack;

  UniquePtr<ModuleLoader> moduleLoader;

  UniquePtr<MarkBitObservers> markObservers;

  // Off-thread parse state.
  js::Monitor offThreadMonitor MOZ_UNANNOTATED;
  Vector<OffThreadJob*, 0, SystemAllocPolicy> offThreadJobs;

  // Queued task callbacks that run after the microtask queue.

  using ObjectVector = GCVector<JSObject*, 0, SystemAllocPolicy>;
  JS::PersistentRooted<ObjectVector> taskCallbacks;
};

extern ShellContext* GetShellContext(JSContext* cx);

[[nodiscard]] extern bool PrintStackTrace(JSContext* cx,
                                          JS::Handle<JSObject*> stackObj);

} /* namespace shell */
} /* namespace js */

#endif
