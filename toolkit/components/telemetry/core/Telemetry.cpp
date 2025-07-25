/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Telemetry.h"

#include <algorithm>
#include <prio.h>
#include <prproces.h>
#if defined(XP_UNIX) && !defined(XP_DARWIN)
#  include <time.h>
#else
#  include <chrono>
#endif
#include "base/pickle.h"
#include "base/process_util.h"
#include "ipc/TelemetryIPCAccumulator.h"
#include "jsapi.h"
#include "jsfriendapi.h"
#include "js/Array.h"  // JS::NewArrayObject
#include "js/GCAPI.h"
#include "js/PropertyAndElement.h"  // JS_DefineElement, JS_DefineProperty
#include "mozilla/dom/ToJSValue.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/glean/ProfilesMetrics.h"
#include "mozilla/glean/TelemetryMetrics.h"
#include "mozilla/Atomics.h"
#include "mozilla/Attributes.h"
#include "mozilla/BackgroundHangMonitor.h"
#ifdef MOZ_BACKGROUNDTASKS
#  include "mozilla/BackgroundTasks.h"
#endif
#include "mozilla/Components.h"
#include "mozilla/DataMutex.h"
#include "mozilla/DebugOnly.h"
#include "mozilla/FStream.h"
#include "mozilla/IOInterposer.h"
#include "mozilla/Likely.h"
#include "mozilla/MathAlgorithms.h"
#include "mozilla/MemoryReporting.h"
#include "mozilla/MemoryTelemetry.h"
#include "mozilla/ModuleUtils.h"
#include "mozilla/Mutex.h"
#include "mozilla/PoisonIOInterposer.h"
#include "mozilla/Preferences.h"
#include "mozilla/StartupTimeline.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/Unused.h"
#if defined(XP_WIN)
#  include "mozilla/WinDllServices.h"
#endif
#include "nsAppDirectoryServiceDefs.h"
#include "nsBaseHashtable.h"
#include "nsClassHashtable.h"
#include "nsCOMArray.h"
#include "nsCOMPtr.h"
#include "nsTHashMap.h"
#include "nsHashKeys.h"
#include "nsIDirectoryEnumerator.h"
#include "nsDirectoryServiceDefs.h"
#include "nsIFileStreams.h"
#include "nsIMemoryReporter.h"
#include "nsISeekableStream.h"
#include "nsITelemetry.h"
#if defined(XP_WIN)
#  include "other/UntrustedModules.h"
#endif
#include "nsJSUtils.h"
#include "nsNativeCharsetUtils.h"
#include "nsNetCID.h"
#include "nsNetUtil.h"
#include "nsProxyRelease.h"
#include "nsReadableUtils.h"
#include "nsString.h"
#include "nsTHashtable.h"
#include "nsThreadUtils.h"
#if defined(XP_WIN)
#  include "nsUnicharUtils.h"
#endif
#include "nsVersionComparator.h"
#include "nsXPCOMCIDInternal.h"
#include "other/CombinedStacks.h"
#include "other/TelemetryIOInterposeObserver.h"
#include "TelemetryCommon.h"
#include "TelemetryEvent.h"
#include "TelemetryHistogram.h"
#include "TelemetryScalar.h"
#include "TelemetryUserInteraction.h"

namespace {

using namespace mozilla;
using mozilla::dom::Promise;
using mozilla::Telemetry::CombinedStacks;
using mozilla::Telemetry::EventExtraEntry;
using mozilla::Telemetry::TelemetryIOInterposeObserver;
using Telemetry::Common::AutoHashtable;
using Telemetry::Common::GetCurrentProduct;
using Telemetry::Common::StringHashSet;
using Telemetry::Common::SupportedProduct;
using Telemetry::Common::ToJSString;

// This is not a member of TelemetryImpl because we want to record I/O during
// startup.
StaticAutoPtr<TelemetryIOInterposeObserver> sTelemetryIOObserver;

void ClearIOReporting() {
  if (!sTelemetryIOObserver) {
    return;
  }
  IOInterposer::Unregister(IOInterposeObserver::OpAllWithStaging,
                           sTelemetryIOObserver);
  sTelemetryIOObserver = nullptr;
}

class TelemetryImpl final : public nsITelemetry, public nsIMemoryReporter {
  NS_DECL_THREADSAFE_ISUPPORTS
  NS_DECL_NSITELEMETRY
  NS_DECL_NSIMEMORYREPORTER

 public:
  void InitMemoryReporter();

  static already_AddRefed<nsITelemetry> CreateTelemetryInstance();
  static void ShutdownTelemetry();
  static void RecordSlowStatement(const nsACString& sql,
                                  const nsACString& dbName, uint32_t delay);
  struct Stat {
    uint32_t hitCount;
    uint32_t totalTime;
  };
  struct StmtStats {
    struct Stat mainThread;
    struct Stat otherThreads;
  };
  typedef nsBaseHashtableET<nsCStringHashKey, StmtStats> SlowSQLEntryType;

  static void RecordIceCandidates(const uint32_t iceCandidateBitmask,
                                  const bool success);
  static bool CanRecordBase();
  static bool CanRecordExtended();
  static bool CanRecordReleaseData();
  static bool CanRecordPrereleaseData();

 private:
  TelemetryImpl();
  ~TelemetryImpl();

  static nsCString SanitizeSQL(const nsACString& sql);

  enum SanitizedState { Sanitized, Unsanitized };

  static void StoreSlowSQL(const nsACString& offender, uint32_t delay,
                           SanitizedState state);

  static bool ReflectMainThreadSQL(SlowSQLEntryType* entry, JSContext* cx,
                                   JS::Handle<JSObject*> obj);
  static bool ReflectOtherThreadsSQL(SlowSQLEntryType* entry, JSContext* cx,
                                     JS::Handle<JSObject*> obj);
  static bool ReflectSQL(const SlowSQLEntryType* entry, const Stat* stat,
                         JSContext* cx, JS::Handle<JSObject*> obj);

  bool AddSQLInfo(JSContext* cx, JS::Handle<JSObject*> rootObj, bool mainThread,
                  bool privateSQL);
  bool GetSQLStats(JSContext* cx, JS::MutableHandle<JS::Value> ret,
                   bool includePrivateSql);

  static StaticDataMutex<TelemetryImpl*> sTelemetry;
  AutoHashtable<SlowSQLEntryType> mPrivateSQL;
  AutoHashtable<SlowSQLEntryType> mSanitizedSQL;
  Mutex mHashMutex MOZ_UNANNOTATED;
  Atomic<bool, SequentiallyConsistent> mCanRecordBase;
  Atomic<bool, SequentiallyConsistent> mCanRecordExtended;

