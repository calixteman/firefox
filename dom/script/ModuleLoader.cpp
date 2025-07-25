/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ScriptLoader.h"
#include "ModuleLoader.h"

#include "jsapi.h"
#include "js/CompileOptions.h"  // JS::CompileOptions, JS::InstantiateOptions
#include "js/ContextOptions.h"  // JS::ContextOptionsRef
#include "js/friend/ErrorMessages.h"  // js::GetErrorMessage, JSMSG_*
#include "js/experimental/JSStencil.h"  // JS::Stencil, JS::CompileModuleScriptToStencil, JS::InstantiateModuleStencil
#include "js/MemoryFunctions.h"
#include "js/Modules.h"  // JS::FinishDynamicModuleImport, JS::{G,S}etModuleResolveHook, JS::Get{ModulePrivate,ModuleScript,RequestedModule{s,Specifier,SourcePos}}, JS::SetModule{DynamicImport,Metadata}Hook
#include "js/PropertyAndElement.h"  // JS_DefineProperty
#include "js/Realm.h"
#include "js/SourceText.h"
#include "js/loader/LoadedScript.h"
#include "js/loader/ScriptLoadRequest.h"
#include "js/loader/ModuleLoaderBase.h"
#include "js/loader/ModuleLoadRequest.h"
#include "mozilla/dom/RequestBinding.h"
#include "mozilla/Assertions.h"
#include "nsError.h"
#include "xpcpublic.h"
#include "GeckoProfiler.h"
#include "nsContentSecurityManager.h"
#include "nsIContent.h"
#include "nsJSUtils.h"
#include "mozilla/CycleCollectedJSContext.h"
#include "mozilla/dom/AutoEntryScript.h"
#include "mozilla/dom/Document.h"
#include "mozilla/dom/Element.h"
#include "nsIPrincipal.h"
#include "mozilla/LoadInfo.h"
#include "mozilla/Maybe.h"

using JS::SourceText;
using namespace JS::loader;

