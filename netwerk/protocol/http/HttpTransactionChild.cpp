/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */
/* vim:set ts=4 sw=4 sts=4 et cin: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

// HttpLog.h should generally be included first
#include "HttpLog.h"

#include "HttpTransactionChild.h"

#include "mozilla/ipc/IPCStreamUtils.h"
#include "mozilla/net/BackgroundDataBridgeParent.h"
#include "mozilla/net/ChannelEventQueue.h"
#include "mozilla/net/InputChannelThrottleQueueChild.h"
#include "mozilla/net/SocketProcessChild.h"
#include "mozilla/ScopeExit.h"
#include "mozilla/StaticPrefs_network.h"
#include "nsInputStreamPump.h"
#include "nsITransportSecurityInfo.h"
#include "nsHttpHandler.h"
#include "nsNetUtil.h"
#include "nsProxyInfo.h"
#include "nsProxyRelease.h"
#include "nsQueryObject.h"
#include "nsSerializationHelper.h"
#include "OpaqueResponseUtils.h"
#include "nsIRequestContext.h"

namespace mozilla::net {

NS_IMPL_ISUPPORTS(HttpTransactionChild, nsIRequestObserver, nsIStreamListener,
                  nsITransportEventSink, nsIThrottledInputChannel,
                  nsIThreadRetargetableStreamListener, nsIEarlyHintObserver);

//-----------------------------------------------------------------------------
// HttpTransactionChild <public>
//-----------------------------------------------------------------------------

HttpTransactionChild::HttpTransactionChild() {
  LOG(("Creating HttpTransactionChild @%p\n", this));
}

HttpTransactionChild::~HttpTransactionChild() {
  LOG(("Destroying HttpTransactionChild @%p\n", this));
}

static already_AddRefed<nsIRequestContext> CreateRequestContext(
    uint64_t aRequestContextID) {
  if (!aRequestContextID) {
    return nullptr;
  }

  nsIRequestContextService* rcsvc = gHttpHandler->GetRequestContextService();
  if (!rcsvc) {
    return nullptr;
  }

  nsCOMPtr<nsIRequestContext> requestContext;
  rcsvc->GetRequestContext(aRequestContextID, getter_AddRefs(requestContext));

  return requestContext.forget();
}

nsresult HttpTransactionChild::InitInternal(
    uint32_t caps, const HttpConnectionInfoCloneArgs& infoArgs,
    nsHttpRequestHead* requestHead, nsIInputStream* requestBody,
    uint64_t requestContentLength, bool requestBodyHasHeaders,
    uint64_t browserId, uint8_t httpTrafficCategory, uint64_t requestContextID,
    ClassOfService classOfService, uint32_t initialRwin,
    bool responseTimeoutEnabled, uint64_t channelId,
    bool aHasTransactionObserver,
    const nsILoadInfo::IPAddressSpace& aParentIPAddressSpace,
    const LNAPerms& aLnaPermissionStatus) {
  LOG(("HttpTransactionChild::InitInternal [this=%p caps=%x]\n", this, caps));

  RefPtr<nsHttpConnectionInfo> cinfo =
      nsHttpConnectionInfo::DeserializeHttpConnectionInfoCloneArgs(infoArgs);
  nsCOMPtr<nsIRequestContext> rc = CreateRequestContext(requestContextID);

  std::function<void(TransactionObserverResult&&)> observer;
  if (aHasTransactionObserver) {
    nsMainThreadPtrHandle<HttpTransactionChild> handle(
        new nsMainThreadPtrHolder<HttpTransactionChild>(
            "HttpTransactionChildProxy", this, false));
    observer = [handle](TransactionObserverResult&& aResult) {
      handle->mTransactionObserverResult.emplace(std::move(aResult));
    };
  }

  nsresult rv = mTransaction->Init(
      caps, cinfo, requestHead, requestBody, requestContentLength,
      requestBodyHasHeaders, GetCurrentSerialEventTarget(),
      nullptr,  // TODO: security callback, fix in bug 1512479.
      this, browserId, static_cast<HttpTrafficCategory>(httpTrafficCategory),
      rc, classOfService, initialRwin, responseTimeoutEnabled, channelId,
      std::move(observer), aParentIPAddressSpace, aLnaPermissionStatus);
  if (NS_WARN_IF(NS_FAILED(rv))) {
    mTransaction = nullptr;
    return rv;
  }

  Unused << mTransaction->AsyncRead(this, getter_AddRefs(mTransactionPump));
  return rv;
}

mozilla::ipc::IPCResult HttpTransactionChild::RecvCancelPump(
    const nsresult& aStatus) {
  LOG(("HttpTransactionChild::RecvCancelPump start [this=%p]\n", this));
  CancelInternal(aStatus);
  return IPC_OK();
}

void HttpTransactionChild::CancelInternal(nsresult aStatus) {
  MOZ_ASSERT(NS_FAILED(aStatus));

  mCanceled = true;
  mStatus = aStatus;
  if (mTransactionPump) {
    mTransactionPump->Cancel(mStatus);
  }
}

mozilla::ipc::IPCResult HttpTransactionChild::RecvSuspendPump() {
  LOG(("HttpTransactionChild::RecvSuspendPump start [this=%p]\n", this));

  if (mTransactionPump) {
    mTransactionPump->Suspend();
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult HttpTransactionChild::RecvResumePump() {
  LOG(("HttpTransactionChild::RecvResumePump start [this=%p]\n", this));

  if (mTransactionPump) {
    mTransactionPump->Resume();
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult HttpTransactionChild::RecvInit(
    const uint32_t& aCaps, const HttpConnectionInfoCloneArgs& aArgs,
    const nsHttpRequestHead& aReqHeaders, const Maybe<IPCStream>& aRequestBody,
    const uint64_t& aReqContentLength, const bool& aReqBodyIncludesHeaders,
    const uint64_t& aTopLevelOuterContentWindowId,
    const uint8_t& aHttpTrafficCategory, const uint64_t& aRequestContextID,
    const ClassOfService& aClassOfService, const uint32_t& aInitialRwin,
    const bool& aResponseTimeoutEnabled, const uint64_t& aChannelId,
    const bool& aHasTransactionObserver,
    const mozilla::Maybe<PInputChannelThrottleQueueChild*>& aThrottleQueue,
    const bool& aIsDocumentLoad,
    const nsILoadInfo::IPAddressSpace& aParentIPAddressSpace,
    const LNAPerms& aLnaPermissionStatus, const TimeStamp& aRedirectStart,
    const TimeStamp& aRedirectEnd) {
  mRequestHead = aReqHeaders;
  if (aRequestBody) {
    mUploadStream = mozilla::ipc::DeserializeIPCStream(aRequestBody);
  }

  mTransaction = new nsHttpTransaction();
  mChannelId = aChannelId;
  mIsDocumentLoad = aIsDocumentLoad;
  mRedirectStart = aRedirectStart;
  mRedirectEnd = aRedirectEnd;

  if (aThrottleQueue.isSome()) {
    mThrottleQueue =
        static_cast<InputChannelThrottleQueueChild*>(aThrottleQueue.ref());
  }

  nsresult rv = InitInternal(
      aCaps, aArgs, &mRequestHead, mUploadStream, aReqContentLength,
      aReqBodyIncludesHeaders, aTopLevelOuterContentWindowId,
      aHttpTrafficCategory, aRequestContextID, aClassOfService, aInitialRwin,
      aResponseTimeoutEnabled, aChannelId, aHasTransactionObserver,
      aParentIPAddressSpace, aLnaPermissionStatus);
  if (NS_FAILED(rv)) {
    LOG(("HttpTransactionChild::RecvInit: [this=%p] InitInternal failed!\n",
         this));
    mTransaction = nullptr;
    SendOnInitFailed(rv);
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult HttpTransactionChild::RecvSetDNSWasRefreshed() {
  LOG(("HttpTransactionChild::SetDNSWasRefreshed [this=%p]\n", this));
  if (mTransaction) {
    mTransaction->SetDNSWasRefreshed();
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult HttpTransactionChild::RecvDontReuseConnection() {
  LOG(("HttpTransactionChild::RecvDontReuseConnection [this=%p]\n", this));
  if (mTransaction) {
    mTransaction->DontReuseConnection();
  }
  return IPC_OK();
}

mozilla::ipc::IPCResult HttpTransactionChild::RecvSetH2WSConnRefTaken() {
  LOG(("HttpTransactionChild::RecvSetH2WSConnRefTaken [this=%p]\n", this));
  if (mTransaction) {
    mTransaction->SetH2WSConnRefTaken();
  }
  return IPC_OK();
}

void HttpTransactionChild::ActorDestroy(ActorDestroyReason aWhy) {
  LOG(("HttpTransactionChild::ActorDestroy [this=%p]\n", this));
  mTransaction = nullptr;
  mTransactionPump = nullptr;
}

nsHttpTransaction* HttpTransactionChild::GetHttpTransaction() {
  return mTransaction.get();
}

//-----------------------------------------------------------------------------
// HttpTransactionChild <nsIStreamListener>
//-----------------------------------------------------------------------------

NS_IMETHODIMP
HttpTransactionChild::OnDataAvailable(nsIRequest* aRequest,
                                      nsIInputStream* aInputStream,
                                      uint64_t aOffset, uint32_t aCount) {
  LOG(("HttpTransactionChild::OnDataAvailable [this=%p, aOffset= %" PRIu64
       " aCount=%" PRIu32 "]\n",
       this, aOffset, aCount));

  // Don't bother sending IPC if already canceled.
  if (mCanceled) {
    return mStatus;
  }

  // TODO: send string data in chunks and handle errors. Bug 1600129.
  nsCString data;
  nsresult rv = NS_ReadInputStreamToString(aInputStream, data, aCount);
  if (NS_FAILED(rv)) {
    return rv;
  }

  mLogicalOffset += aCount;

  if (NS_IsMainThread()) {
    if (!CanSend()) {
      return NS_ERROR_FAILURE;
    }

    nsHttp::SendFunc<nsCString> sendFunc =
        [self = UnsafePtr<HttpTransactionChild>(this)](
            const nsCString& aData, uint64_t aOffset, uint32_t aCount) {
          return self->SendOnDataAvailable(aData, aOffset, aCount,
                                           TimeStamp::Now());
        };

    LOG(("  ODA to parent process"));
    if (!nsHttp::SendDataInChunks(data, aOffset, aCount, sendFunc)) {
      return NS_ERROR_FAILURE;
    }
    return NS_OK;
  }

  MOZ_ASSERT(mDataBridgeParent);

  if (!mDataBridgeParent->CanSend()) {
    return NS_ERROR_FAILURE;
  }

  nsHttp::SendFunc<nsDependentCSubstring> sendFunc =
      [self = UnsafePtr<HttpTransactionChild>(this)](
          const nsDependentCSubstring& aData, uint64_t aOffset,
          uint32_t aCount) {
        return self->mDataBridgeParent->SendOnTransportAndData(
            aOffset, aCount, aData, TimeStamp::Now());
      };

  LOG(("  ODA to content process"));
  if (!nsHttp::SendDataInChunks(data, aOffset, aCount, sendFunc)) {
    MOZ_ASSERT(false, "Send ODA to content process failed");
    return NS_ERROR_FAILURE;
  }

  // We still need to send ODA to parent process, because the data needs to be
  // saved in cache. Note that we set dataSentToChildProcess to true, so this
  // ODA will not be sent to child process.
  RefPtr<HttpTransactionChild> self = this;
  rv = NS_DispatchToMainThread(
      NS_NewRunnableFunction(
          "HttpTransactionChild::OnDataAvailable",
          [self, offset(aOffset), count(aCount), data(data)]() {
            nsHttp::SendFunc<nsCString> sendFunc =
                [self](const nsCString& aData, uint64_t aOffset,
                       uint32_t aCount) {
                  return self->SendOnDataAvailable(aData, aOffset, aCount,
                                                   TimeStamp::Now());
                };

            if (!nsHttp::SendDataInChunks(data, offset, count, sendFunc)) {
              self->CancelInternal(NS_ERROR_FAILURE);
            }
          }),
      NS_DISPATCH_NORMAL);
  MOZ_ASSERT(NS_SUCCEEDED(rv));

  return NS_OK;
}

static TimingStructArgs ToTimingStructArgs(TimingStruct aTiming) {
  TimingStructArgs args;
  args.domainLookupStart() = aTiming.domainLookupStart;
  args.domainLookupEnd() = aTiming.domainLookupEnd;
  args.connectStart() = aTiming.connectStart;
  args.tcpConnectEnd() = aTiming.tcpConnectEnd;
  args.secureConnectionStart() = aTiming.secureConnectionStart;
  args.connectEnd() = aTiming.connectEnd;
  args.requestStart() = aTiming.requestStart;
  args.responseStart() = aTiming.responseStart;
  args.responseEnd() = aTiming.responseEnd;
  args.transactionPending() = aTiming.transactionPending;
  return args;
}

// The maximum number of bytes to consider when attempting to sniff.
// See https://mimesniff.spec.whatwg.org/#reading-the-resource-header.
static const uint32_t MAX_BYTES_SNIFFED = 1445;

static void GetDataForSniffer(void* aClosure, const uint8_t* aData,
                              uint32_t aCount) {
  nsTArray<uint8_t>* outData = static_cast<nsTArray<uint8_t>*>(aClosure);
  outData->AppendElements(aData, std::min(aCount, MAX_BYTES_SNIFFED));
}

bool HttpTransactionChild::CanSendODAToContentProcessDirectly(
    const Maybe<nsHttpResponseHead>& aHead) {
  if (!StaticPrefs::network_send_ODA_to_content_directly()) {
    return false;
  }

  // If this is a document load, the content process that receives ODA is not
  // decided yet, so don't bother to do the rest check.
  if (mIsDocumentLoad) {
    return false;
  }

  if (!aHead) {
    return false;
  }

  // We only need to deliver ODA when the response is succeed.
  if (aHead->Status() != 200) {
    return false;
  }

  // UnknownDecoder could be used in parent process, so we can't send ODA to
  // content process.
  if (!aHead->HasContentType()) {
    return false;
  }

  return true;
}

NS_IMETHODIMP
HttpTransactionChild::OnStartRequest(nsIRequest* aRequest) {
  LOG(("HttpTransactionChild::OnStartRequest start [this=%p] mTransaction=%p\n",
       this, mTransaction.get()));

  // Don't bother sending IPC to parent process if already canceled.
  if (mCanceled) {
    return mStatus;
  }

  if (!CanSend()) {
    return NS_ERROR_FAILURE;
  }

  MOZ_ASSERT(mTransaction);

  nsresult status;
  aRequest->GetStatus(&status);

  mProtocolVersion.Truncate();

  nsCOMPtr<nsITransportSecurityInfo> securityInfo(mTransaction->SecurityInfo());
  if (securityInfo) {
    nsAutoCString protocol;
    if (NS_SUCCEEDED(securityInfo->GetNegotiatedNPN(protocol)) &&
        !protocol.IsEmpty()) {
      mProtocolVersion.Assign(protocol);
    }
  }

  RefPtr<nsHttpConnectionInfo> connInfo;
  UniquePtr<nsHttpResponseHead> head(
      mTransaction->TakeResponseHeadAndConnInfo(getter_AddRefs(connInfo)));
  Maybe<nsHttpResponseHead> optionalHead;
  nsTArray<uint8_t> dataForSniffer;
  if (head) {
    if (mProtocolVersion.IsEmpty()) {
      HttpVersion version = head->Version();
      mProtocolVersion.Assign(nsHttp::GetProtocolVersion(version));
    }
    optionalHead = Some(*head);

    if (GetOpaqueResponseBlockedReason(*head) ==
        OpaqueResponseBlockedReason::BLOCKED_SHOULD_SNIFF) {
      RefPtr<nsInputStreamPump> pump = do_QueryObject(mTransactionPump);
      pump->PeekStream(GetDataForSniffer, &dataForSniffer);
    }
  }

  Maybe<nsCString> optionalAltSvcUsed;
  nsCString altSvcUsed;
  if (NS_SUCCEEDED(mTransaction->RequestHead()->GetHeader(
          nsHttp::Alternate_Service_Used, altSvcUsed)) &&
      !altSvcUsed.IsEmpty()) {
    optionalAltSvcUsed.emplace(altSvcUsed);
  }

  if (CanSendODAToContentProcessDirectly(optionalHead)) {
    Maybe<RefPtr<BackgroundDataBridgeParent>> dataBridgeParent =
        SocketProcessChild::GetSingleton()->GetAndRemoveDataBridge(mChannelId);
    // Check if there is a registered BackgroundDataBridgeParent.
    if (dataBridgeParent) {
      mDataBridgeParent = std::move(dataBridgeParent.ref());

      nsCOMPtr<nsISerialEventTarget> backgroundThread =
          mDataBridgeParent->GetBackgroundThread();
      nsCOMPtr<nsIThreadRetargetableRequest> retargetableTransactionPump;
      retargetableTransactionPump = do_QueryObject(mTransactionPump);
      // nsInputStreamPump should implement this interface.
      MOZ_ASSERT(retargetableTransactionPump);

      nsresult rv =
          retargetableTransactionPump->RetargetDeliveryTo(backgroundThread);
      LOG((" Retarget to background thread [this=%p rv=%08x]\n", this,
           static_cast<uint32_t>(rv)));
      if (NS_FAILED(rv)) {
        mDataBridgeParent->Destroy();
        mDataBridgeParent = nullptr;
      }
    }
  }

  int32_t proxyConnectResponseCode =
      mTransaction->GetProxyConnectResponseCode();

  nsIRequest::TRRMode mode = nsIRequest::TRR_DEFAULT_MODE;
  TRRSkippedReason reason = nsITRRSkipReason::TRR_UNSET;
  {
    NetAddr selfAddr;
    NetAddr peerAddr;
    bool isTrr = false;
    bool echConfigUsed = false;
    if (mTransaction) {
      mTransaction->GetNetworkAddresses(selfAddr, peerAddr, isTrr, mode, reason,
                                        echConfigUsed);
    }
  }

  HttpConnectionInfoCloneArgs infoArgs;
  nsHttpConnectionInfo::SerializeHttpConnectionInfo(connInfo, infoArgs);

  Unused << SendOnStartRequest(
      status, std::move(optionalHead), securityInfo,
      mTransaction->ProxyConnectFailed(),
      ToTimingStructArgs(mTransaction->Timings()), proxyConnectResponseCode,
      dataForSniffer, optionalAltSvcUsed, !!mDataBridgeParent,
      mTransaction->TakeRestartedState(), mTransaction->HTTPSSVCReceivedStage(),
      mTransaction->GetSupportsHTTP3(), mode, reason, mTransaction->Caps(),
      TimeStamp::Now(), infoArgs, mTransaction->GetTargetIPAddressSpace());
  return NS_OK;
}

ResourceTimingStructArgs HttpTransactionChild::GetTimingAttributes() {
  // Note that not all fields in ResourceTimingStructArgs are filled, since
  // we only need some in HttpChannelChild::OnStopRequest.
  ResourceTimingStructArgs args;
  args.domainLookupStart() = mTransaction->GetDomainLookupStart();
  args.domainLookupEnd() = mTransaction->GetDomainLookupEnd();
  args.connectStart() = mTransaction->GetConnectStart();
  args.tcpConnectEnd() = mTransaction->GetTcpConnectEnd();
  args.secureConnectionStart() = mTransaction->GetSecureConnectionStart();
  args.connectEnd() = mTransaction->GetConnectEnd();
  args.requestStart() = mTransaction->GetRequestStart();
  args.responseStart() = mTransaction->GetResponseStart();
  args.responseEnd() = mTransaction->GetResponseEnd();
  args.transferSize() = mTransaction->GetTransferSize();
  args.encodedBodySize() = mLogicalOffset;
  args.redirectStart() = mRedirectStart;
  args.redirectEnd() = mRedirectEnd;
  args.transferSize() = mTransaction->GetTransferSize();
  args.transactionPending() = mTransaction->GetPendingTime();
  return args;
}

NS_IMETHODIMP
HttpTransactionChild::OnStopRequest(nsIRequest* aRequest, nsresult aStatus) {
  LOG(("HttpTransactionChild::OnStopRequest [this=%p]\n", this));

  mTransactionPump = nullptr;

  auto onStopGuard = MakeScopeExit([&] {
    LOG(("  calling mDataBridgeParent->OnStopRequest by ScopeExit [this=%p]\n",
         this));
    MOZ_ASSERT(NS_FAILED(mStatus), "This shoule be only called when failure");
    if (mDataBridgeParent) {
      mDataBridgeParent->OnStopRequest(mStatus, ResourceTimingStructArgs(),
                                       TimeStamp(), nsHttpHeaderArray(),
                                       TimeStamp::Now());
      mDataBridgeParent = nullptr;
    }
  });

  // Don't bother sending IPC to parent process if already canceled.
  if (mCanceled) {
    return mStatus;
  }

  if (!CanSend()) {
    mStatus = NS_ERROR_UNEXPECTED;
    return mStatus;
  }

  MOZ_ASSERT(mTransaction);

  UniquePtr<nsHttpHeaderArray> headerArray(
      mTransaction->TakeResponseTrailers());
  Maybe<nsHttpHeaderArray> responseTrailers;
  if (headerArray) {
    responseTrailers.emplace(*headerArray);
  }

  onStopGuard.release();

  TimeStamp lastActTabOpt = nsHttp::GetLastActiveTabLoadOptimizationHit();

  if (mDataBridgeParent) {
    mDataBridgeParent->OnStopRequest(
        aStatus, GetTimingAttributes(), lastActTabOpt,
        responseTrailers ? *responseTrailers : nsHttpHeaderArray(),
        TimeStamp::Now());
    mDataBridgeParent = nullptr;
  }

  Unused << SendOnStopRequest(aStatus, mTransaction->ResponseIsComplete(),
                              mTransaction->GetTransferSize(),
                              ToTimingStructArgs(mTransaction->Timings()),
                              responseTrailers, mTransactionObserverResult,
                              lastActTabOpt, TimeStamp::Now());

  return NS_OK;
}

//-----------------------------------------------------------------------------
// HttpTransactionChild <nsITransportEventSink>
//-----------------------------------------------------------------------------

NS_IMETHODIMP
HttpTransactionChild::OnTransportStatus(nsITransport* aTransport,
                                        nsresult aStatus, int64_t aProgress,
                                        int64_t aProgressMax) {
  LOG(("HttpTransactionChild::OnTransportStatus [this=%p status=%" PRIx32
       " progress=%" PRId64 "]\n",
       this, static_cast<uint32_t>(aStatus), aProgress));

  if (!CanSend()) {
    return NS_OK;
  }

  Maybe<NetworkAddressArg> arg;
  if (aStatus == NS_NET_STATUS_CONNECTED_TO ||
      aStatus == NS_NET_STATUS_WAITING_FOR) {
    NetAddr selfAddr;
    NetAddr peerAddr;
    bool isTrr = false;
    bool echConfigUsed = false;
    nsIRequest::TRRMode mode = nsIRequest::TRR_DEFAULT_MODE;
    TRRSkippedReason reason = nsITRRSkipReason::TRR_UNSET;
    if (mTransaction) {
      mTransaction->GetNetworkAddresses(selfAddr, peerAddr, isTrr, mode, reason,
                                        echConfigUsed);
    } else {
      nsCOMPtr<nsISocketTransport> socketTransport =
          do_QueryInterface(aTransport);
      if (socketTransport) {
        socketTransport->GetSelfAddr(&selfAddr);
        socketTransport->GetPeerAddr(&peerAddr);
        socketTransport->ResolvedByTRR(&isTrr);
        socketTransport->GetEffectiveTRRMode(&mode);
        socketTransport->GetTrrSkipReason(&reason);
        socketTransport->GetEchConfigUsed(&echConfigUsed);
      }
    }
    arg.emplace(selfAddr, peerAddr, isTrr, mode, reason, echConfigUsed);
  }

  Unused << SendOnTransportStatus(aStatus, aProgress, aProgressMax, arg);
  return NS_OK;
}

//-----------------------------------------------------------------------------
// HttpBaseChannel::nsIThrottledInputChannel
//-----------------------------------------------------------------------------

NS_IMETHODIMP
HttpTransactionChild::SetThrottleQueue(nsIInputChannelThrottleQueue* aQueue) {
  return NS_ERROR_NOT_IMPLEMENTED;
}

NS_IMETHODIMP
HttpTransactionChild::GetThrottleQueue(nsIInputChannelThrottleQueue** aQueue) {
  nsCOMPtr<nsIInputChannelThrottleQueue> queue =
      static_cast<nsIInputChannelThrottleQueue*>(mThrottleQueue.get());
  queue.forget(aQueue);
  return NS_OK;
}

//-----------------------------------------------------------------------------
// EventSourceImpl::nsIThreadRetargetableStreamListener
//-----------------------------------------------------------------------------
NS_IMETHODIMP
HttpTransactionChild::CheckListenerChain() {
  MOZ_ASSERT(NS_IsMainThread(), "Should be on the main thread!");
  return NS_OK;
}

NS_IMETHODIMP
HttpTransactionChild::OnDataFinished(nsresult aStatus) { return NS_OK; }

NS_IMETHODIMP
HttpTransactionChild::EarlyHint(const nsACString& aValue,
                                const nsACString& aReferrerPolicy,
                                const nsACString& aCSPHeader) {
  LOG(("HttpTransactionChild::EarlyHint"));
  if (CanSend()) {
    Unused << SendEarlyHint(aValue, aReferrerPolicy, aCSPHeader);
  }
  return NS_OK;
}

}  // namespace mozilla::net