  bool mCachedTelemetryData;
  uint32_t mLastShutdownTime;
  uint32_t mFailedLockCount;
  nsCOMArray<nsIFetchTelemetryDataCallback> mCallbacks;
  friend class nsFetchTelemetryData;
};

MOZ_RUNINIT StaticDataMutex<TelemetryImpl*> TelemetryImpl::sTelemetry(nullptr,
                                                                      nullptr);

MOZ_DEFINE_MALLOC_SIZE_OF(TelemetryMallocSizeOf)

NS_IMETHODIMP
TelemetryImpl::CollectReports(nsIHandleReportCallback* aHandleReport,
                              nsISupports* aData, bool aAnonymize) {
  mozilla::MallocSizeOf aMallocSizeOf = TelemetryMallocSizeOf;

#define COLLECT_REPORT(name, size, desc) \
  MOZ_COLLECT_REPORT(name, KIND_HEAP, UNITS_BYTES, size, desc)

  COLLECT_REPORT("explicit/telemetry/impl", aMallocSizeOf(this),
                 "Memory used by the Telemetry core implemenation");

  COLLECT_REPORT(
      "explicit/telemetry/scalar/shallow",
      TelemetryScalar::GetMapShallowSizesOfExcludingThis(aMallocSizeOf),
      "Memory used by the Telemetry Scalar implemenation");

  {  // Scope for mHashMutex lock
    MutexAutoLock lock(mHashMutex);
    COLLECT_REPORT("explicit/telemetry/PrivateSQL",
                   mPrivateSQL.SizeOfExcludingThis(aMallocSizeOf),
                   "Memory used by the PrivateSQL Telemetry");

    COLLECT_REPORT("explicit/telemetry/SanitizedSQL",
                   mSanitizedSQL.SizeOfExcludingThis(aMallocSizeOf),
                   "Memory used by the SanitizedSQL Telemetry");
  }

  if (sTelemetryIOObserver) {
    COLLECT_REPORT("explicit/telemetry/IOObserver",
                   sTelemetryIOObserver->SizeOfIncludingThis(aMallocSizeOf),
                   "Memory used by the Telemetry IO Observer");
  }

  COLLECT_REPORT("explicit/telemetry/Callbacks",
                 mCallbacks.ShallowSizeOfExcludingThis(aMallocSizeOf),
                 "Memory used by the Telemetry Callbacks array (shallow)");

  COLLECT_REPORT(
      "explicit/telemetry/histogram/data",
      TelemetryHistogram::GetHistogramSizesOfIncludingThis(aMallocSizeOf),
      "Memory used by Telemetry Histogram data");

  COLLECT_REPORT("explicit/telemetry/scalar/data",
                 TelemetryScalar::GetScalarSizesOfIncludingThis(aMallocSizeOf),
                 "Memory used by Telemetry Scalar data");

  COLLECT_REPORT("explicit/telemetry/event/data",
                 TelemetryEvent::SizeOfIncludingThis(aMallocSizeOf),
                 "Memory used by Telemetry Event data");

#undef COLLECT_REPORT

  return NS_OK;
}

void InitHistogramRecordingEnabled() {
  TelemetryHistogram::InitHistogramRecordingEnabled();
}

using PathChar = filesystem::Path::value_type;
using PathCharPtr = const PathChar*;

static uint32_t ReadLastShutdownDuration(PathCharPtr filename) {
  nsCOMPtr<nsIFile> file;
  if (NS_FAILED(NS_NewPathStringLocalFile(DependentPathString(filename),
                                          getter_AddRefs(file)))) {
    return 0;
  }
  FILE* f;
  if (NS_FAILED(file->OpenANSIFileDesc("r", &f)) || !f) {
    return 0;
  }

  int shutdownTime;
  int r = fscanf(f, "%d\n", &shutdownTime);
  fclose(f);
  if (r != 1) {
    return 0;
  }

  return shutdownTime;
}

const int32_t kMaxFailedProfileLockFileSize = 10;

bool GetFailedLockCount(nsIInputStream* inStream, uint32_t aCount,
                        unsigned int& result) {
  nsAutoCString bufStr;
  nsresult rv;
  rv = NS_ReadInputStreamToString(inStream, bufStr, aCount);
  NS_ENSURE_SUCCESS(rv, false);
  result = bufStr.ToInteger(&rv);
  return NS_SUCCEEDED(rv) && result > 0;
}

nsresult GetFailedProfileLockFile(nsIFile** aFile, nsIFile* aProfileDir) {
  NS_ENSURE_ARG_POINTER(aProfileDir);

  nsresult rv = aProfileDir->Clone(aFile);
  NS_ENSURE_SUCCESS(rv, rv);

  (*aFile)->AppendNative("Telemetry.FailedProfileLocks.txt"_ns);
  return NS_OK;
}

class nsFetchTelemetryData : public Runnable {
 public:
  nsFetchTelemetryData(PathCharPtr aShutdownTimeFilename,
                       nsIFile* aFailedProfileLockFile)
      : mozilla::Runnable("nsFetchTelemetryData"),
        mShutdownTimeFilename(aShutdownTimeFilename),
        mFailedProfileLockFile(aFailedProfileLockFile) {}

 private:
  PathCharPtr mShutdownTimeFilename;
  nsCOMPtr<nsIFile> mFailedProfileLockFile;

 public:
  void MainThread() {
    auto lock = TelemetryImpl::sTelemetry.Lock();
    auto telemetry = lock.ref();
    telemetry->mCachedTelemetryData = true;
    for (unsigned int i = 0, n = telemetry->mCallbacks.Count(); i < n; ++i) {
      telemetry->mCallbacks[i]->Complete();
    }
    telemetry->mCallbacks.Clear();
  }

  NS_IMETHOD Run() override {
    uint32_t failedLockCount = 0;
    uint32_t lastShutdownDuration = 0;
    LoadFailedLockCount(failedLockCount);
    lastShutdownDuration = ReadLastShutdownDuration(mShutdownTimeFilename);
    {
      auto lock = TelemetryImpl::sTelemetry.Lock();
      auto telemetry = lock.ref();
      telemetry->mFailedLockCount = failedLockCount;
      telemetry->mLastShutdownTime = lastShutdownDuration;
    }

    glean::browser_timings::last_shutdown.Set(lastShutdownDuration);
    glean::profile_lock::failed_lock_count.Set(failedLockCount);

    nsCOMPtr<nsIRunnable> e =
        NewRunnableMethod("nsFetchTelemetryData::MainThread", this,
                          &nsFetchTelemetryData::MainThread);
    NS_ENSURE_STATE(e);
    NS_DispatchToMainThread(e);
    return NS_OK;
  }