namespace mozilla::dom {

#undef LOG
#define LOG(args) \
  MOZ_LOG(ScriptLoader::gScriptLoaderLog, mozilla::LogLevel::Debug, args)

#define LOG_ENABLED() \
  MOZ_LOG_TEST(ScriptLoader::gScriptLoaderLog, mozilla::LogLevel::Debug)

//////////////////////////////////////////////////////////////
// DOM module loader
//////////////////////////////////////////////////////////////

ModuleLoader::ModuleLoader(ScriptLoader* aLoader,
                           nsIGlobalObject* aGlobalObject, Kind aKind)
    : ModuleLoaderBase(aLoader, aGlobalObject), mKind(aKind) {}

ScriptLoader* ModuleLoader::GetScriptLoader() {
  return static_cast<ScriptLoader*>(mLoader.get());
}

bool ModuleLoader::CanStartLoad(ModuleLoadRequest* aRequest, nsresult* aRvOut) {
  if (!GetScriptLoader()->GetDocument()) {
    *aRvOut = NS_ERROR_NULL_POINTER;
    return false;
  }

  // If this document is sandboxed without 'allow-scripts', abort.
  if (GetScriptLoader()->GetDocument()->HasScriptsBlockedBySandbox()) {
    *aRvOut = NS_OK;
    return false;
  }

  // To prevent dynamic code execution, content scripts can only
  // load moz-extension URLs.
  nsCOMPtr<nsIPrincipal> principal = aRequest->TriggeringPrincipal();
  if (BasePrincipal::Cast(principal)->ContentScriptAddonPolicy() &&
      !aRequest->mURI->SchemeIs("moz-extension")) {
    *aRvOut = NS_ERROR_DOM_WEBEXT_CONTENT_SCRIPT_URI;
    return false;
  }

  if (LOG_ENABLED()) {
    nsAutoCString url;
    aRequest->mURI->GetAsciiSpec(url);
    LOG(("ScriptLoadRequest (%p): Start Module Load (url = %s)", aRequest,
         url.get()));
  }

  return true;
}

nsresult ModuleLoader::StartFetch(ModuleLoadRequest* aRequest) {
  if (aRequest->IsStencil()) {
    GetScriptLoader()->EmulateNetworkEvents(aRequest);
    SetModuleFetchStarted(aRequest);
    return aRequest->OnFetchComplete(NS_OK);
  }

  // According to the spec, module scripts have different behaviour to classic
  // scripts and always use CORS. Only exception: Non linkable about: pages
  // which load local module scripts.
  bool isAboutPageLoadingChromeURI = ScriptLoader::IsAboutPageLoadingChromeURI(
      aRequest, GetScriptLoader()->GetDocument());

  nsContentSecurityManager::CORSSecurityMapping corsMapping =
      isAboutPageLoadingChromeURI
          ? nsContentSecurityManager::CORSSecurityMapping::DISABLE_CORS_CHECKS
          : nsContentSecurityManager::CORSSecurityMapping::REQUIRE_CORS_CHECKS;

  nsSecurityFlags securityFlags =
      nsContentSecurityManager::ComputeSecurityFlags(aRequest->CORSMode(),
                                                     corsMapping);

  securityFlags |= nsILoadInfo::SEC_ALLOW_CHROME;

  // Delegate Shared Behavior to base ScriptLoader
  //
  // aCharsetForPreload is passed as Nothing() because this is not a preload
  // and `StartLoadInternal` is able to find the charset by using `aRequest`
  // for this case.
  nsresult rv = GetScriptLoader()->StartLoadInternal(
      aRequest, securityFlags, Nothing() /* aCharsetForPreload */);
  NS_ENSURE_SUCCESS(rv, rv);

  // https://html.spec.whatwg.org/multipage/webappapis.html#fetch-an-import()-module-script-graph
  // Step 1. Disallow further import maps given settings object.
  if (!aRequest->GetScriptLoadContext()->IsPreload()) {
    LOG(("ScriptLoadRequest (%p): Disallow further import maps.", aRequest));
    DisallowImportMaps();
  }

  LOG(("ScriptLoadRequest (%p): Start fetching module", aRequest));

  return NS_OK;
}

void ModuleLoader::AsyncExecuteInlineModule(ModuleLoadRequest* aRequest) {
  MOZ_ALWAYS_SUCCEEDS(NS_DispatchToMainThread(
      mozilla::NewRunnableMethod<RefPtr<ModuleLoadRequest>>(
          "ModuleLoader::ExecuteInlineModule", this,
          &ModuleLoader::ExecuteInlineModule, aRequest)));
}

void ModuleLoader::ExecuteInlineModule(ModuleLoadRequest* aRequest) {
  MOZ_ASSERT(aRequest->IsFinished());
  MOZ_ASSERT(aRequest->IsTopLevel());
  MOZ_ASSERT(aRequest->GetScriptLoadContext()->mIsInline);

  if (aRequest->GetScriptLoadContext()->GetParserCreated() == NOT_FROM_PARSER) {
    GetScriptLoader()->RunScriptWhenSafe(aRequest);
  } else {
    GetScriptLoader()->MaybeMoveToLoadedList(aRequest);
    GetScriptLoader()->ProcessPendingRequests();
  }

  aRequest->GetScriptLoadContext()->MaybeUnblockOnload();
}

void ModuleLoader::OnModuleLoadComplete(ModuleLoadRequest* aRequest) {
  MOZ_ASSERT(aRequest->IsFinished());

  if (aRequest->IsTopLevel()) {
    if (aRequest->GetScriptLoadContext()->mIsInline &&
        aRequest->GetScriptLoadContext()->GetParserCreated() ==
            NOT_FROM_PARSER) {
      // https://html.spec.whatwg.org/#prepare-the-script-element
      // Step 32.2.
      //    type: "module":
      //    3.1. Queue an element task on the networking task source given
      //         el to perform the following steps:
      //        1. Mark as ready el given result.
      //
      // Step 33. If ... el's type is "module":
      //    ...
      //    3. Otherwise, if el is not parser-inserted:
      //      3. Set el's steps to run when the result is ready to the
      //         following:
      //        ...
      //        2.1. Execute the script element scripts[0].
      AsyncExecuteInlineModule(aRequest);
      return;
    } else if (aRequest->GetScriptLoadContext()->mIsInline &&
               aRequest->GetScriptLoadContext()->GetParserCreated() !=
                   NOT_FROM_PARSER &&
               !nsContentUtils::IsSafeToRunScript()) {
      // Avoid giving inline async module scripts that don't have
      // external dependencies a guaranteed execution time relative
      // to the HTML parse. That is, deliberately avoid guaranteeing
      // that the script would always observe a DOM shape where the
      // parser has not added further elements to the DOM.
      // (If `nsContentUtils::IsSafeToRunScript()` returns `true`,
      // we come here synchronously from the parser. If it returns
      // `false` we come here from an external dependency completing
      // its fetch, in which case we already are at an unspecific
      // point relative to the parse.)
      AsyncExecuteInlineModule(aRequest);
      return;
    } else {
      GetScriptLoader()->MaybeMoveToLoadedList(aRequest);
      GetScriptLoader()->ProcessPendingRequestsAsync();
    }
  }

  aRequest->GetScriptLoadContext()->MaybeUnblockOnload();
}

nsresult ModuleLoader::CompileFetchedModule(
    JSContext* aCx, JS::Handle<JSObject*> aGlobal, JS::CompileOptions& aOptions,
    ModuleLoadRequest* aRequest, JS::MutableHandle<JSObject*> aModuleOut) {
  if (aRequest->IsTextSource()) {
    GetScriptLoader()->CalculateBytecodeCacheFlag(aRequest);
  }

  if (!nsJSUtils::IsScriptable(aGlobal)) {
    return NS_ERROR_FAILURE;
  }

  switch (aRequest->mModuleType) {
    case JS::ModuleType::Unknown:
      MOZ_CRASH("Unexpected module type");
    case JS::ModuleType::JavaScript:
      return CompileJavaScriptModule(aCx, aOptions, aRequest, aModuleOut);
    case JS::ModuleType::JSON: {
      return CompileJsonModule(aCx, aOptions, aRequest, aModuleOut);
    }
  }

  MOZ_CRASH("Unhandled module type");
}

nsresult ModuleLoader::CompileJavaScriptModule(
    JSContext* aCx, JS::CompileOptions& aOptions, ModuleLoadRequest* aRequest,
    JS::MutableHandle<JSObject*> aModuleOut) {
  if (aRequest->IsStencil()) {
    JS::InstantiateOptions instantiateOptions(aOptions);
    RefPtr<JS::Stencil> stencil = aRequest->GetStencil();
    aModuleOut.set(
        JS::InstantiateModuleStencil(aCx, instantiateOptions, stencil));
    if (!aModuleOut) {
      return NS_ERROR_FAILURE;
    }

    bool alreadyStarted;
    if (!JS::StartCollectingDelazifications(aCx, aModuleOut, stencil,
                                            alreadyStarted)) {
      return NS_ERROR_FAILURE;
    }
    (void)alreadyStarted;

    return NS_OK;
  }

  if (aRequest->GetScriptLoadContext()->mWasCompiledOMT) {
    JS::InstantiationStorage storage;
    RefPtr<JS::Stencil> stencil =
        aRequest->GetScriptLoadContext()->StealOffThreadResult(aCx, &storage);
    if (!stencil) {
      return NS_ERROR_FAILURE;
    }

    JS::InstantiateOptions instantiateOptions(aOptions);
    aModuleOut.set(JS::InstantiateModuleStencil(aCx, instantiateOptions,
                                                stencil, &storage));
    if (!aModuleOut) {
      return NS_ERROR_FAILURE;
    }

    if (aRequest->IsTextSource() &&
        aRequest->PassedConditionForBytecodeEncoding()) {
      bool alreadyStarted;
      if (!JS::StartCollectingDelazifications(aCx, aModuleOut, stencil,
                                              alreadyStarted)) {
        return NS_ERROR_FAILURE;
      }
      MOZ_ASSERT(!alreadyStarted);
    }

    GetScriptLoader()->TryCacheRequest(aRequest, stencil);

    return NS_OK;
  }

  RefPtr<JS::Stencil> stencil;
  if (aRequest->IsTextSource()) {
    MaybeSourceText maybeSource;
    nsresult rv = aRequest->GetScriptSource(aCx, &maybeSource,
                                            aRequest->mLoadContext.get());
    NS_ENSURE_SUCCESS(rv, rv);

    auto compile = [&](auto& source) {
      return JS::CompileModuleScriptToStencil(aCx, aOptions, source);
    };
    stencil = maybeSource.mapNonEmpty(compile);
  } else {
    MOZ_ASSERT(aRequest->IsBytecode());
    JS::DecodeOptions decodeOptions(aOptions);
    decodeOptions.borrowBuffer = true;

    JS::TranscodeRange range = aRequest->Bytecode();
    JS::TranscodeResult tr =
        JS::DecodeStencil(aCx, decodeOptions, range, getter_AddRefs(stencil));
    if (tr != JS::TranscodeResult::Ok) {
      return NS_ERROR_DOM_JS_DECODING_ERROR;
    }
  }

  if (!stencil) {
    return NS_ERROR_FAILURE;
  }

  JS::InstantiateOptions instantiateOptions(aOptions);
  aModuleOut.set(
      JS::InstantiateModuleStencil(aCx, instantiateOptions, stencil));
  if (!aModuleOut) {
    return NS_ERROR_FAILURE;
  }

  if (aRequest->IsTextSource() &&
      aRequest->PassedConditionForBytecodeEncoding()) {
    bool alreadyStarted;
    if (!JS::StartCollectingDelazifications(aCx, aModuleOut, stencil,
                                            alreadyStarted)) {
      return NS_ERROR_FAILURE;
    }
    MOZ_ASSERT(!alreadyStarted);
  }

  GetScriptLoader()->TryCacheRequest(aRequest, stencil);

  return NS_OK;
}

nsresult ModuleLoader::CompileJsonModule(
    JSContext* aCx, JS::CompileOptions& aOptions, ModuleLoadRequest* aRequest,
    JS::MutableHandle<JSObject*> aModuleOut) {
  MOZ_ASSERT(!aRequest->GetScriptLoadContext()->mWasCompiledOMT);

  MOZ_ASSERT(aRequest->IsTextSource());
  ModuleLoader::MaybeSourceText maybeSource;
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

  aModuleOut.set(jsonModule);
  return NS_OK;
}

already_AddRefed<ModuleLoadRequest> ModuleLoader::CreateTopLevel(
    nsIURI* aURI, nsIScriptElement* aElement, ReferrerPolicy aReferrerPolicy,
    ScriptFetchOptions* aFetchOptions, const SRIMetadata& aIntegrity,
    nsIURI* aReferrer, ScriptLoadContext* aContext,
    ScriptLoadRequestType aRequestType) {
  RefPtr<ModuleLoadRequest> request =
      new ModuleLoadRequest(aURI, JS::ModuleType::JavaScript, aReferrerPolicy,
                            aFetchOptions, aIntegrity, aReferrer, aContext,
                            ModuleLoadRequest::Kind::TopLevel, this, nullptr);

  GetScriptLoader()->TryUseCache(request, aElement, aFetchOptions->mNonce,
                                 aRequestType);

  return request.forget();
}

already_AddRefed<ModuleLoadRequest> ModuleLoader::CreateStaticImport(
    nsIURI* aURI, JS::ModuleType aModuleType, ModuleScript* aReferrerScript,
    const mozilla::dom::SRIMetadata& aSriMetadata,
    JS::loader::LoadContextBase* aLoadContext,
    JS::loader::ModuleLoaderBase* aLoader) {
  RefPtr<ScriptLoadContext> newContext = new ScriptLoadContext();
  newContext->mIsInline = false;
  // Propagated Parent values. TODO: allow child modules to use root module's
  // script mode.
  newContext->mScriptMode = aLoadContext->AsWindowContext()->mScriptMode;

  RefPtr<ModuleLoadRequest> request = new ModuleLoadRequest(
      aURI, aModuleType, aReferrerScript->ReferrerPolicy(),
      aReferrerScript->GetFetchOptions(), aSriMetadata,
      aReferrerScript->GetURI(), newContext,
      ModuleLoadRequest::Kind::StaticImport, aLoader,
      aLoadContext->mRequest->AsModuleRequest()->GetRootModule());

  GetScriptLoader()->TryUseCache(request);

  return request.forget();
}

already_AddRefed<ModuleLoadRequest> ModuleLoader::CreateDynamicImport(
    JSContext* aCx, nsIURI* aURI, LoadedScript* aMaybeActiveScript,
    JS::Handle<JSObject*> aModuleRequestObj, JS::Handle<JSObject*> aPromise) {
  MOZ_ASSERT(aModuleRequestObj);
  MOZ_ASSERT(aPromise);

  RefPtr<ScriptFetchOptions> options = nullptr;
  nsIURI* baseURL = nullptr;
  RefPtr<ScriptLoadContext> context = new ScriptLoadContext();
  ReferrerPolicy referrerPolicy;

  if (aMaybeActiveScript) {
    // https://html.spec.whatwg.org/multipage/webappapis.html#hostloadimportedmodule
    // Step 6.3. Set fetchOptions to the new descendant script fetch options for
    // referencingScript's fetch options.
    options = aMaybeActiveScript->GetFetchOptions();
    referrerPolicy = aMaybeActiveScript->ReferrerPolicy();
    baseURL = aMaybeActiveScript->BaseURL();
  } else {
    // We don't have a referencing script so fall back on using
    // options from the document. This can happen when the user
    // triggers an inline event handler, as there is no active script
    // there.
    Document* document = GetScriptLoader()->GetDocument();

    nsCOMPtr<nsIPrincipal> principal = GetGlobalObject()->PrincipalOrNull();
    MOZ_ASSERT_IF(GetKind() == WebExtension,
                  BasePrincipal::Cast(principal)->ContentScriptAddonPolicy());
    MOZ_ASSERT_IF(GetKind() == Normal, principal == document->NodePrincipal());

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
        mozilla::CORS_NONE, /* aNonce = */ u""_ns, RequestPriority::Auto,
        ParserMetadata::NotParserInserted, principal);
    referrerPolicy = document->GetReferrerPolicy();
    baseURL = document->GetDocBaseURI();
  }

  context->mIsInline = false;
  context->mScriptMode = ScriptLoadContext::ScriptMode::eAsync;

  JS::ModuleType moduleType = JS::GetModuleRequestType(aCx, aModuleRequestObj);
  SRIMetadata sriMetadata;
  GetImportMapSRI(aURI, baseURL, mLoader->GetConsoleReportCollector(),
                  &sriMetadata);

  RefPtr<ModuleLoadRequest> request = new ModuleLoadRequest(
      aURI, moduleType, referrerPolicy, options, sriMetadata, baseURL, context,
      ModuleLoadRequest::Kind::DynamicImport, this, nullptr);

  request->SetDynamicImport(aMaybeActiveScript, aModuleRequestObj, aPromise);

  GetScriptLoader()->TryUseCache(request);
  return request.forget();
}

ModuleLoader::~ModuleLoader() {
  LOG(("ModuleLoader::~ModuleLoader %p", this));
  mLoader = nullptr;
}

#undef LOG
#undef LOG_ENABLED

}  // namespace mozilla::dom
