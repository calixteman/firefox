/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "RemoteEncoderModule.h"
#include "RemoteDecodeUtils.h"
#include "RemoteMediaDataEncoderChild.h"
#include "RemoteMediaManagerChild.h"

#ifdef MOZ_APPLEMEDIA
#  include "AppleUtils.h"
#endif

namespace mozilla {

extern LazyLogModule sPEMLog;

RemoteEncoderModule::RemoteEncoderModule(RemoteMediaIn aLocation)
    : mLocation(aLocation) {}

/* static */ already_AddRefed<PlatformEncoderModule>
RemoteEncoderModule::Create(RemoteMediaIn aLocation) {
  if (!XRE_IsContentProcess()) {
    // For now, the RemoteEncoderModule is only available in the content
    // process.
    MOZ_ASSERT_UNREACHABLE("Should not be created outside content process.");
    return nullptr;
  }

  RemoteMediaManagerChild::Init();
  return MakeAndAddRef<RemoteEncoderModule>(aLocation);
}

const char* RemoteEncoderModule::GetName() const {
  switch (mLocation) {
    case RemoteMediaIn::RddProcess:
      return "Remote Encoder Module (RDD)";
    case RemoteMediaIn::GpuProcess:
      return "Remote Encoder Module (GPU)";
    case RemoteMediaIn::UtilityProcess_Generic:
      return "Remote Encoder Module (Utility)";
    case RemoteMediaIn::UtilityProcess_AppleMedia:
      return "Remote Encoder Module (Utility AppleMedia)";
    case RemoteMediaIn::UtilityProcess_WMF:
      return "Remote Encoder Module (Utility WMF)";
    default:
      return "Remote Encoder Module";
  }
}

already_AddRefed<MediaDataEncoder> RemoteEncoderModule::CreateEncoder(
    const EncoderConfig& aConfig, const RefPtr<TaskQueue>& aTaskQueue) const {
  nsCOMPtr<nsISerialEventTarget> thread =
      RemoteMediaManagerChild::GetManagerThread();
  if (!thread) {
    // Shutdown has begun.
    MOZ_LOG(sPEMLog, LogLevel::Debug,
            ("Sandbox %s encoder requested codec %d after shutdown",
             RemoteMediaInToStr(mLocation), static_cast<int>(aConfig.mCodec)));
    return nullptr;
  }

  auto encoder =
      MakeRefPtr<RemoteMediaDataEncoderChild>(std::move(thread), mLocation);

  // This returns a promise, but we know that once it returns, the only
  // interactions the caller can do will require a dispatch to the manager
  // thread. The necessary IPDL constructor events are already queued so the
  // order of events is preserved.
  RemoteMediaManagerChild::InitializeEncoder(RefPtr{encoder}, aConfig);

  return encoder.forget();
}

RefPtr<PlatformEncoderModule::CreateEncoderPromise>
RemoteEncoderModule::AsyncCreateEncoder(const EncoderConfig& aEncoderConfig,
                                        const RefPtr<TaskQueue>& aTaskQueue) {
  nsCOMPtr<nsISerialEventTarget> thread =
      RemoteMediaManagerChild::GetManagerThread();
  if (!thread) {
    // Shutdown has begun.
    MOZ_LOG(sPEMLog, LogLevel::Debug,
            ("Sandbox %s encoder requested codec %d after shutdown",
             RemoteMediaInToStr(mLocation),
             static_cast<int>(aEncoderConfig.mCodec)));
    return PlatformEncoderModule::CreateEncoderPromise::CreateAndReject(
        MediaResult(NS_ERROR_DOM_MEDIA_CANCELED,
                    "Remote manager not available"),
        __func__);
  }

  auto encoder =
      MakeRefPtr<RemoteMediaDataEncoderChild>(std::move(thread), mLocation);
  return RemoteMediaManagerChild::InitializeEncoder(std::move(encoder),
                                                    aEncoderConfig);
}

media::EncodeSupportSet RemoteEncoderModule::Supports(
    const EncoderConfig& aConfig) const {
  if (!CanLikelyEncode(aConfig)) {
    return media::EncodeSupportSet{};
  }

  // TODO(aosmond): The platform specific criteria were copied from the various
  // PEMs in order to pass the WebCodecs WPTs but should eventually be rewritten
  // to generically support any PEM.

#ifdef MOZ_APPLEMEDIA
  // Only two temporal layers supported, and only from 11.3 and more recent
  if (aConfig.mCodec == CodecType::H264 &&
      (aConfig.mScalabilityMode == ScalabilityMode::L1T3 ||
       (aConfig.mScalabilityMode != ScalabilityMode::None &&
        !OSSupportsSVC()))) {
    return media::EncodeSupportSet{};
  }
#endif

#ifdef XP_WIN
  if (aConfig.mScalabilityMode != ScalabilityMode::None) {
    switch (aConfig.mCodec) {
      case CodecType::H264:
      case CodecType::VP8:
      case CodecType::VP9:
        // The codec type support check is sufficient.
        break;
      case CodecType::AV1:
        if (aConfig.mBitrateMode != BitrateMode::Constant) {
          return media::EncodeSupportSet{};
        }
        break;
      default:
        return media::EncodeSupportSet{};
    }
  }
#endif

  return SupportsCodec(aConfig.mCodec);
}

media::EncodeSupportSet RemoteEncoderModule::SupportsCodec(
    CodecType aCodecType) const {
  media::EncodeSupportSet supports =
      RemoteMediaManagerChild::Supports(mLocation, aCodecType);
  MOZ_LOG(sPEMLog, LogLevel::Debug,
          ("Sandbox %s encoder %s requested codec %d",
           RemoteMediaInToStr(mLocation),
           supports.isEmpty() ? "supports" : "rejects",
           static_cast<int>(aCodecType)));
  return supports;
}

}  // namespace mozilla