 private:
  nsresult LoadFailedLockCount(uint32_t& failedLockCount) {
    failedLockCount = 0;
    int64_t fileSize = 0;
    nsresult rv = mFailedProfileLockFile->GetFileSize(&fileSize);
    if (NS_FAILED(rv)) {
      return rv;
    }
    NS_ENSURE_TRUE(fileSize <= kMaxFailedProfileLockFileSize,
                   NS_ERROR_UNEXPECTED);
    nsCOMPtr<nsIInputStream> inStream;
    rv = NS_NewLocalFileInputStream(getter_AddRefs(inStream),
                                    mFailedProfileLockFile, PR_RDONLY);
    NS_ENSURE_SUCCESS(rv, rv);
    NS_ENSURE_TRUE(GetFailedLockCount(inStream, fileSize, failedLockCount),
                   NS_ERROR_UNEXPECTED);
    inStream->Close();

    mFailedProfileLockFile->Remove(false);
    return NS_OK;
  }
};

static TimeStamp gRecordedShutdownStartTime;
static bool gAlreadyFreedShutdownTimeFileName = false;
static PathCharPtr gRecordedShutdownTimeFileName = nullptr;

static PathCharPtr GetShutdownTimeFileName() {
  if (gAlreadyFreedShutdownTimeFileName) {
    return nullptr;
  }

  if (!gRecordedShutdownTimeFileName) {
    nsCOMPtr<nsIFile> mozFile;
    NS_GetSpecialDirectory(NS_APP_USER_PROFILE_50_DIR, getter_AddRefs(mozFile));
    if (!mozFile) return nullptr;

    mozFile->AppendNative("Telemetry.ShutdownTime.txt"_ns);

    gRecordedShutdownTimeFileName = NS_xstrdup(mozFile->NativePath().get());
  }

  return gRecordedShutdownTimeFileName;
}

NS_IMETHODIMP
TelemetryImpl::GetLastShutdownDuration(uint32_t* aResult) {
  // The user must call AsyncFetchTelemetryData first. We return zero instead of
  // reporting a failure so that the rest of telemetry can uniformly handle
  // the read not being available yet.
  if (!mCachedTelemetryData) {
    *aResult = 0;
    return NS_OK;
  }

  *aResult = mLastShutdownTime;
  return NS_OK;
}

NS_IMETHODIMP
TelemetryImpl::GetFailedProfileLockCount(uint32_t* aResult) {
  // The user must call AsyncFetchTelemetryData first. We return zero instead of
  // reporting a failure so that the rest of telemetry can uniformly handle
  // the read not being available yet.
  if (!mCachedTelemetryData) {
    *aResult = 0;
    return NS_OK;
  }

  *aResult = mFailedLockCount;
  return NS_OK;
}

NS_IMETHODIMP
TelemetryImpl::AsyncFetchTelemetryData(
    nsIFetchTelemetryDataCallback* aCallback) {
  // We have finished reading the data already, just call the callback.
  if (mCachedTelemetryData) {
    aCallback->Complete();
    return NS_OK;
  }

  // We already have a read request running, just remember the callback.
  if (mCallbacks.Count() != 0) {
    mCallbacks.AppendObject(aCallback);
    return NS_OK;
  }

  // We make this check so that GetShutdownTimeFileName() doesn't get
  // called; calling that function without telemetry enabled violates
  // assumptions that the write-the-shutdown-timestamp machinery makes.
  if (!Telemetry::CanRecordExtended()) {
    mCachedTelemetryData = true;
    aCallback->Complete();
    return NS_OK;
  }

  // Send the read to a background thread provided by the stream transport
  // service to avoid a read in the main thread.
  nsCOMPtr<nsIEventTarget> targetThread =
      do_GetService(NS_STREAMTRANSPORTSERVICE_CONTRACTID);
  if (!targetThread) {
    mCachedTelemetryData = true;
    aCallback->Complete();
    return NS_OK;
  }

  // We have to get the filename from the main thread.
  PathCharPtr shutdownTimeFilename = GetShutdownTimeFileName();
  if (!shutdownTimeFilename) {
    mCachedTelemetryData = true;
    aCallback->Complete();
    return NS_OK;
  }

  nsCOMPtr<nsIFile> profileDir;
  nsresult rv = NS_GetSpecialDirectory(NS_APP_USER_PROFILE_50_DIR,
                                       getter_AddRefs(profileDir));
  if (NS_FAILED(rv)) {
    mCachedTelemetryData = true;
    aCallback->Complete();
    return NS_OK;
  }

  nsCOMPtr<nsIFile> failedProfileLockFile;
  rv = GetFailedProfileLockFile(getter_AddRefs(failedProfileLockFile),
                                profileDir);
  if (NS_FAILED(rv)) {
    mCachedTelemetryData = true;
    aCallback->Complete();
    return NS_OK;
  }

  mCallbacks.AppendObject(aCallback);

  nsCOMPtr<nsIRunnable> event =
      new nsFetchTelemetryData(shutdownTimeFilename, failedProfileLockFile);

  targetThread->Dispatch(event, NS_DISPATCH_NORMAL);
  return NS_OK;
}

TelemetryImpl::TelemetryImpl()
    : mHashMutex("Telemetry::mHashMutex"),
      mCanRecordBase(false),
      mCanRecordExtended(false),
      mCachedTelemetryData(false),
      mLastShutdownTime(0),
      mFailedLockCount(0) {
  // We expect TelemetryHistogram::InitializeGlobalState() to have been
  // called before we get to this point.
  MOZ_ASSERT(TelemetryHistogram::GlobalStateHasBeenInitialized());
}

TelemetryImpl::~TelemetryImpl() {
  UnregisterWeakMemoryReporter(this);

  // This is still racey as access to these collections is guarded using
  // sTelemetry. We will fix this in bug 1367344.
  MutexAutoLock hashLock(mHashMutex);
}

void TelemetryImpl::InitMemoryReporter() { RegisterWeakMemoryReporter(this); }

bool TelemetryImpl::ReflectSQL(const SlowSQLEntryType* entry, const Stat* stat,
                               JSContext* cx, JS::Handle<JSObject*> obj) {
  if (stat->hitCount == 0) return true;

  const nsACString& sql = entry->GetKey();

  JS::Rooted<JSObject*> arrayObj(cx, JS::NewArrayObject(cx, 0));
  if (!arrayObj) {
    return false;
  }
  return (
      JS_DefineElement(cx, arrayObj, 0, stat->hitCount, JSPROP_ENUMERATE) &&
      JS_DefineElement(cx, arrayObj, 1, stat->totalTime, JSPROP_ENUMERATE) &&
      JS_DefineProperty(cx, obj, sql.BeginReading(), arrayObj,
                        JSPROP_ENUMERATE));
}

bool TelemetryImpl::ReflectMainThreadSQL(SlowSQLEntryType* entry, JSContext* cx,
                                         JS::Handle<JSObject*> obj) {
  return ReflectSQL(entry, &entry->GetModifiableData()->mainThread, cx, obj);
}

bool TelemetryImpl::ReflectOtherThreadsSQL(SlowSQLEntryType* entry,
                                           JSContext* cx,
                                           JS::Handle<JSObject*> obj) {
  return ReflectSQL(entry, &entry->GetModifiableData()->otherThreads, cx, obj);
}

bool TelemetryImpl::AddSQLInfo(JSContext* cx, JS::Handle<JSObject*> rootObj,
                               bool mainThread, bool privateSQL) {
  JS::Rooted<JSObject*> statsObj(cx, JS_NewPlainObject(cx));
  if (!statsObj) return false;

  AutoHashtable<SlowSQLEntryType>& sqlMap =
      (privateSQL ? mPrivateSQL : mSanitizedSQL);
  AutoHashtable<SlowSQLEntryType>::ReflectEntryFunc reflectFunction =
      (mainThread ? ReflectMainThreadSQL : ReflectOtherThreadsSQL);
  if (!sqlMap.ReflectIntoJS(reflectFunction, cx, statsObj)) {
    return false;
  }

  return JS_DefineProperty(cx, rootObj,
                           mainThread ? "mainThread" : "otherThreads", statsObj,
                           JSPROP_ENUMERATE);
}

NS_IMETHODIMP
TelemetryImpl::GetSnapshotForHistograms(const nsACString& aStoreName,
                                        bool aClearStore, bool aFilterTest,
                                        JSContext* aCx,
                                        JS::MutableHandle<JS::Value> aResult) {
  constexpr auto defaultStore = "main"_ns;
  unsigned int dataset = mCanRecordExtended
                             ? nsITelemetry::DATASET_PRERELEASE_CHANNELS
                             : nsITelemetry::DATASET_ALL_CHANNELS;
  return TelemetryHistogram::CreateHistogramSnapshots(
      aCx, aResult, aStoreName.IsVoid() ? defaultStore : aStoreName, dataset,
      aClearStore, aFilterTest);
}

NS_IMETHODIMP
TelemetryImpl::GetSnapshotForKeyedHistograms(
    const nsACString& aStoreName, bool aClearStore, bool aFilterTest,
    JSContext* aCx, JS::MutableHandle<JS::Value> aResult) {
  constexpr auto defaultStore = "main"_ns;
  unsigned int dataset = mCanRecordExtended
                             ? nsITelemetry::DATASET_PRERELEASE_CHANNELS
                             : nsITelemetry::DATASET_ALL_CHANNELS;
  return TelemetryHistogram::GetKeyedHistogramSnapshots(
      aCx, aResult, aStoreName.IsVoid() ? defaultStore : aStoreName, dataset,
      aClearStore, aFilterTest);
}

NS_IMETHODIMP
TelemetryImpl::GetCategoricalLabels(JSContext* aCx,
                                    JS::MutableHandle<JS::Value> aResult) {
  return TelemetryHistogram::GetCategoricalHistogramLabels(aCx, aResult);
}

NS_IMETHODIMP
TelemetryImpl::GetSnapshotForScalars(const nsACString& aStoreName,
                                     bool aClearStore, bool aFilterTest,
                                     JSContext* aCx,
                                     JS::MutableHandle<JS::Value> aResult) {
  constexpr auto defaultStore = "main"_ns;
  unsigned int dataset = mCanRecordExtended
                             ? nsITelemetry::DATASET_PRERELEASE_CHANNELS
                             : nsITelemetry::DATASET_ALL_CHANNELS;
  return TelemetryScalar::CreateSnapshots(
      dataset, aClearStore, aCx, 1, aResult, aFilterTest,
      aStoreName.IsVoid() ? defaultStore : aStoreName);
}

NS_IMETHODIMP
TelemetryImpl::GetSnapshotForKeyedScalars(
    const nsACString& aStoreName, bool aClearStore, bool aFilterTest,
    JSContext* aCx, JS::MutableHandle<JS::Value> aResult) {
  constexpr auto defaultStore = "main"_ns;
  unsigned int dataset = mCanRecordExtended
                             ? nsITelemetry::DATASET_PRERELEASE_CHANNELS
                             : nsITelemetry::DATASET_ALL_CHANNELS;
  return TelemetryScalar::CreateKeyedSnapshots(
      dataset, aClearStore, aCx, 1, aResult, aFilterTest,
      aStoreName.IsVoid() ? defaultStore : aStoreName);
}

bool TelemetryImpl::GetSQLStats(JSContext* cx, JS::MutableHandle<JS::Value> ret,
                                bool includePrivateSql) {
  JS::Rooted<JSObject*> root_obj(cx, JS_NewPlainObject(cx));
  if (!root_obj) return false;
  ret.setObject(*root_obj);

  MutexAutoLock hashMutex(mHashMutex);
  // Add info about slow SQL queries on the main thread
  if (!AddSQLInfo(cx, root_obj, true, includePrivateSql)) return false;
  // Add info about slow SQL queries on other threads
  if (!AddSQLInfo(cx, root_obj, false, includePrivateSql)) return false;

  return true;
}

NS_IMETHODIMP
TelemetryImpl::GetSlowSQL(JSContext* cx, JS::MutableHandle<JS::Value> ret) {
  if (GetSQLStats(cx, ret, false)) return NS_OK;
  return NS_ERROR_FAILURE;
}

NS_IMETHODIMP
TelemetryImpl::GetDebugSlowSQL(JSContext* cx,
                               JS::MutableHandle<JS::Value> ret) {
  bool revealPrivateSql =
      Preferences::GetBool("toolkit.telemetry.debugSlowSql", false);
  if (GetSQLStats(cx, ret, revealPrivateSql)) return NS_OK;
  return NS_ERROR_FAILURE;
}

NS_IMETHODIMP
TelemetryImpl::SubmitAndGetUntrustedModulePayload(JSContext* aCx,
                                                  Promise** aPromise) {
#if defined(XP_WIN)
  return Telemetry::SubmitAndGetUntrustedModulePayload(aCx, aPromise);
#else
  return NS_ERROR_NOT_IMPLEMENTED;
#endif
}

NS_IMETHODIMP
TelemetryImpl::GetUntrustedModuleLoadEvents(uint32_t aFlags, JSContext* cx,
                                            Promise** aPromise) {
#if defined(XP_WIN)
  return Telemetry::GetUntrustedModuleLoadEvents(aFlags, cx, aPromise);
#else
  return NS_ERROR_NOT_IMPLEMENTED;
#endif
}

NS_IMETHODIMP
TelemetryImpl::GetAreUntrustedModuleLoadEventsReady(bool* ret) {
#if defined(XP_WIN)
  *ret = DllServices::Get()->IsReadyForBackgroundProcessing();
  return NS_OK;
#else
  return NS_ERROR_NOT_IMPLEMENTED;
#endif
}

NS_IMETHODIMP
TelemetryImpl::GetHistogramById(const nsACString& name, JSContext* cx,
                                JS::MutableHandle<JS::Value> ret) {
  return TelemetryHistogram::GetHistogramById(name, cx, ret);
}

NS_IMETHODIMP
TelemetryImpl::GetKeyedHistogramById(const nsACString& name, JSContext* cx,
                                     JS::MutableHandle<JS::Value> ret) {
  return TelemetryHistogram::GetKeyedHistogramById(name, cx, ret);
}

/**
 * Indicates if Telemetry can record base data (FHR data). This is true if the
 * FHR data reporting service or self-support are enabled.
 *
 * In the unlikely event that adding a new base probe is needed, please check
 * the data collection wiki at https://wiki.mozilla.org/Firefox/Data_Collection
 * and talk to the Telemetry team.
 */
NS_IMETHODIMP
TelemetryImpl::GetCanRecordBase(bool* ret) {
  *ret = mCanRecordBase;
  return NS_OK;
}

NS_IMETHODIMP
TelemetryImpl::SetCanRecordBase(bool canRecord) {
#ifndef FUZZING
  if (canRecord != mCanRecordBase) {
    TelemetryHistogram::SetCanRecordBase(canRecord);
    TelemetryScalar::SetCanRecordBase(canRecord);
    TelemetryEvent::SetCanRecordBase(canRecord);
    mCanRecordBase = canRecord;
  }
#endif
  return NS_OK;
}

/**
 * Indicates if Telemetry is allowed to record extended data. Returns false if
 * the user hasn't opted into "extended Telemetry" on the Release channel, when
 * the user has explicitly opted out of Telemetry on Nightly/Aurora/Beta or if
 * manually set to false during tests. If the returned value is false, gathering
 * of extended telemetry statistics is disabled.
 */
NS_IMETHODIMP
TelemetryImpl::GetCanRecordExtended(bool* ret) {
  *ret = mCanRecordExtended;
  return NS_OK;
}

NS_IMETHODIMP
TelemetryImpl::SetCanRecordExtended(bool canRecord) {
#ifndef FUZZING
  if (canRecord != mCanRecordExtended) {
    TelemetryHistogram::SetCanRecordExtended(canRecord);
    TelemetryScalar::SetCanRecordExtended(canRecord);
    TelemetryEvent::SetCanRecordExtended(canRecord);
    mCanRecordExtended = canRecord;
  }
#endif
  return NS_OK;
}

NS_IMETHODIMP
TelemetryImpl::GetCanRecordReleaseData(bool* ret) {
  *ret = mCanRecordBase;
  return NS_OK;
}

NS_IMETHODIMP
TelemetryImpl::GetCanRecordPrereleaseData(bool* ret) {
  *ret = mCanRecordExtended;
  return NS_OK;
}

NS_IMETHODIMP
TelemetryImpl::GetIsOfficialTelemetry(bool* ret) {
#if defined(MOZILLA_OFFICIAL) && defined(MOZ_TELEMETRY_REPORTING) && \
    !defined(DEBUG)
  *ret = true;
#else
  *ret = false;
#endif
  return NS_OK;
}

already_AddRefed<nsITelemetry> TelemetryImpl::CreateTelemetryInstance() {
  {
    auto lock = sTelemetry.Lock();
    MOZ_ASSERT(
        *lock == nullptr,
        "CreateTelemetryInstance may only be called once, via GetService()");
  }

  bool useTelemetry = false;
#ifndef FUZZING
  if (XRE_IsParentProcess() || XRE_IsContentProcess() || XRE_IsGPUProcess() ||
      XRE_IsRDDProcess() || XRE_IsSocketProcess() || XRE_IsUtilityProcess()) {
    useTelemetry = true;
  }
#endif
#ifdef MOZ_BACKGROUNDTASKS
  if (BackgroundTasks::IsBackgroundTaskMode()) {
    // Background tasks collect per-task metrics with Glean.
    useTelemetry = false;
  }
#endif

  // First, initialize the TelemetryHistogram and TelemetryScalar global states.
  TelemetryHistogram::InitializeGlobalState(useTelemetry, useTelemetry);
  TelemetryScalar::InitializeGlobalState(useTelemetry, useTelemetry);

  // Only record events from the parent process.
  TelemetryEvent::InitializeGlobalState(XRE_IsParentProcess(),
                                        XRE_IsParentProcess());

  // Currently, only UserInteractions from the parent process are recorded.
  TelemetryUserInteraction::InitializeGlobalState(useTelemetry);

  // Now, create and initialize the Telemetry global state.
  TelemetryImpl* telemetry = new TelemetryImpl();
  {
    auto lock = sTelemetry.Lock();
    *lock = telemetry;
    // AddRef for the local reference before releasing the lock.
    NS_ADDREF(telemetry);
  }

  // AddRef for the caller
  nsCOMPtr<nsITelemetry> ret = telemetry;

  telemetry->mCanRecordBase = useTelemetry;
  telemetry->mCanRecordExtended = useTelemetry;

  telemetry->InitMemoryReporter();
  InitHistogramRecordingEnabled();  // requires sTelemetry to exist

  return ret.forget();
}

void TelemetryImpl::ShutdownTelemetry() {
  // No point in collecting IO beyond this point
  ClearIOReporting();
  {
    auto lock = sTelemetry.Lock();
    NS_IF_RELEASE(lock.ref());
  }

  // Lastly, de-initialise the TelemetryHistogram and TelemetryScalar global
  // states, so as to release any heap storage that would otherwise be kept
  // alive by it.
  TelemetryHistogram::DeInitializeGlobalState();
  TelemetryScalar::DeInitializeGlobalState();
  TelemetryEvent::DeInitializeGlobalState();

  TelemetryUserInteraction::DeInitializeGlobalState();
  TelemetryIPCAccumulator::DeInitializeGlobalState();
}

void TelemetryImpl::StoreSlowSQL(const nsACString& sql, uint32_t delay,
                                 SanitizedState state) {
  auto lock = sTelemetry.Lock();
  auto telemetry = lock.ref();
  AutoHashtable<SlowSQLEntryType>* slowSQLMap = nullptr;
  if (state == Sanitized)
    slowSQLMap = &(telemetry->mSanitizedSQL);
  else
    slowSQLMap = &(telemetry->mPrivateSQL);

  MutexAutoLock hashMutex(telemetry->mHashMutex);

  SlowSQLEntryType* entry = slowSQLMap->GetEntry(sql);
  if (!entry) {
    entry = slowSQLMap->PutEntry(sql);
    if (MOZ_UNLIKELY(!entry)) return;
    entry->GetModifiableData()->mainThread.hitCount = 0;
    entry->GetModifiableData()->mainThread.totalTime = 0;
    entry->GetModifiableData()->otherThreads.hitCount = 0;
    entry->GetModifiableData()->otherThreads.totalTime = 0;
  }

  if (NS_IsMainThread()) {
    entry->GetModifiableData()->mainThread.hitCount++;
    entry->GetModifiableData()->mainThread.totalTime += delay;
  } else {
    entry->GetModifiableData()->otherThreads.hitCount++;
    entry->GetModifiableData()->otherThreads.totalTime += delay;
  }
}

/**
 * This method replaces string literals in SQL strings with the word :private
 *
 * States used in this state machine:
 *
 * NORMAL:
 *  - This is the active state when not iterating over a string literal or
 *  comment
 *
 * SINGLE_QUOTE:
 *  - Defined here: http://www.sqlite.org/lang_expr.html
 *  - This state represents iterating over a string literal opened with
 *  a single quote.
 *  - A single quote within the string can be encoded by putting 2 single quotes
 *  in a row, e.g. 'This literal contains an escaped quote '''
 *  - Any double quotes found within a single-quoted literal are ignored
 *  - This state covers BLOB literals, e.g. X'ABC123'
 *  - The string literal and the enclosing quotes will be replaced with
 *  the text :private
 *
 * DOUBLE_QUOTE:
 *  - Same rules as the SINGLE_QUOTE state.
 *  - According to http://www.sqlite.org/lang_keywords.html,
 *  SQLite interprets text in double quotes as an identifier unless it's used in
 *  a context where it cannot be resolved to an identifier and a string literal
 *  is allowed. This method removes text in double-quotes for safety.
 *
 *  DASH_COMMENT:
 *  - http://www.sqlite.org/lang_comment.html
 *  - A dash comment starts with two dashes in a row,
 *  e.g. DROP TABLE foo -- a comment
 *  - Any text following two dashes in a row is interpreted as a comment until
 *  end of input or a newline character
 *  - Any quotes found within the comment are ignored and no replacements made
 *
 *  C_STYLE_COMMENT:
 *  - http://www.sqlite.org/lang_comment.html
 *  - A C-style comment starts with a forward slash and an asterisk, and ends
 *  with an asterisk and a forward slash
 *  - Any text following comment start is interpreted as a comment up to end of
 *  input or comment end
 *  - Any quotes found within the comment are ignored and no replacements made
 */
nsCString TelemetryImpl::SanitizeSQL(const nsACString& sql) {
  nsCString output;
  int length = sql.Length();

  typedef enum {
    NORMAL,
    SINGLE_QUOTE,
    DOUBLE_QUOTE,
    DASH_COMMENT,
    C_STYLE_COMMENT,
  } State;

  State state = NORMAL;
  int fragmentStart = 0;
  for (int i = 0; i < length; i++) {
    char character = sql[i];
    char nextCharacter = (i + 1 < length) ? sql[i + 1] : '\0';

    switch (character) {
      case '\'':
      case '"':
        if (state == NORMAL) {
          state = (character == '\'') ? SINGLE_QUOTE : DOUBLE_QUOTE;
          output +=
              nsDependentCSubstring(sql, fragmentStart, i - fragmentStart);
          output += ":private";
          fragmentStart = -1;
        } else if ((state == SINGLE_QUOTE && character == '\'') ||
                   (state == DOUBLE_QUOTE && character == '"')) {
          if (nextCharacter == character) {
            // Two consecutive quotes within a string literal are a single
            // escaped quote
            i++;
          } else {
            state = NORMAL;
            fragmentStart = i + 1;
          }
        }
        break;
      case '-':
        if (state == NORMAL) {
          if (nextCharacter == '-') {
            state = DASH_COMMENT;
            i++;
          }
        }
        break;
      case '\n':
        if (state == DASH_COMMENT) {
          state = NORMAL;
        }
        break;
      case '/':
        if (state == NORMAL) {
          if (nextCharacter == '*') {
            state = C_STYLE_COMMENT;
            i++;
          }
        }
        break;
      case '*':
        if (state == C_STYLE_COMMENT) {
          if (nextCharacter == '/') {
            state = NORMAL;
          }
        }
        break;
      default:
        continue;
    }
  }

  if ((fragmentStart >= 0) && fragmentStart < length)
    output += nsDependentCSubstring(sql, fragmentStart, length - fragmentStart);

  return output;
}

// An allowlist mechanism to prevent Telemetry reporting on Addon & Thunderbird
// DBs.
struct TrackedDBEntry {
  const char* mName;
  const uint32_t mNameLength;

