/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "js/experimental/JSStencil.h"  // JS::Stencil, JS::CompileModuleScriptToStencil, JS::InstantiateModuleStencil
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/loader/ModuleLoadRequest.h"
#include "mozilla/dom/RequestBinding.h"
#include "mozilla/dom/WorkerLoadContext.h"
#include "mozilla/dom/WorkerPrivate.h"
#include "mozilla/dom/workerinternals/ScriptLoader.h"
#include "mozilla/dom/WorkerScope.h"
#include "WorkerModuleLoader.h"

#include "nsISupportsImpl.h"

namespace mozilla::dom::workerinternals::loader {

//////////////////////////////////////////////////////////////
// WorkerModuleLoader
//////////////////////////////////////////////////////////////

NS_IMPL_ADDREF_INHERITED(WorkerModuleLoader, JS::loader::ModuleLoaderBase)
NS_IMPL_RELEASE_INHERITED(WorkerModuleLoader, JS::loader::ModuleLoaderBase)

NS_IMPL_CYCLE_COLLECTION_INHERITED(WorkerModuleLoader,
                                   JS::loader::ModuleLoaderBase)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(WorkerModuleLoader)
NS_INTERFACE_MAP_END_INHERITING(JS::loader::ModuleLoaderBase)

WorkerModuleLoader::WorkerModuleLoader(WorkerScriptLoader* aScriptLoader,
                                       nsIGlobalObject* aGlobalObject)
    : ModuleLoaderBase(aScriptLoader, aGlobalObject) {}

nsIURI* WorkerModuleLoader::GetBaseURI() const {
  WorkerPrivate* workerPrivate = GetCurrentThreadWorkerPrivate();
  return workerPrivate->GetBaseURI();
}

already_AddRefed<ModuleLoadRequest> WorkerModuleLoader::CreateStaticImport(
    nsIURI* aURI, JS::ModuleType aModuleType,
    JS::loader::ModuleScript* aReferrerScript,
    const mozilla::dom::SRIMetadata& aSriMetadata,
    JS::loader::LoadContextBase* aLoadContext,
    JS::loader::ModuleLoaderBase* aLoader) {
  // We are intentionally deviating from the specification here and using the
  // worker's CSP rather than the document CSP. The spec otherwise requires our
  // service worker integration to be changed, and additionally the decision
  // here did not make sense as we are treating static imports as different from
  // other kinds of subresources.
  // See Discussion in https://github.com/w3c/webappsec-csp/issues/336
  Maybe<ClientInfo> clientInfo = GetGlobalObject()->GetClientInfo();

  WorkerLoadContext* context = aLoadContext->AsWorkerContext();
  RefPtr<WorkerLoadContext> loadContext = new WorkerLoadContext(
      WorkerLoadContext::Kind::StaticImport, clientInfo, context->mScriptLoader,
      context->mOnlyExistingCachedResourcesAllowed);
  RefPtr<ModuleLoadRequest> request = new ModuleLoadRequest(
      aURI, aModuleType, aReferrerScript->ReferrerPolicy(),
      aReferrerScript->GetFetchOptions(), SRIMetadata(),
      aReferrerScript->GetURI(), loadContext,
      ModuleLoadRequest::Kind::StaticImport, this,
      aLoadContext->mRequest->AsModuleRequest()->GetRootModule());

  request->mURL = request->mURI->GetSpecOrDefault();
  request->NoCacheEntryFound();
  return request.forget();
}

bool WorkerModuleLoader::CreateDynamicImportLoader() {
  WorkerPrivate* workerPrivate = GetCurrentThreadWorkerPrivate();
  workerPrivate->AssertIsOnWorkerThread();

  IgnoredErrorResult rv;
  RefPtr<WorkerScriptLoader> loader = loader::WorkerScriptLoader::Create(
      workerPrivate, nullptr, nullptr,
      GetCurrentScriptLoader()->GetWorkerScriptType(), rv);
  if (NS_WARN_IF(rv.Failed())) {
    return false;
  }

  SetScriptLoader(loader);
  return true;
}

already_AddRefed<ModuleLoadRequest> WorkerModuleLoader::CreateDynamicImport(
    JSContext* aCx, nsIURI* aURI, LoadedScript* aMaybeActiveScript,
    JS::Handle<JSObject*> aModuleRequestObj, JS::Handle<JSObject*> aPromise) {
  WorkerPrivate* workerPrivate = GetCurrentThreadWorkerPrivate();

  if (!CreateDynamicImportLoader()) {
    return nullptr;
  }

  // Not supported for Service Workers.
  // https://github.com/w3c/ServiceWorker/issues/1585 covers existing discussion
  // about potentially supporting use of import().
  if (workerPrivate->IsServiceWorker()) {
    return nullptr;
  }
  MOZ_ASSERT(aModuleRequestObj);
  MOZ_ASSERT(aPromise);

  RefPtr<ScriptFetchOptions> options;
  nsIURI* baseURL = nullptr;
  if (aMaybeActiveScript) {
    // https://html.spec.whatwg.org/multipage/webappapis.html#hostloadimportedmodule
    // Step 6.3. Set fetchOptions to the new descendant script fetch options for
    // referencingScript's fetch options.
    options = aMaybeActiveScript->GetFetchOptions();
    baseURL = aMaybeActiveScript->BaseURL();
  } else {
    // https://html.spec.whatwg.org/multipage/webappapis.html#hostloadimportedmodule
    // Step 4. Let fetchOptions be the default classic script fetch options.
    //
    // https://html.spec.whatwg.org/multipage/webappapis.html#default-classic-script-fetch-options
    // The default classic script fetch options are a script fetch options whose
    // cryptographic nonce is the empty string, integrity metadata is the empty
    // string, parser metadata is "not-parser-inserted", credentials mode is
    // "same-origin", referrer policy is the empty string, and fetch priority is
    // "auto".
    options = new ScriptFetchOptions(
        CORSMode::CORS_NONE, /* aNonce = */ u""_ns, RequestPriority::Auto,
        JS::loader::ParserMetadata::NotParserInserted, nullptr);
    baseURL = GetBaseURI();
  }

  Maybe<ClientInfo> clientInfo = GetGlobalObject()->GetClientInfo();

  RefPtr<WorkerLoadContext> context = new WorkerLoadContext(
      WorkerLoadContext::Kind::DynamicImport, clientInfo,
      GetCurrentScriptLoader(),
      // When dynamic import is supported in ServiceWorkers,
      // the current plan in onlyExistingCachedResourcesAllowed
      // is that only existing cached resources will be
      // allowed.  (`import()` will not be used for caching
      // side effects, but instead a specific method will be
      // used during installation.)
      true);

  JS::ModuleType moduleType = JS::GetModuleRequestType(aCx, aModuleRequestObj);
  ReferrerPolicy referrerPolicy = workerPrivate->GetReferrerPolicy();
  RefPtr<ModuleLoadRequest> request = new ModuleLoadRequest(
      aURI, moduleType, referrerPolicy, options, SRIMetadata(), baseURL,
      context, ModuleLoadRequest::Kind::DynamicImport, this, nullptr);

  request->SetDynamicImport(aMaybeActiveScript, aModuleRequestObj, aPromise);
  request->NoCacheEntryFound();

  return request.forget();
}

bool WorkerModuleLoader::IsDynamicImportSupported() {
  WorkerPrivate* workerPrivate = GetCurrentThreadWorkerPrivate();
  if (workerPrivate->IsServiceWorker()) {
    return false;
  }

  return true;
}

bool WorkerModuleLoader::CanStartLoad(ModuleLoadRequest* aRequest,
                                      nsresult* aRvOut) {
  return true;
}

nsresult WorkerModuleLoader::StartFetch(ModuleLoadRequest* aRequest) {
  if (!GetScriptLoaderFor(aRequest)->DispatchLoadScript(aRequest)) {
    return NS_ERROR_FAILURE;
  }
  return NS_OK;
}

nsresult WorkerModuleLoader::CompileFetchedModule(
    JSContext* aCx, JS::Handle<JSObject*> aGlobal, JS::CompileOptions& aOptions,
    ModuleLoadRequest* aRequest, JS::MutableHandle<JSObject*> aModuleScript) {
  switch (aRequest->mModuleType) {
    case JS::ModuleType::Unknown:
      MOZ_CRASH("Unexpected module type");
    case JS::ModuleType::JavaScript:
      return CompileJavaScriptModule(aCx, aOptions, aRequest, aModuleScript);
    case JS::ModuleType::JSON:
      return CompileJsonModule(aCx, aOptions, aRequest, aModuleScript);
  }

  MOZ_CRASH("Unhandled module type");
}

nsresult WorkerModuleLoader::CompileJavaScriptModule(
    JSContext* aCx, JS::CompileOptions& aOptions, ModuleLoadRequest* aRequest,
    JS::MutableHandle<JSObject*> aModuleScript) {
  MOZ_ASSERT(aRequest->IsTextSource());
  MaybeSourceText maybeSource;
  nsresult rv = aRequest->GetScriptSource(aCx, &maybeSource,
                                          aRequest->mLoadContext.get());
  NS_ENSURE_SUCCESS(rv, rv);

  RefPtr<JS::Stencil> stencil;

  auto compile = [&](auto& source) {
    return JS::CompileModuleScriptToStencil(aCx, aOptions, source);
  };
  stencil = maybeSource.mapNonEmpty(compile);

  if (!stencil) {
    return NS_ERROR_FAILURE;
  }

  JS::InstantiateOptions instantiateOptions(aOptions);
  aModuleScript.set(
      JS::InstantiateModuleStencil(aCx, instantiateOptions, stencil));
  if (!aModuleScript) {
    return NS_ERROR_FAILURE;
  }

  return NS_OK;
}

nsresult WorkerModuleLoader::CompileJsonModule(
    JSContext* aCx, JS::CompileOptions& aOptions, ModuleLoadRequest* aRequest,
    JS::MutableHandle<JSObject*> aModuleScript) {
  MOZ_ASSERT(aRequest->IsTextSource());
  MaybeSourceText maybeSource;
  nsresult rv = aRequest->GetScriptSource(aCx, &maybeSource,
                                          aRequest->mLoadContext.get());
  NS_ENSURE_SUCCESS(rv, rv);

  auto compile = [&](auto& source) {
    return JS::CompileJsonModule(aCx, aOptions, source);
  };

  auto* jsonModule = maybeSource.mapNonEmpty(compile);
  if (!jsonModule) {
    return NS_ERROR_FAILURE;
  }

  aModuleScript.set(jsonModule);
  return NS_OK;
}

WorkerScriptLoader* WorkerModuleLoader::GetCurrentScriptLoader() {
  return static_cast<WorkerScriptLoader*>(mLoader.get());
}

WorkerScriptLoader* WorkerModuleLoader::GetScriptLoaderFor(
    ModuleLoadRequest* aRequest) {
  return aRequest->GetWorkerLoadContext()->mScriptLoader;
}

void WorkerModuleLoader::OnModuleLoadComplete(ModuleLoadRequest* aRequest) {
  if (aRequest->IsTopLevel()) {
    AutoJSAPI jsapi;
    if (NS_WARN_IF(!jsapi.Init(GetGlobalObject()))) {
      return;
    }
    RefPtr<WorkerScriptLoader> requestScriptLoader =
        GetScriptLoaderFor(aRequest);
    if (aRequest->IsDynamicImport()) {
      aRequest->ProcessDynamicImport();
      requestScriptLoader->TryShutdown();
    } else {
      requestScriptLoader->MaybeMoveToLoadedList(aRequest);
      requestScriptLoader->ProcessPendingRequests(jsapi.cx());
    }
  }
}

bool WorkerModuleLoader::IsModuleEvaluationAborted(
    ModuleLoadRequest* aRequest) {
  WorkerPrivate* workerPrivate = GetCurrentThreadWorkerPrivate();
  return !workerPrivate || !workerPrivate->GlobalScope() ||
         workerPrivate->GlobalScope()->IsDying();
}

}  // namespace mozilla::dom::workerinternals::loader