  // This struct isn't meant to be used beyond the static arrays below.
  constexpr TrackedDBEntry(const char* aName, uint32_t aNameLength)
      : mName(aName), mNameLength(aNameLength) {}

  TrackedDBEntry() = delete;
  TrackedDBEntry(TrackedDBEntry&) = delete;
};

#define TRACKEDDB_ENTRY(_name) {_name, (sizeof(_name) - 1)}

// An allowlist of database names. If the database name exactly matches one of
// these then its SQL statements will always be recorded.
static constexpr TrackedDBEntry kTrackedDBs[] = {
    // IndexedDB for about:home, see aboutHome.js
    TRACKEDDB_ENTRY("818200132aebmoouht.sqlite"),
    TRACKEDDB_ENTRY("addons.sqlite"),
    TRACKEDDB_ENTRY("content-prefs.sqlite"),
    TRACKEDDB_ENTRY("cookies.sqlite"),
    TRACKEDDB_ENTRY("extensions.sqlite"),
    TRACKEDDB_ENTRY("favicons.sqlite"),
    TRACKEDDB_ENTRY("formhistory.sqlite"),
    TRACKEDDB_ENTRY("index.sqlite"),
    TRACKEDDB_ENTRY("netpredictions.sqlite"),
    TRACKEDDB_ENTRY("permissions.sqlite"),
    TRACKEDDB_ENTRY("places.sqlite"),
    TRACKEDDB_ENTRY("reading-list.sqlite"),
    TRACKEDDB_ENTRY("search.sqlite"),
    TRACKEDDB_ENTRY("urlclassifier3.sqlite"),
    TRACKEDDB_ENTRY("webappsstore.sqlite")};

// An allowlist of database name prefixes. If the database name begins with
// one of these prefixes then its SQL statements will always be recorded.
static const TrackedDBEntry kTrackedDBPrefixes[] = {
    TRACKEDDB_ENTRY("indexedDB-")};

#undef TRACKEDDB_ENTRY

// Slow SQL statements will be automatically
// trimmed to kMaxSlowStatementLength characters.
// This limit doesn't include the ellipsis and DB name,
// that are appended at the end of the stored statement.
const uint32_t kMaxSlowStatementLength = 1000;

void TelemetryImpl::RecordSlowStatement(const nsACString& sql,
                                        const nsACString& dbName,
                                        uint32_t delay) {
  MOZ_ASSERT(!sql.IsEmpty());
  MOZ_ASSERT(!dbName.IsEmpty());

  {
    auto lock = sTelemetry.Lock();
    if (!lock.ref() || !TelemetryHistogram::CanRecordExtended()) {
      return;
    }
  }

  bool recordStatement = false;

  for (const TrackedDBEntry& nameEntry : kTrackedDBs) {
    MOZ_ASSERT(nameEntry.mNameLength);
    const nsDependentCString name(nameEntry.mName, nameEntry.mNameLength);
    if (dbName == name) {
      recordStatement = true;
      break;
    }
  }

  if (!recordStatement) {
    for (const TrackedDBEntry& prefixEntry : kTrackedDBPrefixes) {
      MOZ_ASSERT(prefixEntry.mNameLength);
      const nsDependentCString prefix(prefixEntry.mName,
                                      prefixEntry.mNameLength);
      if (StringBeginsWith(dbName, prefix)) {
        recordStatement = true;
        break;
      }
    }
  }

  if (recordStatement) {
    nsAutoCString sanitizedSQL(SanitizeSQL(sql));
    if (sanitizedSQL.Length() > kMaxSlowStatementLength) {
      sanitizedSQL.SetLength(kMaxSlowStatementLength);
      sanitizedSQL += "...";
    }
    sanitizedSQL.AppendPrintf(" /* %s */", nsPromiseFlatCString(dbName).get());
    StoreSlowSQL(sanitizedSQL, delay, Sanitized);
  } else {
    // Report aggregate DB-level statistics for addon DBs
    nsAutoCString aggregate;
    aggregate.AppendPrintf("Untracked SQL for %s",
                           nsPromiseFlatCString(dbName).get());
    StoreSlowSQL(aggregate, delay, Sanitized);
  }

  nsAutoCString fullSQL;
  fullSQL.AppendPrintf("%s /* %s */", nsPromiseFlatCString(sql).get(),
                       nsPromiseFlatCString(dbName).get());
  StoreSlowSQL(fullSQL, delay, Unsanitized);
}

bool TelemetryImpl::CanRecordBase() {
  auto lock = sTelemetry.Lock();
  auto telemetry = lock.ref();
  if (!telemetry) {
    return false;
  }
  bool canRecordBase;
  nsresult rv = telemetry->GetCanRecordBase(&canRecordBase);
  return NS_SUCCEEDED(rv) && canRecordBase;
}

bool TelemetryImpl::CanRecordExtended() {
  auto lock = sTelemetry.Lock();
  auto telemetry = lock.ref();
  if (!telemetry) {
    return false;
  }
  bool canRecordExtended;
  nsresult rv = telemetry->GetCanRecordExtended(&canRecordExtended);
  return NS_SUCCEEDED(rv) && canRecordExtended;
}

bool TelemetryImpl::CanRecordReleaseData() { return CanRecordBase(); }

bool TelemetryImpl::CanRecordPrereleaseData() { return CanRecordExtended(); }

NS_IMPL_ISUPPORTS(TelemetryImpl, nsITelemetry, nsIMemoryReporter)

NS_IMETHODIMP
TelemetryImpl::GetFileIOReports(JSContext* cx,
                                JS::MutableHandle<JS::Value> ret) {
  if (sTelemetryIOObserver) {
    JS::Rooted<JSObject*> obj(cx, JS_NewPlainObject(cx));
    if (!obj) {
      return NS_ERROR_FAILURE;
    }

    if (!sTelemetryIOObserver->ReflectIntoJS(cx, obj)) {
      return NS_ERROR_FAILURE;
    }
    ret.setObject(*obj);
    return NS_OK;
  }
  ret.setNull();
  return NS_OK;
}

NS_IMETHODIMP
TelemetryImpl::MsSinceProcessStart(double* aResult) {
  return Telemetry::Common::MsSinceProcessStart(aResult);
}

NS_IMETHODIMP
TelemetryImpl::MsSinceProcessStartIncludingSuspend(double* aResult) {
  return Telemetry::Common::MsSinceProcessStartIncludingSuspend(aResult);
}

NS_IMETHODIMP
TelemetryImpl::MsSinceProcessStartExcludingSuspend(double* aResult) {
  return Telemetry::Common::MsSinceProcessStartExcludingSuspend(aResult);
}

NS_IMETHODIMP
TelemetryImpl::MsSystemNow(double* aResult) {
#if defined(XP_UNIX) && !defined(XP_DARWIN)
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  *aResult = ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
#else
  using namespace std::chrono;
  milliseconds ms =
      duration_cast<milliseconds>(system_clock::now().time_since_epoch());
  *aResult = static_cast<double>(ms.count());
#endif  // XP_UNIX && !XP_DARWIN

  return NS_OK;
}

// Telemetry Scalars IDL Implementation

NS_IMETHODIMP
TelemetryImpl::RegisterBuiltinScalars(const nsACString& aCategoryName,
                                      JS::Handle<JS::Value> aScalarData,
                                      JSContext* cx) {
  return TelemetryScalar::RegisterScalars(aCategoryName, aScalarData, cx);
}

NS_IMETHODIMP
TelemetryImpl::ClearScalars() {
  TelemetryScalar::ClearScalars();
  return NS_OK;
}

// Telemetry Event IDL implementation.

NS_IMETHODIMP
TelemetryImpl::SnapshotEvents(uint32_t aDataset, bool aClear,
                              uint32_t aEventLimit, JSContext* aCx,
                              uint8_t optional_argc,
                              JS::MutableHandle<JS::Value> aResult) {
  return TelemetryEvent::CreateSnapshots(aDataset, aClear, aEventLimit, aCx,
                                         optional_argc, aResult);
}

NS_IMETHODIMP
TelemetryImpl::RegisterBuiltinEvents(const nsACString& aCategory,
                                     JS::Handle<JS::Value> aEventData,
                                     JSContext* cx) {
  return TelemetryEvent::RegisterBuiltinEvents(aCategory, aEventData, cx);
}

NS_IMETHODIMP
TelemetryImpl::ClearEvents() {
  TelemetryEvent::ClearEvents();
  return NS_OK;
}

NS_IMETHODIMP
TelemetryImpl::FlushBatchedChildTelemetry() {
  TelemetryIPCAccumulator::IPCTimerFired(nullptr, nullptr);
  return NS_OK;
}

NS_IMETHODIMP
TelemetryImpl::EarlyInit() {
  Unused << MemoryTelemetry::Get();

  return NS_OK;
}

NS_IMETHODIMP
TelemetryImpl::DelayedInit() {
  MemoryTelemetry::Get().DelayedInit();
  return NS_OK;
}

NS_IMETHODIMP
TelemetryImpl::Shutdown() {
  MemoryTelemetry::Get().Shutdown();
  return NS_OK;
}

NS_IMETHODIMP
TelemetryImpl::GatherMemory(JSContext* aCx, Promise** aResult) {
  ErrorResult rv;
  RefPtr<Promise> promise = Promise::Create(xpc::CurrentNativeGlobal(aCx), rv);
  if (rv.Failed()) {
    return rv.StealNSResult();
  }

  MemoryTelemetry::Get().GatherReports(
      [promise]() { promise->MaybeResolve(JS::UndefinedHandleValue); });

  promise.forget(aResult);
  return NS_OK;
}

NS_IMETHODIMP
TelemetryImpl::GetAllStores(JSContext* aCx,
                            JS::MutableHandle<JS::Value> aResult) {
  StringHashSet stores;
  nsresult rv;

  rv = TelemetryHistogram::GetAllStores(stores);
  if (NS_FAILED(rv)) {
    return rv;
  }
  rv = TelemetryScalar::GetAllStores(stores);
  if (NS_FAILED(rv)) {
    return rv;
  }

  JS::RootedVector<JS::Value> allStores(aCx);
  if (!allStores.reserve(stores.Count())) {
    return NS_ERROR_FAILURE;
  }

  for (const auto& value : stores) {
    JS::Rooted<JS::Value> store(aCx);

    store.setString(ToJSString(aCx, value));
    if (!allStores.append(store)) {
      return NS_ERROR_FAILURE;
    }
  }

  JS::Rooted<JSObject*> rarray(aCx, JS::NewArrayObject(aCx, allStores));
  if (rarray == nullptr) {
    return NS_ERROR_FAILURE;
  }
  aResult.setObject(*rarray);

  return NS_OK;
}

}  // namespace

////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////
//
// EXTERNALLY VISIBLE FUNCTIONS in mozilla::
// These are NOT listed in Telemetry.h

namespace mozilla {

void RecordShutdownStartTimeStamp() {
#ifdef DEBUG
  // FIXME: this function should only be called once, since it should be called
  // at the earliest point we *know* we are shutting down. Unfortunately
  // this assert has been firing. Given that if we are called multiple times
  // we just keep the last timestamp, the assert is commented for now.
  static bool recorded = false;
  //  MOZ_ASSERT(!recorded);
  (void)
      recorded;  // Silence unused-var warnings (remove when assert re-enabled)
  recorded = true;
#endif

  if (!Telemetry::CanRecordExtended()) return;

  gRecordedShutdownStartTime = TimeStamp::Now();

  GetShutdownTimeFileName();
}

void RecordShutdownEndTimeStamp() {
  if (!gRecordedShutdownTimeFileName || gAlreadyFreedShutdownTimeFileName)
    return;

  PathString name(gRecordedShutdownTimeFileName);
  free(const_cast<PathChar*>(gRecordedShutdownTimeFileName));
  gRecordedShutdownTimeFileName = nullptr;
  gAlreadyFreedShutdownTimeFileName = true;

  if (gRecordedShutdownStartTime.IsNull()) {
    // If |CanRecordExtended()| is true before |AsyncFetchTelemetryData| is
    // called and then disabled before shutdown, |RecordShutdownStartTimeStamp|
    // will bail out and we will end up with a null |gRecordedShutdownStartTime|
    // here. This can happen during tests.
    return;
  }

  AutoPathString tmpName(name);
  tmpName.AppendLiteral(".tmp");
  nsCOMPtr<nsIFile> tmpFile;
  if (NS_FAILED(NS_NewPathStringLocalFile(tmpName, getter_AddRefs(tmpFile)))) {
    return;
  }
  FILE* f;
  if (NS_FAILED(tmpFile->OpenANSIFileDesc("w", &f)) || !f) return;
  // On a normal release build this should be called just before
  // calling _exit, but on a debug build or when the user forces a full
  // shutdown this is called as late as possible, so we have to
  // allow this write as write poisoning will be enabled.
  MozillaRegisterDebugFILE(f);

  TimeStamp now = TimeStamp::Now();
  MOZ_ASSERT(now >= gRecordedShutdownStartTime);
  TimeDuration diff = now - gRecordedShutdownStartTime;
  uint32_t diff2 = diff.ToMilliseconds();
  int written = fprintf(f, "%d\n", diff2);
  MozillaUnRegisterDebugFILE(f);
  int rv = fclose(f);
  if (written < 0 || rv != 0) {
    tmpFile->Remove(false);
    return;
  }
  nsCOMPtr<nsIFile> file;
  if (NS_FAILED(NS_NewPathStringLocalFile(name, getter_AddRefs(file)))) {
    return;
  }
  nsAutoString leafName;
  if (NS_SUCCEEDED(file->GetLeafName(leafName))) {
    tmpFile->RenameTo(nullptr, leafName);
  }
}

}  // namespace mozilla

////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////
//
// EXTERNALLY VISIBLE FUNCTIONS in mozilla::Telemetry::
// These are listed in Telemetry.h

namespace mozilla::Telemetry {

const char* GetHistogramName(HistogramID id) {
  return TelemetryHistogram::GetHistogramName(id);
}

bool CanRecordBase() { return TelemetryImpl::CanRecordBase(); }

bool CanRecordExtended() { return TelemetryImpl::CanRecordExtended(); }

bool CanRecordReleaseData() { return TelemetryImpl::CanRecordReleaseData(); }

bool CanRecordPrereleaseData() {
  return TelemetryImpl::CanRecordPrereleaseData();
}

void RecordSlowSQLStatement(const nsACString& statement,
                            const nsACString& dbName, uint32_t delay) {
  TelemetryImpl::RecordSlowStatement(statement, dbName, delay);
}

void Init() {
  // Make the service manager hold a long-lived reference to the service
  nsCOMPtr<nsITelemetry> telemetryService =
      do_GetService("@mozilla.org/base/telemetry;1");
  MOZ_ASSERT(telemetryService);
}

void WriteFailedProfileLock(nsIFile* aProfileDir) {
  nsCOMPtr<nsIFile> file;
  nsresult rv = GetFailedProfileLockFile(getter_AddRefs(file), aProfileDir);
  NS_ENSURE_SUCCESS_VOID(rv);
  int64_t fileSize = 0;
  rv = file->GetFileSize(&fileSize);
  // It's expected that the file might not exist yet
  if (NS_FAILED(rv) && rv != NS_ERROR_FILE_NOT_FOUND) {
    return;
  }
  nsCOMPtr<nsIRandomAccessStream> fileRandomAccessStream;
  rv = NS_NewLocalFileRandomAccessStream(getter_AddRefs(fileRandomAccessStream),
                                         file, PR_RDWR | PR_CREATE_FILE, 0640);
  NS_ENSURE_SUCCESS_VOID(rv);
  NS_ENSURE_TRUE_VOID(fileSize <= kMaxFailedProfileLockFileSize);
  unsigned int failedLockCount = 0;
  if (fileSize > 0) {
    nsCOMPtr<nsIInputStream> inStream =
        do_QueryInterface(fileRandomAccessStream);
    NS_ENSURE_TRUE_VOID(inStream);
    if (!GetFailedLockCount(inStream, fileSize, failedLockCount)) {
      failedLockCount = 0;
    }
  }
  ++failedLockCount;
  nsAutoCString bufStr;
  bufStr.AppendInt(static_cast<int>(failedLockCount));
  // If we read in an existing failed lock count, we need to reset the file ptr
  if (fileSize > 0) {
    rv = fileRandomAccessStream->Seek(nsISeekableStream::NS_SEEK_SET, 0);
    NS_ENSURE_SUCCESS_VOID(rv);
  }
  nsCOMPtr<nsIOutputStream> outStream =
      do_QueryInterface(fileRandomAccessStream);
  uint32_t bytesLeft = bufStr.Length();
  const char* bytes = bufStr.get();
  do {
    uint32_t written = 0;
    rv = outStream->Write(bytes, bytesLeft, &written);
    if (NS_FAILED(rv)) {
      break;
    }
    bytes += written;
    bytesLeft -= written;
  } while (bytesLeft > 0);
  fileRandomAccessStream->SetEOF();
}

void InitIOReporting(nsIFile* aXreDir) {
  // Never initialize twice
  if (sTelemetryIOObserver) {
    return;
  }

  sTelemetryIOObserver = new TelemetryIOInterposeObserver(aXreDir);
  IOInterposer::Register(IOInterposeObserver::OpAllWithStaging,
                         sTelemetryIOObserver);
}

void SetProfileDir(nsIFile* aProfD) {
  if (!sTelemetryIOObserver || !aProfD) {
    return;
  }
  nsAutoString profDirPath;
  nsresult rv = aProfD->GetPath(profDirPath);
  if (NS_FAILED(rv)) {
    return;
  }
  sTelemetryIOObserver->AddPath(profDirPath, u"{profile}"_ns);
}

void ShutdownTelemetry() { TelemetryImpl::ShutdownTelemetry(); }

}  // namespace mozilla::Telemetry

NS_IMPL_COMPONENT_FACTORY(nsITelemetry) {
  return TelemetryImpl::CreateTelemetryInstance().downcast<nsISupports>();
}
