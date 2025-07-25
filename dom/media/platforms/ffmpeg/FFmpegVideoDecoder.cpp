/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim:set ts=2 sw=2 sts=2 et cindent: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "FFmpegVideoDecoder.h"

#include "EncoderConfig.h"
#include "FFmpegLog.h"
#include "FFmpegUtils.h"
#include "ImageContainer.h"
#include "MP4Decoder.h"
#include "MediaInfo.h"
#include "VALibWrapper.h"
#include "VideoUtils.h"
#include "VPXDecoder.h"
#include "mozilla/layers/KnowsCompositor.h"
#include "nsPrintfCString.h"
#if LIBAVCODEC_VERSION_MAJOR >= 57
#  include "mozilla/layers/TextureClient.h"
#endif
#if LIBAVCODEC_VERSION_MAJOR >= 58
#  include "mozilla/ProfilerMarkers.h"
#endif
#ifdef MOZ_USE_HWDECODE
#  include "H264.h"
#  include "H265.h"
#endif
#if defined(MOZ_USE_HWDECODE) && defined(MOZ_WIDGET_GTK)
#  include "mozilla/gfx/gfxVars.h"
#  include "mozilla/layers/DMABUFSurfaceImage.h"
#  include "FFmpegVideoFramePool.h"
#  include "va/va.h"
#endif

#if defined(MOZ_AV1) && \
    (defined(FFVPX_VERSION) || LIBAVCODEC_VERSION_MAJOR >= 59)
#  define FFMPEG_AV1_DECODE 1
#  include "AOMDecoder.h"
#endif

#if LIBAVCODEC_VERSION_MAJOR < 54
#  define AV_PIX_FMT_YUV420P PIX_FMT_YUV420P
#  define AV_PIX_FMT_YUVJ420P PIX_FMT_YUVJ420P
#  define AV_PIX_FMT_YUV420P10LE PIX_FMT_YUV420P10LE
#  define AV_PIX_FMT_YUV422P PIX_FMT_YUV422P
#  define AV_PIX_FMT_YUV422P10LE PIX_FMT_YUV422P10LE
#  define AV_PIX_FMT_YUV444P PIX_FMT_YUV444P
#  define AV_PIX_FMT_YUVJ444P PIX_FMT_YUVJ444P
#  define AV_PIX_FMT_YUV444P10LE PIX_FMT_YUV444P10LE
#  define AV_PIX_FMT_GBRP PIX_FMT_GBRP
#  define AV_PIX_FMT_GBRP10LE PIX_FMT_GBRP10LE
#  define AV_PIX_FMT_NONE PIX_FMT_NONE
#  define AV_PIX_FMT_VAAPI_VLD PIX_FMT_VAAPI_VLD
#endif
#if LIBAVCODEC_VERSION_MAJOR > 58
#  define AV_PIX_FMT_VAAPI_VLD AV_PIX_FMT_VAAPI
#endif
#include "mozilla/PodOperations.h"
#include "mozilla/StaticPrefs_gfx.h"
#include "mozilla/StaticPrefs_media.h"
#include "mozilla/TaskQueue.h"
#include "nsThreadUtils.h"
#include "prsystem.h"

#ifdef XP_WIN
#  include "mozilla/gfx/DeviceManagerDx.h"
#  include "mozilla/gfx/gfxVars.h"
#endif

#ifdef MOZ_ENABLE_D3D11VA
#  include "D3D11TextureWrapper.h"
#  include "DXVA2Manager.h"
#  include "ffvpx/hwcontext_d3d11va.h"
#endif

// Use some extra HW frames for potential rendering lags.
// AV1 and VP9 can have maximum 8 frames for reference frames, so 1 base + 8
// references.
#define EXTRA_HW_FRAMES 9

#if LIBAVCODEC_VERSION_MAJOR >= 57 && LIBAVUTIL_VERSION_MAJOR >= 56
#  define CUSTOMIZED_BUFFER_ALLOCATION 1
#endif

#define AV_LOG_DEBUG 48

typedef mozilla::layers::Image Image;
typedef mozilla::layers::PlanarYCbCrImage PlanarYCbCrImage;
typedef mozilla::layers::BufferRecycleBin BufferRecycleBin;

namespace mozilla {

#if defined(MOZ_USE_HWDECODE) && defined(MOZ_WIDGET_GTK)
MOZ_RUNINIT nsTArray<AVCodecID>
    FFmpegVideoDecoder<LIBAV_VER>::mAcceleratedFormats;
#endif

using media::TimeUnit;

/**
 * FFmpeg calls back to this function with a list of pixel formats it supports.
 * We choose a pixel format that we support and return it.
 * For now, we just look for YUV420P, YUVJ420P, YUV444 and YUVJ444 as
 * those are the only non-HW accelerated format supported by FFmpeg's H264 and
 * VP9 decoder.
 */
static AVPixelFormat ChoosePixelFormat(AVCodecContext* aCodecContext,
                                       const AVPixelFormat* aFormats) {
  FFMPEGV_LOG("Choosing FFmpeg pixel format for video decoding.");
  for (; *aFormats > -1; aFormats++) {
    switch (*aFormats) {
      case AV_PIX_FMT_YUV420P:
        FFMPEGV_LOG("Requesting pixel format YUV420P.");
        return AV_PIX_FMT_YUV420P;
      case AV_PIX_FMT_YUVJ420P:
        FFMPEGV_LOG("Requesting pixel format YUVJ420P.");
        return AV_PIX_FMT_YUVJ420P;
      case AV_PIX_FMT_YUV420P10LE:
        FFMPEGV_LOG("Requesting pixel format YUV420P10LE.");
        return AV_PIX_FMT_YUV420P10LE;
      case AV_PIX_FMT_YUV422P:
        FFMPEGV_LOG("Requesting pixel format YUV422P.");
        return AV_PIX_FMT_YUV422P;
      case AV_PIX_FMT_YUV422P10LE:
        FFMPEGV_LOG("Requesting pixel format YUV422P10LE.");
        return AV_PIX_FMT_YUV422P10LE;
      case AV_PIX_FMT_YUV444P:
        FFMPEGV_LOG("Requesting pixel format YUV444P.");
        return AV_PIX_FMT_YUV444P;
      case AV_PIX_FMT_YUVJ444P:
        FFMPEGV_LOG("Requesting pixel format YUVJ444P.");
        return AV_PIX_FMT_YUVJ444P;
      case AV_PIX_FMT_YUV444P10LE:
        FFMPEGV_LOG("Requesting pixel format YUV444P10LE.");
        return AV_PIX_FMT_YUV444P10LE;
#if LIBAVCODEC_VERSION_MAJOR >= 57
      case AV_PIX_FMT_YUV420P12LE:
        FFMPEGV_LOG("Requesting pixel format YUV420P12LE.");
        return AV_PIX_FMT_YUV420P12LE;
      case AV_PIX_FMT_YUV422P12LE:
        FFMPEGV_LOG("Requesting pixel format YUV422P12LE.");
        return AV_PIX_FMT_YUV422P12LE;
      case AV_PIX_FMT_YUV444P12LE:
        FFMPEGV_LOG("Requesting pixel format YUV444P12LE.");
        return AV_PIX_FMT_YUV444P12LE;
#endif
      case AV_PIX_FMT_GBRP:
        FFMPEGV_LOG("Requesting pixel format GBRP.");
        return AV_PIX_FMT_GBRP;
      case AV_PIX_FMT_GBRP10LE:
        FFMPEGV_LOG("Requesting pixel format GBRP10LE.");
        return AV_PIX_FMT_GBRP10LE;
      default:
        break;
    }
  }

  NS_WARNING("FFmpeg does not share any supported pixel formats.");
  return AV_PIX_FMT_NONE;
}

#ifdef MOZ_USE_HWDECODE
static AVPixelFormat ChooseVAAPIPixelFormat(AVCodecContext* aCodecContext,
                                            const AVPixelFormat* aFormats) {
  FFMPEGV_LOG("Choosing FFmpeg pixel format for VA-API video decoding.");
  for (; *aFormats > -1; aFormats++) {
    switch (*aFormats) {
      case AV_PIX_FMT_VAAPI_VLD:
        FFMPEGV_LOG("Requesting pixel format VAAPI_VLD");
        return AV_PIX_FMT_VAAPI_VLD;
      default:
        break;
    }
  }
  NS_WARNING("FFmpeg does not share any supported pixel formats.");
  return AV_PIX_FMT_NONE;
}

static AVPixelFormat ChooseV4L2PixelFormat(AVCodecContext* aCodecContext,
                                           const AVPixelFormat* aFormats) {
  FFMPEGV_LOG("Choosing FFmpeg pixel format for V4L2 video decoding.");
  for (; *aFormats > -1; aFormats++) {
    switch (*aFormats) {
      case AV_PIX_FMT_DRM_PRIME:
        FFMPEGV_LOG("Requesting pixel format DRM PRIME");
        return AV_PIX_FMT_DRM_PRIME;
      default:
        break;
    }
  }
  NS_WARNING("FFmpeg does not share any supported V4L2 pixel formats.");
  return AV_PIX_FMT_NONE;
}

static AVPixelFormat ChooseD3D11VAPixelFormat(AVCodecContext* aCodecContext,
                                              const AVPixelFormat* aFormats) {
#  ifdef MOZ_ENABLE_D3D11VA
  FFMPEGV_LOG("Choosing FFmpeg pixel format for D3D11VA video decoding %d. ",
              *aFormats);
  for (; *aFormats > -1; aFormats++) {
    switch (*aFormats) {
      case AV_PIX_FMT_D3D11:
        FFMPEGV_LOG("Requesting pixel format D3D11");
        return AV_PIX_FMT_D3D11;
      default:
        break;
    }
  }
  NS_WARNING("FFmpeg does not share any supported D3D11 pixel formats.");
#  endif  // MOZ_ENABLE_D3D11VA
  return AV_PIX_FMT_NONE;
}
#endif

#if defined(MOZ_USE_HWDECODE) && defined(MOZ_WIDGET_GTK)
static void VAAPIDisplayReleaseCallback(struct AVHWDeviceContext* hwctx) {
  auto displayHolder = static_cast<VADisplayHolder*>(hwctx->user_opaque);
  displayHolder->Release();
}

bool FFmpegVideoDecoder<LIBAV_VER>::CreateVAAPIDeviceContext() {
  mVAAPIDeviceContext = mLib->av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VAAPI);
  if (!mVAAPIDeviceContext) {
    FFMPEG_LOG("  av_hwdevice_ctx_alloc failed.");
    return false;
  }

  auto releaseVAAPIcontext =
      MakeScopeExit([&] { mLib->av_buffer_unref(&mVAAPIDeviceContext); });

  AVHWDeviceContext* hwctx = (AVHWDeviceContext*)mVAAPIDeviceContext->data;
  AVVAAPIDeviceContext* vactx = (AVVAAPIDeviceContext*)hwctx->hwctx;

  RefPtr displayHolder = VADisplayHolder::GetSingleton();
  if (!displayHolder) {
    return false;
  }

  mDisplay = displayHolder->Display();
  hwctx->user_opaque = displayHolder.forget().take();
  hwctx->free = VAAPIDisplayReleaseCallback;

  vactx->display = mDisplay;
  if (mLib->av_hwdevice_ctx_init(mVAAPIDeviceContext) < 0) {
    FFMPEG_LOG("  av_hwdevice_ctx_init failed.");
    return false;
  }

  mCodecContext->hw_device_ctx = mLib->av_buffer_ref(mVAAPIDeviceContext);
  releaseVAAPIcontext.release();
  return true;
}

void FFmpegVideoDecoder<LIBAV_VER>::AdjustHWDecodeLogging() {
  if (!getenv("LIBVA_MESSAGING_LEVEL")) {
    if (MOZ_LOG_TEST(sFFmpegVideoLog, LogLevel::Debug)) {
      setenv("LIBVA_MESSAGING_LEVEL", "1", false);
    } else if (MOZ_LOG_TEST(sFFmpegVideoLog, LogLevel::Info)) {
      setenv("LIBVA_MESSAGING_LEVEL", "2", false);
    } else {
      setenv("LIBVA_MESSAGING_LEVEL", "0", false);
    }
  }
}

MediaResult FFmpegVideoDecoder<LIBAV_VER>::InitVAAPIDecoder() {
  FFMPEG_LOG("Initialising VA-API FFmpeg decoder");

  StaticMutexAutoLock mon(sMutex);

  // mAcceleratedFormats is already configured so check supported
  // formats before we do anything.
  if (mAcceleratedFormats.Length()) {
    if (!IsFormatAccelerated(mCodecID)) {
      FFMPEG_LOG("  Format %s is not accelerated",
                 mLib->avcodec_get_name(mCodecID));
      return NS_ERROR_NOT_AVAILABLE;
    } else {
      FFMPEG_LOG("  Format %s is accelerated",
                 mLib->avcodec_get_name(mCodecID));
    }
  }

  if (!mLib->IsVAAPIAvailable()) {
    FFMPEG_LOG("  libva library or symbols are missing.");
    return NS_ERROR_NOT_AVAILABLE;
  }

  AVCodec* codec =
      FindVideoHardwareAVCodec(mLib, mCodecID, AV_HWDEVICE_TYPE_VAAPI);
  if (!codec) {
    FFMPEG_LOG("  couldn't find ffmpeg VA-API decoder");
    return NS_ERROR_DOM_MEDIA_FATAL_ERR;
  }
  // This logic is mirrored in FFmpegDecoderModule::Supports. We prefer to use
  // our own OpenH264 decoder through the plugin over ffmpeg by default due to
  // broken decoding with some versions. openh264 has broken decoding of some
  // h264 videos so don't use it unless explicitly allowed for now.
  if (!strcmp(codec->name, "libopenh264") &&
      !StaticPrefs::media_ffmpeg_allow_openh264()) {
    FFMPEG_LOG("  unable to find codec (openh264 disabled by pref)");
    return MediaResult(
        NS_ERROR_DOM_MEDIA_FATAL_ERR,
        RESULT_DETAIL("unable to find codec (openh264 disabled by pref)"));
  }
  FFMPEG_LOG("  codec %s : %s", codec->name, codec->long_name);

  if (!(mCodecContext = mLib->avcodec_alloc_context3(codec))) {
    FFMPEG_LOG("  couldn't init VA-API ffmpeg context");
    return NS_ERROR_OUT_OF_MEMORY;
  }
  mCodecContext->opaque = this;

  InitHWCodecContext(ContextType::VAAPI);

  auto releaseVAAPIdecoder = MakeScopeExit([&] {
    if (mVAAPIDeviceContext) {
      mLib->av_buffer_unref(&mVAAPIDeviceContext);
    }
    if (mCodecContext) {
      mLib->av_freep(&mCodecContext);
    }
  });

  if (!CreateVAAPIDeviceContext()) {
    mLib->av_freep(&mCodecContext);
    FFMPEG_LOG("  Failed to create VA-API device context");
    return NS_ERROR_DOM_MEDIA_FATAL_ERR;
  }

  MediaResult ret = AllocateExtraData();
  if (NS_FAILED(ret)) {
    mLib->av_buffer_unref(&mVAAPIDeviceContext);
    mLib->av_freep(&mCodecContext);
    return ret;
  }

  if (mLib->avcodec_open2(mCodecContext, codec, nullptr) < 0) {
    mLib->av_buffer_unref(&mVAAPIDeviceContext);
    mLib->av_freep(&mCodecContext);
    FFMPEG_LOG("  Couldn't initialise VA-API decoder");
    return NS_ERROR_DOM_MEDIA_FATAL_ERR;
  }

  if (mAcceleratedFormats.IsEmpty()) {
    mAcceleratedFormats = GetAcceleratedFormats();
    if (!IsFormatAccelerated(mCodecID)) {
      FFMPEG_LOG("  Format %s is not accelerated",
                 mLib->avcodec_get_name(mCodecID));
      return NS_ERROR_NOT_AVAILABLE;
    }
  }

  AdjustHWDecodeLogging();

  FFMPEG_LOG("  VA-API FFmpeg init successful");
  releaseVAAPIdecoder.release();
  return NS_OK;
}

MediaResult FFmpegVideoDecoder<LIBAV_VER>::InitV4L2Decoder() {
  FFMPEG_LOG("Initialising V4L2-DRM FFmpeg decoder");

  StaticMutexAutoLock mon(sMutex);

  // mAcceleratedFormats is already configured so check supported
  // formats before we do anything.
  if (mAcceleratedFormats.Length()) {
    if (!IsFormatAccelerated(mCodecID)) {
      FFMPEG_LOG("  Format %s is not accelerated",
                 mLib->avcodec_get_name(mCodecID));
      return NS_ERROR_NOT_AVAILABLE;
    }
    FFMPEG_LOG("  Format %s is accelerated", mLib->avcodec_get_name(mCodecID));
  }

  // Select the appropriate v4l2 codec
  AVCodec* codec = FindVideoHardwareAVCodec(mLib, mCodecID);
  if (!codec) {
    FFMPEG_LOG("No appropriate v4l2 codec found");
    return NS_ERROR_DOM_MEDIA_FATAL_ERR;
  }
  FFMPEG_LOG("  V4L2 codec %s : %s", codec->name, codec->long_name);

  if (!(mCodecContext = mLib->avcodec_alloc_context3(codec))) {
    FFMPEG_LOG("  couldn't init HW ffmpeg context");
    return NS_ERROR_OUT_OF_MEMORY;
  }
  mCodecContext->opaque = this;

  InitHWCodecContext(ContextType::V4L2);

  // Disable cropping in FFmpeg.  Because our frames are opaque DRM buffers
  // FFmpeg can't actually crop them and it tries to do so by just modifying
  // the width and height.  This causes problems because V4L2 outputs a single
  // buffer/layer/plane with all three planes stored contiguously.  We need to
  // know the offsets to each plane, and if FFmpeg applies cropping (and then
  // we can't find out what the original uncropped width/height was) then we
  // can't work out the offsets.
  mCodecContext->apply_cropping = 0;

  auto releaseDecoder = MakeScopeExit([&] {
    if (mCodecContext) {
      mLib->av_freep(&mCodecContext);
    }
  });

  MediaResult ret = AllocateExtraData();
  if (NS_FAILED(ret)) {
    mLib->av_freep(&mCodecContext);
    return ret;
  }

  if (mLib->avcodec_open2(mCodecContext, codec, nullptr) < 0) {
    mLib->av_freep(&mCodecContext);
    FFMPEG_LOG("  Couldn't initialise V4L2 decoder");
    return NS_ERROR_DOM_MEDIA_FATAL_ERR;
  }

  // Set mAcceleratedFormats
  if (mAcceleratedFormats.IsEmpty()) {
    // FFmpeg does not correctly report that the V4L2 wrapper decoders are
    // hardware accelerated, but we know they always are.  If we've gotten
    // this far then we know this codec has a V4L2 wrapper decoder and so is
    // accelerateed.
    mAcceleratedFormats.AppendElement(mCodecID);
  }

  AdjustHWDecodeLogging();

  FFMPEG_LOG("  V4L2 FFmpeg init successful");
  mUsingV4L2 = true;
  releaseDecoder.release();
  return NS_OK;
}
#endif

#if LIBAVCODEC_VERSION_MAJOR < 58
FFmpegVideoDecoder<LIBAV_VER>::PtsCorrectionContext::PtsCorrectionContext()
    : mNumFaultyPts(0),
      mNumFaultyDts(0),
      mLastPts(INT64_MIN),
      mLastDts(INT64_MIN) {}

int64_t FFmpegVideoDecoder<LIBAV_VER>::PtsCorrectionContext::GuessCorrectPts(
    int64_t aPts, int64_t aDts) {
  int64_t pts = AV_NOPTS_VALUE;

  if (aDts != int64_t(AV_NOPTS_VALUE)) {
    mNumFaultyDts += aDts <= mLastDts;
    mLastDts = aDts;
  }
  if (aPts != int64_t(AV_NOPTS_VALUE)) {
    mNumFaultyPts += aPts <= mLastPts;
    mLastPts = aPts;
  }
  if ((mNumFaultyPts <= mNumFaultyDts || aDts == int64_t(AV_NOPTS_VALUE)) &&
      aPts != int64_t(AV_NOPTS_VALUE)) {
    pts = aPts;
  } else {
    pts = aDts;
  }
  return pts;
}

void FFmpegVideoDecoder<LIBAV_VER>::PtsCorrectionContext::Reset() {
  mNumFaultyPts = 0;
  mNumFaultyDts = 0;
  mLastPts = INT64_MIN;
  mLastDts = INT64_MIN;
}
#endif

#if defined(MOZ_USE_HWDECODE) && defined(MOZ_WIDGET_GTK)
bool FFmpegVideoDecoder<LIBAV_VER>::ShouldEnableLinuxHWDecoding() const {
  bool supported = false;
  switch (mCodecID) {
    case AV_CODEC_ID_H264:
      supported = gfx::gfxVars::UseH264HwDecode();
      break;
    case AV_CODEC_ID_VP8:
      supported = gfx::gfxVars::UseVP8HwDecode();
      break;
    case AV_CODEC_ID_VP9:
      supported = gfx::gfxVars::UseVP9HwDecode();
      break;
    case AV_CODEC_ID_AV1:
      supported = gfx::gfxVars::UseAV1HwDecode();
      break;
    case AV_CODEC_ID_HEVC:
      supported = gfx::gfxVars::UseHEVCHwDecode();
      break;
    default:
      break;
  }
  if (!supported) {
    FFMPEG_LOG("Codec %s is not accelerated", mLib->avcodec_get_name(mCodecID));
    return false;
  }

  bool isHardwareWebRenderUsed = mImageAllocator &&
                                 (mImageAllocator->GetCompositorBackendType() ==
                                  layers::LayersBackend::LAYERS_WR) &&
                                 !mImageAllocator->UsingSoftwareWebRender();
  if (!isHardwareWebRenderUsed) {
    FFMPEG_LOG("Hardware WebRender is off, VAAPI is disabled");
    return false;
  }
  if (!XRE_IsRDDProcess()) {
    FFMPEG_LOG("VA-API works in RDD process only");
    return false;
  }
  return true;
}
#endif

#if defined(MOZ_WIDGET_GTK) && defined(MOZ_USE_HWDECODE)
bool FFmpegVideoDecoder<LIBAV_VER>::UploadSWDecodeToDMABuf() const {
  // Use direct DMABuf upload for GL backend Wayland compositor only.
  return mImageAllocator && (mImageAllocator->GetCompositorBackendType() ==
                                 layers::LayersBackend::LAYERS_WR &&
                             !mImageAllocator->UsingSoftwareWebRender() &&
                             mImageAllocator->GetWebRenderCompositorType() ==
                                 layers::WebRenderCompositor::WAYLAND);
}
#endif

FFmpegVideoDecoder<LIBAV_VER>::FFmpegVideoDecoder(
    FFmpegLibWrapper* aLib, const VideoInfo& aConfig,
    KnowsCompositor* aAllocator, ImageContainer* aImageContainer,
    bool aLowLatency, bool aDisableHardwareDecoding, bool a8BitOutput,
    Maybe<TrackingId> aTrackingId)
    : FFmpegDataDecoder(aLib, GetCodecId(aConfig.mMimeType)),
      mImageAllocator(aAllocator),
#ifdef MOZ_USE_HWDECODE
#  ifdef MOZ_WIDGET_GTK
      mHardwareDecodingDisabled(aDisableHardwareDecoding ||
                                !ShouldEnableLinuxHWDecoding()),
#  else
      mHardwareDecodingDisabled(aDisableHardwareDecoding),
#  endif  // MOZ_WIDGET_GTK
#endif    // MOZ_USE_HWDECODE
      mImageContainer(aImageContainer),
      mInfo(aConfig),
      mLowLatency(aLowLatency),
      mTrackingId(std::move(aTrackingId)),
      // Value may be changed later when codec is known after initialization.
      m8BitOutput(a8BitOutput) {
  FFMPEG_LOG("FFmpegVideoDecoder::FFmpegVideoDecoder MIME %s Codec ID %d",
             aConfig.mMimeType.get(), mCodecID);
  // Use a new MediaByteBuffer as the object will be modified during
  // initialization.
  mExtraData = new MediaByteBuffer;
  mExtraData->AppendElements(*aConfig.mExtraData);
#if defined(MOZ_WIDGET_GTK) && defined(MOZ_USE_HWDECODE)
  mUploadSWDecodeToDMABuf = UploadSWDecodeToDMABuf();
#endif
#ifdef MOZ_USE_HWDECODE
  InitHWDecoderIfAllowed();
#endif  // MOZ_USE_HWDECODE
}

FFmpegVideoDecoder<LIBAV_VER>::~FFmpegVideoDecoder() {
#ifdef CUSTOMIZED_BUFFER_ALLOCATION
  MOZ_DIAGNOSTIC_ASSERT(mAllocatedImages.IsEmpty(),
                        "Should release all shmem buffers before destroy!");
#endif
}

#ifdef MOZ_USE_HWDECODE
void FFmpegVideoDecoder<LIBAV_VER>::InitHWDecoderIfAllowed() {
  if (mHardwareDecodingDisabled) {
    return;
  }

#  ifdef MOZ_ENABLE_VAAPI
  if (NS_SUCCEEDED(InitVAAPIDecoder())) {
    return;
  }
#  endif  // MOZ_ENABLE_VAAPI

#  ifdef MOZ_ENABLE_V4L2
  // VAAPI didn't work or is disabled, so try V4L2 with DRM
  if (NS_SUCCEEDED(InitV4L2Decoder())) {
    return;
  }
#  endif  // MOZ_ENABLE_V4L2

#  ifdef MOZ_ENABLE_D3D11VA
  if (XRE_IsGPUProcess() && NS_SUCCEEDED(InitD3D11VADecoder())) {
    return;
  }
#  endif  // MOZ_ENABLE_D3D11VA
}
#endif  // MOZ_USE_HWDECODE

RefPtr<MediaDataDecoder::InitPromise> FFmpegVideoDecoder<LIBAV_VER>::Init() {
  FFMPEG_LOG("FFmpegVideoDecoder, init, IsHardwareAccelerated=%d\n",
             IsHardwareAccelerated());
  // We've finished the HW decoder initialization in the ctor.
  if (IsHardwareAccelerated()) {
    return InitPromise::CreateAndResolve(TrackInfo::kVideoTrack, __func__);
  }
  MediaResult rv = InitSWDecoder(nullptr);
  if (NS_FAILED(rv)) {
    return InitPromise::CreateAndReject(rv, __func__);
  }
  // Enable 8-bit conversion only for dav1d.
  m8BitOutput =
      m8BitOutput && 0 == strncmp(mCodecContext->codec->name, "libdav1d", 8);
  if (m8BitOutput) {
    FFMPEG_LOG("Enable 8-bit output for dav1d");
    m8BitRecycleBin = MakeRefPtr<BufferRecycleBin>();
  }
  return InitPromise::CreateAndResolve(TrackInfo::kVideoTrack, __func__);
}

static gfx::ColorRange GetColorRange(enum AVColorRange& aColorRange) {
  return aColorRange == AVCOL_RANGE_JPEG ? gfx::ColorRange::FULL
                                         : gfx::ColorRange::LIMITED;
}

static bool IsYUVFormat(const AVPixelFormat& aFormat) {
  return aFormat != AV_PIX_FMT_GBRP && aFormat != AV_PIX_FMT_GBRP10LE;
}

static gfx::YUVColorSpace TransferAVColorSpaceToColorSpace(
    const AVColorSpace aSpace, const AVPixelFormat aFormat,
    const gfx::IntSize& aSize) {
  if (!IsYUVFormat(aFormat)) {
    return gfx::YUVColorSpace::Identity;
  }
  switch (aSpace) {
#if LIBAVCODEC_VERSION_MAJOR >= 55
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
      return gfx::YUVColorSpace::BT2020;
#endif
    case AVCOL_SPC_BT709:
      return gfx::YUVColorSpace::BT709;
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_BT470BG:
      return gfx::YUVColorSpace::BT601;
    default:
      return DefaultColorSpace(aSize);
  }
}

#ifdef CUSTOMIZED_BUFFER_ALLOCATION
static int GetVideoBufferWrapper(struct AVCodecContext* aCodecContext,
                                 AVFrame* aFrame, int aFlags) {
  auto* decoder =
      static_cast<FFmpegVideoDecoder<LIBAV_VER>*>(aCodecContext->opaque);
  int rv = decoder->GetVideoBuffer(aCodecContext, aFrame, aFlags);
  return rv < 0 ? decoder->GetVideoBufferDefault(aCodecContext, aFrame, aFlags)
                : rv;
}

static void ReleaseVideoBufferWrapper(void* opaque, uint8_t* data) {
  if (opaque) {
    FFMPEGV_LOG("ReleaseVideoBufferWrapper: PlanarYCbCrImage=%p", opaque);
    RefPtr<ImageBufferWrapper> image = static_cast<ImageBufferWrapper*>(opaque);
    image->ReleaseBuffer();
  }
}

static bool IsColorFormatSupportedForUsingCustomizedBuffer(
    const AVPixelFormat& aFormat) {
#  if XP_WIN
  // Currently the web render doesn't support uploading R16 surface, so we can't
  // use the shmem texture for 10 bit+ videos which would be uploaded by the
  // web render. See Bug 1751498.
  return aFormat == AV_PIX_FMT_YUV420P || aFormat == AV_PIX_FMT_YUVJ420P ||
         aFormat == AV_PIX_FMT_YUV444P || aFormat == AV_PIX_FMT_YUVJ444P;
#  else
  // For now, we only support for YUV420P, YUVJ420P, YUV444P and YUVJ444P which
  // are the only non-HW accelerated format supported by FFmpeg's H264 and VP9
  // decoder.
  return aFormat == AV_PIX_FMT_YUV420P || aFormat == AV_PIX_FMT_YUVJ420P ||
         aFormat == AV_PIX_FMT_YUV420P10LE ||
         aFormat == AV_PIX_FMT_YUV420P12LE || aFormat == AV_PIX_FMT_YUV444P ||
         aFormat == AV_PIX_FMT_YUVJ444P || aFormat == AV_PIX_FMT_YUV444P10LE ||
         aFormat == AV_PIX_FMT_YUV444P12LE;
#  endif
}

static bool IsYUV420Sampling(const AVPixelFormat& aFormat) {
  return aFormat == AV_PIX_FMT_YUV420P || aFormat == AV_PIX_FMT_YUVJ420P ||
         aFormat == AV_PIX_FMT_YUV420P10LE || aFormat == AV_PIX_FMT_YUV420P12LE;
}

#  if defined(MOZ_WIDGET_GTK)
bool FFmpegVideoDecoder<LIBAV_VER>::IsLinuxHDR() const {
  if (!mInfo.mColorPrimaries || !mInfo.mTransferFunction) {
    return false;
  }
  return mInfo.mColorPrimaries.value() == gfx::ColorSpace2::BT2020 &&
         (mInfo.mTransferFunction.value() == gfx::TransferFunction::PQ ||
          mInfo.mTransferFunction.value() == gfx::TransferFunction::HLG);
}
#  endif

layers::TextureClient*
FFmpegVideoDecoder<LIBAV_VER>::AllocateTextureClientForImage(
    struct AVCodecContext* aCodecContext, PlanarYCbCrImage* aImage) {
  MOZ_ASSERT(
      IsColorFormatSupportedForUsingCustomizedBuffer(aCodecContext->pix_fmt));

  // FFmpeg will store images with color depth > 8 bits in 16 bits with extra
  // padding.
  const int32_t bytesPerChannel =
      GetColorDepth(aCodecContext->pix_fmt) == gfx::ColorDepth::COLOR_8 ? 1 : 2;

  // If adjusted Ysize is larger than the actual image size (coded_width *
  // coded_height), that means ffmpeg decoder needs extra padding on both width
  // and height. If that happens, the planes will need to be cropped later in
  // order to avoid visible incorrect border on the right and bottom of the
  // actual image.
  //
  // Here are examples of various sizes video in YUV420P format, the width and
  // height would need to be adjusted in order to align padding.
  //
  // Eg1. video (1920*1080)
  // plane Y
  // width 1920 height 1080 -> adjusted-width 1920 adjusted-height 1088
  // plane Cb/Cr
  // width 960  height  540 -> adjusted-width 1024 adjusted-height 544
  //
  // Eg2. video (2560*1440)
  // plane Y
  // width 2560 height 1440 -> adjusted-width 2560 adjusted-height 1440
  // plane Cb/Cr
  // width 1280 height  720 -> adjusted-width 1280 adjusted-height 736
  layers::PlanarYCbCrData data;
  const auto yDims =
      gfx::IntSize{aCodecContext->coded_width, aCodecContext->coded_height};
  auto paddedYSize = yDims;
  mLib->avcodec_align_dimensions(aCodecContext, &paddedYSize.width,
                                 &paddedYSize.height);
  data.mYStride = paddedYSize.Width() * bytesPerChannel;

  MOZ_ASSERT(
      IsColorFormatSupportedForUsingCustomizedBuffer(aCodecContext->pix_fmt));
  auto uvDims = yDims;
  if (IsYUV420Sampling(aCodecContext->pix_fmt)) {
    uvDims.width = (uvDims.width + 1) / 2;
    uvDims.height = (uvDims.height + 1) / 2;
    data.mChromaSubsampling = gfx::ChromaSubsampling::HALF_WIDTH_AND_HEIGHT;
  }
  auto paddedCbCrSize = uvDims;
  mLib->avcodec_align_dimensions(aCodecContext, &paddedCbCrSize.width,
                                 &paddedCbCrSize.height);
  data.mCbCrStride = paddedCbCrSize.Width() * bytesPerChannel;

  // Setting other attributes
  data.mPictureRect = gfx::IntRect(
      mInfo.ScaledImageRect(aCodecContext->width, aCodecContext->height)
          .TopLeft(),
      gfx::IntSize(aCodecContext->width, aCodecContext->height));
  data.mStereoMode = mInfo.mStereoMode;
  if (aCodecContext->colorspace != AVCOL_SPC_UNSPECIFIED) {
    data.mYUVColorSpace = TransferAVColorSpaceToColorSpace(
        aCodecContext->colorspace, aCodecContext->pix_fmt,
        data.mPictureRect.Size());
  } else {
    data.mYUVColorSpace = mInfo.mColorSpace
                              ? *mInfo.mColorSpace
                              : DefaultColorSpace(data.mPictureRect.Size());
  }
  data.mColorDepth = GetColorDepth(aCodecContext->pix_fmt);
  data.mColorRange = GetColorRange(aCodecContext->color_range);

  FFMPEG_LOGV(
      "Created plane data, YSize=(%d, %d), CbCrSize=(%d, %d), "
      "CroppedYSize=(%d, %d), CroppedCbCrSize=(%d, %d), ColorDepth=%hhu",
      paddedYSize.Width(), paddedYSize.Height(), paddedCbCrSize.Width(),
      paddedCbCrSize.Height(), data.YPictureSize().Width(),
      data.YPictureSize().Height(), data.CbCrPictureSize().Width(),
      data.CbCrPictureSize().Height(), static_cast<uint8_t>(data.mColorDepth));

  // Allocate a shmem buffer for image.
  if (NS_FAILED(aImage->CreateEmptyBuffer(data, paddedYSize, paddedCbCrSize))) {
    return nullptr;
  }
  return aImage->GetTextureClient(mImageAllocator);
}

int FFmpegVideoDecoder<LIBAV_VER>::GetVideoBuffer(
    struct AVCodecContext* aCodecContext, AVFrame* aFrame, int aFlags) {
  FFMPEG_LOGV("GetVideoBuffer: aCodecContext=%p aFrame=%p", aCodecContext,
              aFrame);
  if (!StaticPrefs::media_ffmpeg_customized_buffer_allocation()) {
    return AVERROR(EINVAL);
  }

  if (mIsUsingShmemBufferForDecode && !*mIsUsingShmemBufferForDecode) {
    return AVERROR(EINVAL);
  }

  // Codec doesn't support custom allocator.
  if (!(aCodecContext->codec->capabilities & AV_CODEC_CAP_DR1)) {
    return AVERROR(EINVAL);
  }

  // Pre-allocation is only for sw decoding. During decoding, ffmpeg decoder
  // will need to reference decoded frames, if those frames are on shmem buffer,
  // then it would cause a need to read CPU data from GPU, which is slow.
  if (IsHardwareAccelerated()) {
    return AVERROR(EINVAL);
  }

#  if defined(MOZ_WIDGET_GTK) && defined(MOZ_USE_HWDECODE)
  if (mUploadSWDecodeToDMABuf) {
    FFMPEG_LOG("DMABuf upload doesn't use shm buffers");
    return AVERROR(EINVAL);
  }
#  endif

  if (!IsColorFormatSupportedForUsingCustomizedBuffer(aCodecContext->pix_fmt)) {
    FFMPEG_LOG("Not support color format %d", aCodecContext->pix_fmt);
    return AVERROR(EINVAL);
  }

  if (aCodecContext->lowres != 0) {
    FFMPEG_LOG("Not support low resolution decoding");
    return AVERROR(EINVAL);
  }

  const gfx::IntSize size(aCodecContext->width, aCodecContext->height);
  int rv = mLib->av_image_check_size(size.Width(), size.Height(), 0, nullptr);
  if (rv < 0) {
    FFMPEG_LOG("Invalid image size");
    return rv;
  }

  CheckedInt32 dataSize = mLib->av_image_get_buffer_size(
      aCodecContext->pix_fmt, aCodecContext->coded_width,
      aCodecContext->coded_height, 32);
  if (!dataSize.isValid()) {
    FFMPEG_LOG("Data size overflow!");
    return AVERROR(EINVAL);
  }

  if (!mImageContainer) {
    FFMPEG_LOG("No Image container!");
    return AVERROR(EINVAL);
  }

  RefPtr<PlanarYCbCrImage> image = mImageContainer->CreatePlanarYCbCrImage();
  if (!image) {
    FFMPEG_LOG("Failed to create YCbCr image");
    return AVERROR(EINVAL);
  }
  image->SetColorDepth(mInfo.mColorDepth);

  RefPtr<layers::TextureClient> texture =
      AllocateTextureClientForImage(aCodecContext, image);
  if (!texture) {
    FFMPEG_LOG("Failed to allocate a texture client");
    return AVERROR(EINVAL);
  }

  if (!texture->Lock(layers::OpenMode::OPEN_WRITE)) {
    FFMPEG_LOG("Failed to lock the texture");
    return AVERROR(EINVAL);
  }
  auto autoUnlock = MakeScopeExit([&] { texture->Unlock(); });

  layers::MappedYCbCrTextureData mapped;
  if (!texture->BorrowMappedYCbCrData(mapped)) {
    FFMPEG_LOG("Failed to borrow mapped data for the texture");
    return AVERROR(EINVAL);
  }

  aFrame->data[0] = mapped.y.data;
  aFrame->data[1] = mapped.cb.data;
  aFrame->data[2] = mapped.cr.data;

  aFrame->linesize[0] = mapped.y.stride;
  aFrame->linesize[1] = mapped.cb.stride;
  aFrame->linesize[2] = mapped.cr.stride;

  aFrame->width = aCodecContext->coded_width;
  aFrame->height = aCodecContext->coded_height;
  aFrame->format = aCodecContext->pix_fmt;
  aFrame->extended_data = aFrame->data;
#  if LIBAVCODEC_VERSION_MAJOR < 61
  aFrame->reordered_opaque = aCodecContext->reordered_opaque;
#  endif
  MOZ_ASSERT(aFrame->data[0] && aFrame->data[1] && aFrame->data[2]);

  // This will hold a reference to image, and the reference would be dropped
  // when ffmpeg tells us that the buffer is no longer needed.
  auto imageWrapper = MakeRefPtr<ImageBufferWrapper>(image.get(), this);
  aFrame->buf[0] =
      mLib->av_buffer_create(aFrame->data[0], dataSize.value(),
                             ReleaseVideoBufferWrapper, imageWrapper.get(), 0);
  if (!aFrame->buf[0]) {
    FFMPEG_LOG("Failed to allocate buffer");
    return AVERROR(EINVAL);
  }

  FFMPEG_LOG("Created av buffer, buf=%p, data=%p, image=%p, sz=%d",
             aFrame->buf[0], aFrame->data[0], imageWrapper.get(),
             dataSize.value());
  mAllocatedImages.Insert(imageWrapper.get());
  mIsUsingShmemBufferForDecode = Some(true);
  return 0;
}
#endif

void FFmpegVideoDecoder<LIBAV_VER>::InitCodecContext() {
  mCodecContext->width = mInfo.mImage.width;
  mCodecContext->height = mInfo.mImage.height;

  // We use the same logic as libvpx in determining the number of threads to use
  // so that we end up behaving in the same fashion when using ffmpeg as
  // we would otherwise cause various crashes (see bug 1236167)
  int decode_threads = 1;
  if (mInfo.mDisplay.width >= 2048) {
    decode_threads = 8;
  } else if (mInfo.mDisplay.width >= 1024) {
    decode_threads = 4;
  } else if (mInfo.mDisplay.width >= 320) {
    decode_threads = 2;
  }

  if (mLowLatency) {
    mCodecContext->flags |= AV_CODEC_FLAG_LOW_DELAY;
    // ffvp9 and ffvp8 at this stage do not support slice threading, but it may
    // help with the h264 decoder if there's ever one.
    mCodecContext->thread_type = FF_THREAD_SLICE;
  } else {
    decode_threads = std::min(decode_threads, PR_GetNumberOfProcessors() - 1);
    decode_threads = std::max(decode_threads, 1);
    mCodecContext->thread_count = decode_threads;
    if (decode_threads > 1) {
      mCodecContext->thread_type = FF_THREAD_SLICE | FF_THREAD_FRAME;
    }
  }

  // FFmpeg will call back to this to negotiate a video pixel format.
  mCodecContext->get_format = ChoosePixelFormat;
#ifdef CUSTOMIZED_BUFFER_ALLOCATION
  FFMPEG_LOG("Set get_buffer2 for customized buffer allocation");
  mCodecContext->get_buffer2 = GetVideoBufferWrapper;
  mCodecContext->opaque = this;
#  if FF_API_THREAD_SAFE_CALLBACKS
  mCodecContext->thread_safe_callbacks = 1;
#  endif
#endif
}

nsCString FFmpegVideoDecoder<LIBAV_VER>::GetCodecName() const {
#if LIBAVCODEC_VERSION_MAJOR > 53
  return nsCString(mLib->avcodec_descriptor_get(mCodecID)->name);
#else
  return nsLiteralCString("FFmpegAudioDecoder");
#endif
}

#ifdef MOZ_USE_HWDECODE
void FFmpegVideoDecoder<LIBAV_VER>::InitHWCodecContext(ContextType aType) {
  mCodecContext->width = mInfo.mImage.width;
  mCodecContext->height = mInfo.mImage.height;
  mCodecContext->thread_count = 1;

  if (aType == ContextType::V4L2) {
    mCodecContext->get_format = ChooseV4L2PixelFormat;
  } else if (aType == ContextType::VAAPI) {
    mCodecContext->get_format = ChooseVAAPIPixelFormat;
  } else {
    MOZ_DIAGNOSTIC_ASSERT(aType == ContextType::D3D11VA);
    mCodecContext->get_format = ChooseD3D11VAPixelFormat;
  }

  if (mCodecID == AV_CODEC_ID_H264) {
    mCodecContext->extra_hw_frames =
        H264::ComputeMaxRefFrames(mInfo.mExtraData);
  } else if (mCodecID == AV_CODEC_ID_HEVC) {
    mCodecContext->extra_hw_frames =
        H265::ComputeMaxRefFrames(mInfo.mExtraData);
  } else {
    mCodecContext->extra_hw_frames = EXTRA_HW_FRAMES;
  }
  if (mLowLatency) {
    mCodecContext->flags |= AV_CODEC_FLAG_LOW_DELAY;
  }
}
#endif

static int64_t GetFramePts(const AVFrame* aFrame) {
#if LIBAVCODEC_VERSION_MAJOR > 57
  return aFrame->pts;
#else
  return aFrame->pkt_pts;
#endif
}

#if LIBAVCODEC_VERSION_MAJOR >= 58
void FFmpegVideoDecoder<LIBAV_VER>::DecodeStats::DecodeStart() {
  mDecodeStart = TimeStamp::Now();
}

bool FFmpegVideoDecoder<LIBAV_VER>::DecodeStats::IsDecodingSlow() const {
  return mDecodedFramesLate > mMaxLateDecodedFrames;
}

void FFmpegVideoDecoder<LIBAV_VER>::DecodeStats::UpdateDecodeTimes(
    const AVFrame* aFrame) {
  TimeStamp now = TimeStamp::Now();
  float decodeTime = (now - mDecodeStart).ToMilliseconds();
  mDecodeStart = now;

  const float frameDuration = Duration(aFrame) / 1000.0f;
  if (frameDuration <= 0.0f) {
    FFMPEGV_LOG("Incorrect frame duration, skipping decode stats.");
    return;
  }

  mDecodedFrames++;
  mAverageFrameDuration =
      (mAverageFrameDuration * (mDecodedFrames - 1) + frameDuration) /
      mDecodedFrames;
  mAverageFrameDecodeTime =
      (mAverageFrameDecodeTime * (mDecodedFrames - 1) + decodeTime) /
      mDecodedFrames;

  FFMPEGV_LOG(
      "Frame decode takes %.2f ms average decode time %.2f ms frame duration "
      "%.2f average frame duration %.2f decoded %d frames\n",
      decodeTime, mAverageFrameDecodeTime, frameDuration, mAverageFrameDuration,
      mDecodedFrames);

  // Frame duration and frame decode times may vary and may not
  // neccessarily lead to video playback failure.
  //
  // Checks frame decode time and recent frame duration and also
  // frame decode time and average frame duration (video fps).
  //
  // Log a problem only if both indicators fails.
  if (decodeTime > frameDuration && decodeTime > mAverageFrameDuration) {
    PROFILER_MARKER_TEXT("FFmpegVideoDecoder::DoDecode", MEDIA_PLAYBACK, {},
                         "frame decode takes too long");
    mDecodedFramesLate++;
    mLastDelayedFrameNum = mDecodedFrames;
    FFMPEGV_LOG("  slow decode: failed to decode in time (decoded late %d)",
                mDecodedFramesLate);
  } else if (mLastDelayedFrameNum) {
    // Reset mDecodedFramesLate in case of correct decode during
    // mDelayedFrameReset period.
    float correctPlaybackTime =
        (mDecodedFrames - mLastDelayedFrameNum) * mAverageFrameDuration;
    if (correctPlaybackTime > mDelayedFrameReset) {
      FFMPEGV_LOG("  mLastFramePts reset due to seamless decode period");
      mDecodedFramesLate = 0;
      mLastDelayedFrameNum = 0;
    }
  }
}
#endif

MediaResult FFmpegVideoDecoder<LIBAV_VER>::DoDecode(
    MediaRawData* aSample, uint8_t* aData, int aSize, bool* aGotFrame,
    MediaDataDecoder::DecodedData& aResults) {
  MOZ_ASSERT(mTaskQueue->IsOnCurrentThread());
  AVPacket* packet;

#if LIBAVCODEC_VERSION_MAJOR >= 61
  packet = mLib->av_packet_alloc();
  auto raii = MakeScopeExit([&]() { mLib->av_packet_free(&packet); });
#else
  AVPacket packet_mem;
  packet = &packet_mem;
  mLib->av_init_packet(packet);
#endif

#if LIBAVCODEC_VERSION_MAJOR >= 58
  mDecodeStats.DecodeStart();
#endif

  packet->data = aData;
  packet->size = aSize;
  packet->dts = aSample->mTimecode.ToMicroseconds();
  packet->pts = aSample->mTime.ToMicroseconds();
  packet->flags = aSample->mKeyframe ? AV_PKT_FLAG_KEY : 0;
  packet->pos = aSample->mOffset;

  mTrackingId.apply([&](const auto& aId) {
    MediaInfoFlag flag = MediaInfoFlag::None;
    flag |= (aSample->mKeyframe ? MediaInfoFlag::KeyFrame
                                : MediaInfoFlag::NonKeyFrame);
    flag |= (IsHardwareAccelerated() ? MediaInfoFlag::HardwareDecoding
                                     : MediaInfoFlag::SoftwareDecoding);
    switch (mCodecID) {
      case AV_CODEC_ID_H264:
        flag |= MediaInfoFlag::VIDEO_H264;
        break;
#if LIBAVCODEC_VERSION_MAJOR >= 54
      case AV_CODEC_ID_VP8:
        flag |= MediaInfoFlag::VIDEO_VP8;
        break;
#endif
#if LIBAVCODEC_VERSION_MAJOR >= 55
      case AV_CODEC_ID_VP9:
        flag |= MediaInfoFlag::VIDEO_VP9;
        break;
      case AV_CODEC_ID_HEVC:
        flag |= MediaInfoFlag::VIDEO_HEVC;
        break;
#endif
#ifdef FFMPEG_AV1_DECODE
      case AV_CODEC_ID_AV1:
        flag |= MediaInfoFlag::VIDEO_AV1;
        break;
#endif
      default:
        break;
    }
    mPerformanceRecorder.Start(
        packet->dts,
        nsPrintfCString("FFmpegVideoDecoder(%d)", LIBAVCODEC_VERSION_MAJOR),
        aId, flag);
  });

#ifdef MOZ_FFMPEG_USE_INPUT_INFO_MAP
  InsertInputInfo(aSample);
#endif

#if LIBAVCODEC_VERSION_MAJOR >= 58
  if (aData || !mHasSentDrainPacket) {
    packet->duration = aSample->mDuration.ToMicroseconds();
    int res = mLib->avcodec_send_packet(mCodecContext, packet);
    if (res < 0) {
      // In theory, avcodec_send_packet could sent -EAGAIN should its internal
      // buffers be full. In practice this can't happen as we only feed one
      // frame at a time, and we immediately call avcodec_receive_frame right
      // after.
      char errStr[AV_ERROR_MAX_STRING_SIZE];
      mLib->av_strerror(res, errStr, AV_ERROR_MAX_STRING_SIZE);
      FFMPEG_LOG("avcodec_send_packet error: %s", errStr);
      return MediaResult(
          res == int(AVERROR_EOF) ? NS_ERROR_DOM_MEDIA_END_OF_STREAM
                                  : NS_ERROR_DOM_MEDIA_DECODE_ERR,
          RESULT_DETAIL("avcodec_send_packet error: %s", errStr));
    }
  }
  if (!aData) {
    // On some platforms (e.g. Android), there are a limited number of output
    // buffers available. When draining, we may reach this limit, so we must
    // return what we have, and allow the caller to try again. We don't need to
    // resend the null packet in that case since the codec is still in the
    // draining state.
    mHasSentDrainPacket = true;
  }
  if (aGotFrame) {
    *aGotFrame = false;
  }
  do {
    if (!PrepareFrame()) {
      NS_WARNING("FFmpeg decoder failed to allocate frame.");
      return MediaResult(NS_ERROR_OUT_OF_MEMORY, __func__);
    }

#  if defined(MOZ_USE_HWDECODE) && defined(MOZ_WIDGET_GTK)
    // Release unused VA-API surfaces before avcodec_receive_frame() as
    // ffmpeg recycles VASurface for HW decoding.
    if (mVideoFramePool) {
      mVideoFramePool->ReleaseUnusedVAAPIFrames();
    }
#  endif

    int res = mLib->avcodec_receive_frame(mCodecContext, mFrame);
    if (res == int(AVERROR_EOF)) {
      FFMPEG_LOG("  End of stream or output buffer shortage.");
      return NS_ERROR_DOM_MEDIA_END_OF_STREAM;
    }
    if (res == AVERROR(EAGAIN)) {
      return NS_OK;
    }
    if (res < 0) {
      char errStr[AV_ERROR_MAX_STRING_SIZE];
      mLib->av_strerror(res, errStr, AV_ERROR_MAX_STRING_SIZE);
      FFMPEG_LOG("  avcodec_receive_frame error: %s", errStr);
      return MediaResult(
          NS_ERROR_DOM_MEDIA_DECODE_ERR,
          RESULT_DETAIL("avcodec_receive_frame error: %s", errStr));
    }

    mDecodeStats.UpdateDecodeTimes(mFrame);

    MediaResult rv;
#  ifdef MOZ_USE_HWDECODE
    if (IsHardwareAccelerated()) {
#    ifdef MOZ_WIDGET_GTK
      if (mDecodeStats.IsDecodingSlow() &&
          !StaticPrefs::media_ffmpeg_disable_software_fallback()) {
        PROFILER_MARKER_TEXT("FFmpegVideoDecoder::DoDecode", MEDIA_PLAYBACK, {},
                             "Fallback to SW decode");
        FFMPEG_LOG("  HW decoding is slow, switching back to SW decode");
        return MediaResult(
            NS_ERROR_DOM_MEDIA_DECODE_ERR,
            RESULT_DETAIL("HW decoding is slow, switching back to SW decode"));
      }
      if (mUsingV4L2) {
        rv = CreateImageV4L2(mFrame->pkt_pos, GetFramePts(mFrame),
                             Duration(mFrame), aResults);
      } else {
        rv = CreateImageVAAPI(mFrame->pkt_pos, GetFramePts(mFrame),
                              Duration(mFrame), aResults);
      }

      // If VA-API/V4L2 playback failed, just quit. Decoder is going to be
      // restarted without hardware acceleration
      if (NS_FAILED(rv)) {
        // Explicitly remove dmabuf surface pool as it's configured
        // for VA-API/V4L2 support.
        mVideoFramePool = nullptr;
        return rv;
      }
#    elif defined(MOZ_ENABLE_D3D11VA)
      rv = CreateImageD3D11(mFrame->pkt_pos, GetFramePts(mFrame),
                            Duration(mFrame), aResults);
#    else
      return MediaResult(NS_ERROR_DOM_MEDIA_DECODE_ERR,
                         RESULT_DETAIL("No HW decoding implementation!"));
#    endif
    } else
#  endif
    {
      rv = CreateImage(mFrame->pkt_pos, GetFramePts(mFrame), Duration(mFrame),
                       aResults);
    }
    if (NS_FAILED(rv)) {
      return rv;
    }

    RecordFrame(aSample, aResults.LastElement());
    if (aGotFrame) {
      *aGotFrame = true;
    }
  } while (true);
#else
  if (!PrepareFrame()) {
    NS_WARNING("FFmpeg decoder failed to allocate frame.");
    return MediaResult(NS_ERROR_OUT_OF_MEMORY, __func__);
  }

  // Required with old version of FFmpeg/LibAV
  mFrame->reordered_opaque = AV_NOPTS_VALUE;

  int decoded;
  int bytesConsumed =
      mLib->avcodec_decode_video2(mCodecContext, mFrame, &decoded, packet);

  FFMPEG_LOG(
      "DoDecodeFrame:decode_video: rv=%d decoded=%d "
      "(Input: pts(%" PRId64 ") dts(%" PRId64 ") Output: pts(%" PRId64
      ") "
      "opaque(%" PRId64 ") pts(%" PRId64 ") pkt_dts(%" PRId64 "))",
      bytesConsumed, decoded, packet->pts, packet->dts, mFrame->pts,
      mFrame->reordered_opaque, mFrame->pts, mFrame->pkt_dts);

  if (bytesConsumed < 0) {
    return MediaResult(NS_ERROR_DOM_MEDIA_DECODE_ERR,
                       RESULT_DETAIL("FFmpeg video error: %d", bytesConsumed));
  }

  if (!decoded) {
    if (aGotFrame) {
      *aGotFrame = false;
    }
    return NS_OK;
  }

  // If we've decoded a frame then we need to output it
  int64_t pts =
      mPtsContext.GuessCorrectPts(GetFramePts(mFrame), mFrame->pkt_dts);

  InputInfo info(aSample);
  TakeInputInfo(mFrame, info);

  MediaResult rv = CreateImage(aSample->mOffset, pts, info.mDuration, aResults);
  if (NS_FAILED(rv)) {
    return rv;
  }

  mTrackingId.apply(
      [&](const auto&) { RecordFrame(aSample, aResults.LastElement()); });

  if (aGotFrame) {
    *aGotFrame = true;
  }
  return rv;
#endif
}

void FFmpegVideoDecoder<LIBAV_VER>::RecordFrame(const MediaRawData* aSample,
                                                const MediaData* aData) {
  mPerformanceRecorder.Record(
      aData->mTimecode.ToMicroseconds(), [&](auto& aStage) {
        aStage.SetResolution(mFrame->width, mFrame->height);
        auto format = [&]() -> Maybe<DecodeStage::ImageFormat> {
          switch (mCodecContext->pix_fmt) {
            case AV_PIX_FMT_YUV420P:
            case AV_PIX_FMT_YUVJ420P:
            case AV_PIX_FMT_YUV420P10LE:
#if LIBAVCODEC_VERSION_MAJOR >= 57
            case AV_PIX_FMT_YUV420P12LE:
#endif
              return Some(DecodeStage::YUV420P);
            case AV_PIX_FMT_YUV422P:
            case AV_PIX_FMT_YUV422P10LE:
#if LIBAVCODEC_VERSION_MAJOR >= 57
            case AV_PIX_FMT_YUV422P12LE:
#endif
              return Some(DecodeStage::YUV422P);
            case AV_PIX_FMT_YUV444P:
            case AV_PIX_FMT_YUVJ444P:
            case AV_PIX_FMT_YUV444P10LE:
#if LIBAVCODEC_VERSION_MAJOR >= 57
            case AV_PIX_FMT_YUV444P12LE:
#endif
              return Some(DecodeStage::YUV444P);
            case AV_PIX_FMT_GBRP:
            case AV_PIX_FMT_GBRP10LE:
              return Some(DecodeStage::GBRP);
            case AV_PIX_FMT_VAAPI_VLD:
              return Some(DecodeStage::VAAPI_SURFACE);
#ifdef MOZ_ENABLE_D3D11VA
            case AV_PIX_FMT_D3D11:
              return Some(DecodeStage::D3D11_SURFACE);
#endif
            default:
              return Nothing();
          }
        }();
        format.apply([&](auto& aFmt) { aStage.SetImageFormat(aFmt); });
        aStage.SetColorDepth(GetColorDepth(mCodecContext->pix_fmt));
        aStage.SetYUVColorSpace(GetFrameColorSpace());
        aStage.SetColorRange(GetFrameColorRange());
        aStage.SetStartTimeAndEndTime(aSample->mTime.ToMicroseconds(),
                                      aSample->GetEndTime().ToMicroseconds());
      });
}

gfx::ColorDepth FFmpegVideoDecoder<LIBAV_VER>::GetColorDepth(
    const AVPixelFormat& aFormat) const {
  switch (aFormat) {
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUVJ444P:
      return gfx::ColorDepth::COLOR_8;
    case AV_PIX_FMT_YUV420P10LE:
    case AV_PIX_FMT_YUV422P10LE:
    case AV_PIX_FMT_YUV444P10LE:
    case AV_PIX_FMT_GBRP10LE:
      return gfx::ColorDepth::COLOR_10;
#if LIBAVCODEC_VERSION_MAJOR >= 57
    case AV_PIX_FMT_YUV420P12LE:
    case AV_PIX_FMT_YUV422P12LE:
    case AV_PIX_FMT_YUV444P12LE:
      return gfx::ColorDepth::COLOR_12;
#endif
#ifdef MOZ_ENABLE_D3D11VA
    case AV_PIX_FMT_D3D11:
#endif
    case AV_PIX_FMT_VAAPI_VLD:
      return mInfo.mColorDepth;
    default:
      MOZ_ASSERT_UNREACHABLE("Not supported format?");
      return gfx::ColorDepth::COLOR_8;
  }
}

gfx::YUVColorSpace FFmpegVideoDecoder<LIBAV_VER>::GetFrameColorSpace() const {
  AVColorSpace colorSpace = AVCOL_SPC_UNSPECIFIED;
#if LIBAVCODEC_VERSION_MAJOR > 58
  colorSpace = mFrame->colorspace;
#else
  if (mLib->av_frame_get_colorspace) {
    colorSpace = (AVColorSpace)mLib->av_frame_get_colorspace(mFrame);
  }
#endif
  return TransferAVColorSpaceToColorSpace(
      colorSpace, (AVPixelFormat)mFrame->format,
      gfx::IntSize{mFrame->width, mFrame->height});
}

gfx::ColorSpace2 FFmpegVideoDecoder<LIBAV_VER>::GetFrameColorPrimaries() const {
  AVColorPrimaries colorPrimaries = AVCOL_PRI_UNSPECIFIED;
#if LIBAVCODEC_VERSION_MAJOR > 57
  colorPrimaries = mFrame->color_primaries;
#endif
  switch (colorPrimaries) {
#if LIBAVCODEC_VERSION_MAJOR >= 55
    case AVCOL_PRI_BT2020:
      return gfx::ColorSpace2::BT2020;
#endif
    case AVCOL_PRI_BT709:
      return gfx::ColorSpace2::BT709;
    default:
      return gfx::ColorSpace2::BT709;
  }
}

gfx::ColorRange FFmpegVideoDecoder<LIBAV_VER>::GetFrameColorRange() const {
  AVColorRange range = AVCOL_RANGE_UNSPECIFIED;
#if LIBAVCODEC_VERSION_MAJOR > 58
  range = mFrame->color_range;
#else
  if (mLib->av_frame_get_color_range) {
    range = (AVColorRange)mLib->av_frame_get_color_range(mFrame);
  }
#endif
  return GetColorRange(range);
}

gfx::SurfaceFormat FFmpegVideoDecoder<LIBAV_VER>::GetSurfaceFormat() const {
  switch (mInfo.mColorDepth) {
    case gfx::ColorDepth::COLOR_8:
      return gfx::SurfaceFormat::NV12;
    case gfx::ColorDepth::COLOR_10:
      return gfx::SurfaceFormat::P010;
    default:
      MOZ_ASSERT_UNREACHABLE("Unexpected surface type");
      return gfx::SurfaceFormat::NV12;
  }
}

MediaResult FFmpegVideoDecoder<LIBAV_VER>::CreateImage(
    int64_t aOffset, int64_t aPts, int64_t aDuration,
    MediaDataDecoder::DecodedData& aResults) {
  FFMPEG_LOG("Got one frame output with pts=%" PRId64 " dts=%" PRId64
             " duration=%" PRId64,
             aPts, mFrame->pkt_dts, aDuration);

  VideoData::QuantizableBuffer b;
  b.mPlanes[0].mData = mFrame->data[0];
  b.mPlanes[1].mData = mFrame->data[1];
  b.mPlanes[2].mData = mFrame->data[2];

  b.mPlanes[0].mStride = mFrame->linesize[0];
  b.mPlanes[1].mStride = mFrame->linesize[1];
  b.mPlanes[2].mStride = mFrame->linesize[2];

  b.mPlanes[0].mSkip = 0;
  b.mPlanes[1].mSkip = 0;
  b.mPlanes[2].mSkip = 0;

  b.mPlanes[0].mWidth = mFrame->width;
  b.mPlanes[0].mHeight = mFrame->height;
  if (mCodecContext->pix_fmt == AV_PIX_FMT_YUV444P ||
      mCodecContext->pix_fmt == AV_PIX_FMT_YUV444P10LE ||
      mCodecContext->pix_fmt == AV_PIX_FMT_GBRP ||
      mCodecContext->pix_fmt == AV_PIX_FMT_GBRP10LE
#if LIBAVCODEC_VERSION_MAJOR >= 57
      || mCodecContext->pix_fmt == AV_PIX_FMT_YUV444P12LE
#endif
  ) {
    b.mPlanes[1].mWidth = b.mPlanes[2].mWidth = mFrame->width;
    b.mPlanes[1].mHeight = b.mPlanes[2].mHeight = mFrame->height;
    if (mCodecContext->pix_fmt == AV_PIX_FMT_YUV444P10LE ||
        mCodecContext->pix_fmt == AV_PIX_FMT_GBRP10LE) {
      b.mColorDepth = gfx::ColorDepth::COLOR_10;
    }
#if LIBAVCODEC_VERSION_MAJOR >= 57
    else if (mCodecContext->pix_fmt == AV_PIX_FMT_YUV444P12LE) {
      b.mColorDepth = gfx::ColorDepth::COLOR_12;
    }
#endif
  } else if (mCodecContext->pix_fmt == AV_PIX_FMT_YUV422P ||
             mCodecContext->pix_fmt == AV_PIX_FMT_YUV422P10LE
#if LIBAVCODEC_VERSION_MAJOR >= 57
             || mCodecContext->pix_fmt == AV_PIX_FMT_YUV422P12LE
#endif
  ) {
    b.mChromaSubsampling = gfx::ChromaSubsampling::HALF_WIDTH;
    b.mPlanes[1].mWidth = b.mPlanes[2].mWidth = (mFrame->width + 1) >> 1;
    b.mPlanes[1].mHeight = b.mPlanes[2].mHeight = mFrame->height;
    if (mCodecContext->pix_fmt == AV_PIX_FMT_YUV422P10LE) {
      b.mColorDepth = gfx::ColorDepth::COLOR_10;
    }
#if LIBAVCODEC_VERSION_MAJOR >= 57
    else if (mCodecContext->pix_fmt == AV_PIX_FMT_YUV422P12LE) {
      b.mColorDepth = gfx::ColorDepth::COLOR_12;
    }
#endif
  } else {
    b.mChromaSubsampling = gfx::ChromaSubsampling::HALF_WIDTH_AND_HEIGHT;
    b.mPlanes[1].mWidth = b.mPlanes[2].mWidth = (mFrame->width + 1) >> 1;
    b.mPlanes[1].mHeight = b.mPlanes[2].mHeight = (mFrame->height + 1) >> 1;
    if (mCodecContext->pix_fmt == AV_PIX_FMT_YUV420P10LE) {
      b.mColorDepth = gfx::ColorDepth::COLOR_10;
    }
#if LIBAVCODEC_VERSION_MAJOR >= 57
    else if (mCodecContext->pix_fmt == AV_PIX_FMT_YUV420P12LE) {
      b.mColorDepth = gfx::ColorDepth::COLOR_12;
    }
#endif
  }
  b.mYUVColorSpace = GetFrameColorSpace();
  b.mColorRange = GetFrameColorRange();

  RefPtr<VideoData> v;
#ifdef CUSTOMIZED_BUFFER_ALLOCATION
  bool requiresCopy = false;
#  ifdef XP_MACOSX
  // Bug 1765388: macOS needs to generate a MacIOSurfaceImage in order to
  // properly display HDR video. The later call to ::CreateAndCopyData does
  // that. If this shared memory buffer path also generated a
  // MacIOSurfaceImage, then we could use it for HDR.
  requiresCopy = (b.mColorDepth != gfx::ColorDepth::COLOR_8);
#  endif
  if (mIsUsingShmemBufferForDecode && *mIsUsingShmemBufferForDecode &&
      !requiresCopy) {
    RefPtr<ImageBufferWrapper> wrapper = static_cast<ImageBufferWrapper*>(
        mLib->av_buffer_get_opaque(mFrame->buf[0]));
    MOZ_ASSERT(wrapper);
    FFMPEG_LOGV("Create a video data from a shmem image=%p", wrapper.get());
    v = VideoData::CreateFromImage(
        mInfo.mDisplay, aOffset, TimeUnit::FromMicroseconds(aPts),
        TimeUnit::FromMicroseconds(aDuration), wrapper->AsImage(),
        !!mFrame->key_frame, TimeUnit::FromMicroseconds(-1));
  }
#endif
#if defined(MOZ_WIDGET_GTK) && defined(MOZ_USE_HWDECODE)
  if (mUploadSWDecodeToDMABuf) {
    MOZ_DIAGNOSTIC_ASSERT(!v);
    if (!mVideoFramePool) {
      mVideoFramePool = MakeUnique<VideoFramePool<LIBAV_VER>>(10);
    }
    const auto yuvData = layers::PlanarYCbCrData::From(b);
    if (yuvData) {
      auto surface =
          mVideoFramePool->GetVideoFrameSurface(*yuvData, mCodecContext);
      if (surface) {
        surface->SetYUVColorSpace(GetFrameColorSpace());
        surface->SetColorRange(GetFrameColorRange());
        if (mInfo.mColorPrimaries) {
          surface->SetColorPrimaries(mInfo.mColorPrimaries.value());
        }
        if (mInfo.mTransferFunction) {
          surface->SetTransferFunction(mInfo.mTransferFunction.value());
        }
        FFMPEG_LOGV(
            "Uploaded frame DMABuf surface UID %d HDR %d color space %s/%s "
            "transfer %s",
            surface->GetDMABufSurface()->GetUID(), IsLinuxHDR(),
            YUVColorSpaceToString(GetFrameColorSpace()),
            mInfo.mColorPrimaries
                ? ColorSpace2ToString(mInfo.mColorPrimaries.value())
                : "unknown",
            mInfo.mTransferFunction
                ? TransferFunctionToString(mInfo.mTransferFunction.value())
                : "unknown");
        v = VideoData::CreateFromImage(
            mInfo.mDisplay, aOffset, TimeUnit::FromMicroseconds(aPts),
            TimeUnit::FromMicroseconds(aDuration), surface->GetAsImage(),
            !!mFrame->key_frame, TimeUnit::FromMicroseconds(-1));
      } else {
        FFMPEG_LOG("Failed to uploaded video data to DMABuf");
      }
    } else {
      FFMPEG_LOG("Failed to convert PlanarYCbCrData");
    }
  }
#endif
  if (!v) {
    if (m8BitOutput && b.mColorDepth != gfx::ColorDepth::COLOR_8) {
      MediaResult ret = b.To8BitPerChannel(m8BitRecycleBin);
      if (NS_FAILED(ret.Code())) {
        FFMPEG_LOG("%s: %s", __func__, ret.Message().get());
        return ret;
      }
    }
    Result<already_AddRefed<VideoData>, MediaResult> r =
        VideoData::CreateAndCopyData(
            mInfo, mImageContainer, aOffset, TimeUnit::FromMicroseconds(aPts),
            TimeUnit::FromMicroseconds(aDuration), b, !!mFrame->key_frame,
            TimeUnit::FromMicroseconds(mFrame->pkt_dts),
            mInfo.ScaledImageRect(mFrame->width, mFrame->height),
            mImageAllocator);
    if (r.isErr()) {
      return r.unwrapErr();
    }
    v = r.unwrap();
  }
  MOZ_ASSERT(v);
  aResults.AppendElement(std::move(v));
  return NS_OK;
}

#if defined(MOZ_USE_HWDECODE) && defined(MOZ_WIDGET_GTK)
bool FFmpegVideoDecoder<LIBAV_VER>::GetVAAPISurfaceDescriptor(
    VADRMPRIMESurfaceDescriptor* aVaDesc) {
  VASurfaceID surface_id = (VASurfaceID)(uintptr_t)mFrame->data[3];
  VAStatus vas = VALibWrapper::sFuncs.vaExportSurfaceHandle(
      mDisplay, surface_id, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
      VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, aVaDesc);
  if (vas != VA_STATUS_SUCCESS) {
    FFMPEG_LOG("GetVAAPISurfaceDescriptor(): vaExportSurfaceHandle failed");
    return false;
  }
  vas = VALibWrapper::sFuncs.vaSyncSurface(mDisplay, surface_id);
  if (vas != VA_STATUS_SUCCESS) {
    FFMPEG_LOG("GetVAAPISurfaceDescriptor(): vaSyncSurface failed");
  }
  return true;
}

MediaResult FFmpegVideoDecoder<LIBAV_VER>::CreateImageVAAPI(
    int64_t aOffset, int64_t aPts, int64_t aDuration,
    MediaDataDecoder::DecodedData& aResults) {
  VADRMPRIMESurfaceDescriptor vaDesc;
  if (!GetVAAPISurfaceDescriptor(&vaDesc)) {
    return MediaResult(
        NS_ERROR_DOM_MEDIA_DECODE_ERR,
        RESULT_DETAIL("Unable to get frame by vaExportSurfaceHandle()"));
  }
  auto releaseSurfaceDescriptor = MakeScopeExit(
      [&] { DMABufSurfaceYUV::ReleaseVADRMPRIMESurfaceDescriptor(vaDesc); });

  MOZ_ASSERT(mTaskQueue->IsOnCurrentThread());
  if (!mVideoFramePool) {
    AVHWFramesContext* context =
        (AVHWFramesContext*)mCodecContext->hw_frames_ctx->data;
    mVideoFramePool =
        MakeUnique<VideoFramePool<LIBAV_VER>>(context->initial_pool_size);
  }
  auto surface = mVideoFramePool->GetVideoFrameSurface(
      vaDesc, mFrame->width, mFrame->height, mCodecContext, mFrame, mLib);
  if (!surface) {
    FFMPEG_LOG("CreateImageVAAPI(): failed to get VideoFrameSurface");
    return MediaResult(NS_ERROR_DOM_MEDIA_DECODE_ERR,
                       RESULT_DETAIL("VAAPI dmabuf allocation error"));
  }

  surface->SetYUVColorSpace(GetFrameColorSpace());
  surface->SetColorRange(GetFrameColorRange());
  if (mInfo.mColorPrimaries) {
    surface->SetColorPrimaries(mInfo.mColorPrimaries.value());
  }
  if (mInfo.mTransferFunction) {
    surface->SetTransferFunction(mInfo.mTransferFunction.value());
  }

  FFMPEG_LOG("VA-API frame pts=%" PRId64 " dts=%" PRId64 " duration=%" PRId64
             " color space %s/%s transfer %s",
             aPts, mFrame->pkt_dts, aDuration,
             YUVColorSpaceToString(GetFrameColorSpace()),
             mInfo.mColorPrimaries
                 ? ColorSpace2ToString(mInfo.mColorPrimaries.value())
                 : "unknown",
             mInfo.mTransferFunction
                 ? TransferFunctionToString(mInfo.mTransferFunction.value())
                 : "unknown");

  RefPtr<VideoData> vp = VideoData::CreateFromImage(
      mInfo.mDisplay, aOffset, TimeUnit::FromMicroseconds(aPts),
      TimeUnit::FromMicroseconds(aDuration), surface->GetAsImage(),
      !!mFrame->key_frame, TimeUnit::FromMicroseconds(mFrame->pkt_dts));

  if (!vp) {
    return MediaResult(NS_ERROR_DOM_MEDIA_DECODE_ERR,
                       RESULT_DETAIL("VAAPI image allocation error"));
  }

  aResults.AppendElement(std::move(vp));
  return NS_OK;
}

MediaResult FFmpegVideoDecoder<LIBAV_VER>::CreateImageV4L2(
    int64_t aOffset, int64_t aPts, int64_t aDuration,
    MediaDataDecoder::DecodedData& aResults) {
  FFMPEG_LOG("V4L2 Got one frame output with pts=%" PRId64 " dts=%" PRId64
             " duration=%" PRId64,
             aPts, mFrame->pkt_dts, aDuration);

  AVDRMFrameDescriptor* desc = (AVDRMFrameDescriptor*)mFrame->data[0];
  if (!desc) {
    return MediaResult(NS_ERROR_DOM_MEDIA_DECODE_ERR,
                       RESULT_DETAIL("Missing DRM PRIME descriptor in frame"));
  }

  // Note that the FDs in desc are owned by FFmpeg and it will reuse them
  // each time the same buffer is dequeued in future.  So we shouldn't close
  // them and so don't setup a clean-up handler for desc.

  MOZ_ASSERT(mTaskQueue->IsOnCurrentThread());
  if (!mVideoFramePool) {
    // With the V4L2 wrapper codec we can't see the capture buffer pool size.
    // But, this value is only used for deciding when we are running out of
    // free buffers and so should start copying them.  So a rough estimate
    // is sufficient, and the codec defaults to 20 capture buffers.
    mVideoFramePool = MakeUnique<VideoFramePool<LIBAV_VER>>(20);
  }

  auto surface = mVideoFramePool->GetVideoFrameSurface(
      *desc, mFrame->width, mFrame->height, mCodecContext, mFrame, mLib);
  if (!surface) {
    return MediaResult(NS_ERROR_DOM_MEDIA_DECODE_ERR,
                       RESULT_DETAIL("V4L2 dmabuf allocation error"));
  }
  surface->SetYUVColorSpace(GetFrameColorSpace());
  surface->SetColorRange(GetFrameColorRange());

  RefPtr<VideoData> vp = VideoData::CreateFromImage(
      mInfo.mDisplay, aOffset, TimeUnit::FromMicroseconds(aPts),
      TimeUnit::FromMicroseconds(aDuration), surface->GetAsImage(),
      !!mFrame->key_frame, TimeUnit::FromMicroseconds(mFrame->pkt_dts));

  if (!vp) {
    return MediaResult(NS_ERROR_DOM_MEDIA_DECODE_ERR,
                       RESULT_DETAIL("V4L2 image creation error"));
  }

  aResults.AppendElement(std::move(vp));
  return NS_OK;
}
#endif

RefPtr<MediaDataDecoder::FlushPromise>
FFmpegVideoDecoder<LIBAV_VER>::ProcessFlush() {
  FFMPEG_LOG("ProcessFlush()");
  MOZ_ASSERT(mTaskQueue->IsOnCurrentThread());
#if LIBAVCODEC_VERSION_MAJOR >= 58
  mHasSentDrainPacket = false;
#endif
#if LIBAVCODEC_VERSION_MAJOR < 58
  mPtsContext.Reset();
#endif
#ifdef MOZ_FFMPEG_USE_DURATION_MAP
  mDurationMap.Clear();
#endif
#if defined(MOZ_USE_HWDECODE) && defined(MOZ_WIDGET_GTK)
  if (mVideoFramePool) {
    mVideoFramePool->FlushFFmpegFrames();
  }
#endif
  mPerformanceRecorder.Record(std::numeric_limits<int64_t>::max());
  return FFmpegDataDecoder::ProcessFlush();
}

AVCodecID FFmpegVideoDecoder<LIBAV_VER>::GetCodecId(
    const nsACString& aMimeType) {
  if (MP4Decoder::IsH264(aMimeType)) {
    return AV_CODEC_ID_H264;
  }

#if LIBAVCODEC_VERSION_MAJOR >= 55
  if (MP4Decoder::IsHEVC(aMimeType)) {
    return AV_CODEC_ID_HEVC;
  }
#endif

  if (aMimeType.EqualsLiteral("video/x-vnd.on2.vp6")) {
    return AV_CODEC_ID_VP6F;
  }

#if LIBAVCODEC_VERSION_MAJOR >= 54
  if (VPXDecoder::IsVP8(aMimeType)) {
    return AV_CODEC_ID_VP8;
  }
#endif

#if LIBAVCODEC_VERSION_MAJOR >= 55
  if (VPXDecoder::IsVP9(aMimeType)) {
    return AV_CODEC_ID_VP9;
  }
#endif

#if defined(FFMPEG_AV1_DECODE)
  if (AOMDecoder::IsAV1(aMimeType)) {
    return AV_CODEC_ID_AV1;
  }
#endif

  return AV_CODEC_ID_NONE;
}

void FFmpegVideoDecoder<LIBAV_VER>::ProcessShutdown() {
  MOZ_ASSERT(mTaskQueue->IsOnCurrentThread());
#if defined(MOZ_USE_HWDECODE) && defined(MOZ_WIDGET_GTK)
  mVideoFramePool = nullptr;
  if (IsHardwareAccelerated()) {
    mLib->av_buffer_unref(&mVAAPIDeviceContext);
  }
#endif
#ifdef MOZ_ENABLE_D3D11VA
  if (IsHardwareAccelerated()) {
    AVHWDeviceContext* hwctx =
        reinterpret_cast<AVHWDeviceContext*>(mD3D11VADeviceContext->data);
    AVD3D11VADeviceContext* d3d11vactx =
        reinterpret_cast<AVD3D11VADeviceContext*>(hwctx->hwctx);
    d3d11vactx->device = nullptr;
    mLib->av_buffer_unref(&mD3D11VADeviceContext);
    mD3D11VADeviceContext = nullptr;
  }
#endif
  FFmpegDataDecoder<LIBAV_VER>::ProcessShutdown();
}

bool FFmpegVideoDecoder<LIBAV_VER>::IsHardwareAccelerated(
    nsACString& aFailureReason) const {
#if defined(MOZ_USE_HWDECODE) && defined(MOZ_WIDGET_GTK)
  return mUsingV4L2 || !!mVAAPIDeviceContext;
#elif defined(MOZ_ENABLE_D3D11VA)
  return !!mD3D11VADeviceContext;
#else
  return false;
#endif
}

#if defined(MOZ_USE_HWDECODE) && defined(MOZ_WIDGET_GTK)
bool FFmpegVideoDecoder<LIBAV_VER>::IsFormatAccelerated(
    AVCodecID aCodecID) const {
  for (const auto& format : mAcceleratedFormats) {
    if (format == aCodecID) {
      return true;
    }
  }
  return false;
}

// See ffmpeg / vaapi_decode.c how CodecID is mapped to VAProfile.
static const struct {
  enum AVCodecID codec_id;
  VAProfile va_profile;
  char name[100];
} vaapi_profile_map[] = {
#  define MAP(c, v, n) {AV_CODEC_ID_##c, VAProfile##v, n}
    MAP(H264, H264ConstrainedBaseline, "H264ConstrainedBaseline"),
    MAP(H264, H264Main, "H264Main"),
    MAP(H264, H264High, "H264High"),
    MAP(VP8, VP8Version0_3, "VP8Version0_3"),
    MAP(VP9, VP9Profile0, "VP9Profile0"),
    MAP(VP9, VP9Profile2, "VP9Profile2"),
    MAP(AV1, AV1Profile0, "AV1Profile0"),
    MAP(AV1, AV1Profile1, "AV1Profile1"),
    MAP(HEVC, HEVCMain, "HEVCMain"),
    MAP(HEVC, HEVCMain10, "HEVCMain10"),
    MAP(HEVC, HEVCMain10, "HEVCMain12"),
#  undef MAP
};

static AVCodecID VAProfileToCodecID(VAProfile aVAProfile) {
  for (const auto& profile : vaapi_profile_map) {
    if (profile.va_profile == aVAProfile) {
      return profile.codec_id;
    }
  }
  return AV_CODEC_ID_NONE;
}

static const char* VAProfileName(VAProfile aVAProfile) {
  for (const auto& profile : vaapi_profile_map) {
    if (profile.va_profile == aVAProfile) {
      return profile.name;
    }
  }
  return nullptr;
}

// This code is adopted from mpv project va-api routine
// determine_working_formats()
void FFmpegVideoDecoder<LIBAV_VER>::AddAcceleratedFormats(
    nsTArray<AVCodecID>& aCodecList, AVCodecID aCodecID,
    AVVAAPIHWConfig* hwconfig) {
  AVHWFramesConstraints* fc =
      mLib->av_hwdevice_get_hwframe_constraints(mVAAPIDeviceContext, hwconfig);
  if (!fc) {
    FFMPEG_LOG("    failed to retrieve libavutil frame constraints");
    return;
  }
  auto autoRelease =
      MakeScopeExit([&] { mLib->av_hwframe_constraints_free(&fc); });

  bool foundSupportedFormat = false;
  for (int n = 0;
       fc->valid_sw_formats && fc->valid_sw_formats[n] != AV_PIX_FMT_NONE;
       n++) {
#  ifdef MOZ_LOGGING
    char formatDesc[1000];
    FFMPEG_LOG("    codec %s format %s", mLib->avcodec_get_name(aCodecID),
               mLib->av_get_pix_fmt_string(formatDesc, sizeof(formatDesc),
                                           fc->valid_sw_formats[n]));
#  endif
    if (fc->valid_sw_formats[n] == AV_PIX_FMT_NV12 ||
        fc->valid_sw_formats[n] == AV_PIX_FMT_YUV420P) {
      foundSupportedFormat = true;
#  ifndef MOZ_LOGGING
      break;
#  endif
    }
  }

  if (!foundSupportedFormat) {
    FFMPEG_LOG("    %s target pixel format is not supported!",
               mLib->avcodec_get_name(aCodecID));
    return;
  }

  if (!aCodecList.Contains(aCodecID)) {
    aCodecList.AppendElement(aCodecID);
  }
}

nsTArray<AVCodecID> FFmpegVideoDecoder<LIBAV_VER>::GetAcceleratedFormats() {
  FFMPEG_LOG("FFmpegVideoDecoder::GetAcceleratedFormats()");

  VAProfile* profiles = nullptr;
  VAEntrypoint* entryPoints = nullptr;

  nsTArray<AVCodecID> supportedHWCodecs(AV_CODEC_ID_NONE);
#  ifdef MOZ_LOGGING
  auto printCodecs = MakeScopeExit([&] {
    FFMPEG_LOG("  Supported accelerated formats:");
    for (unsigned i = 0; i < supportedHWCodecs.Length(); i++) {
      FFMPEG_LOG("      %s", mLib->avcodec_get_name(supportedHWCodecs[i]));
    }
  });
#  endif

  AVVAAPIHWConfig* hwconfig =
      mLib->av_hwdevice_hwconfig_alloc(mVAAPIDeviceContext);
  if (!hwconfig) {
    FFMPEG_LOG("  failed to get AVVAAPIHWConfig");
    return supportedHWCodecs;
  }
  auto autoRelease = MakeScopeExit([&] {
    delete[] profiles;
    delete[] entryPoints;
    mLib->av_freep(&hwconfig);
  });

  int maxProfiles = vaMaxNumProfiles(mDisplay);
  int maxEntryPoints = vaMaxNumEntrypoints(mDisplay);
  if (MOZ_UNLIKELY(maxProfiles <= 0 || maxEntryPoints <= 0)) {
    return supportedHWCodecs;
  }

  profiles = new VAProfile[maxProfiles];
  int numProfiles = 0;
  VAStatus status = vaQueryConfigProfiles(mDisplay, profiles, &numProfiles);
  if (status != VA_STATUS_SUCCESS) {
    FFMPEG_LOG("  vaQueryConfigProfiles() failed %s", vaErrorStr(status));
    return supportedHWCodecs;
  }
  numProfiles = std::min(numProfiles, maxProfiles);

  entryPoints = new VAEntrypoint[maxEntryPoints];
  for (int p = 0; p < numProfiles; p++) {
    VAProfile profile = profiles[p];

    AVCodecID codecID = VAProfileToCodecID(profile);
    if (codecID == AV_CODEC_ID_NONE) {
      continue;
    }

    int numEntryPoints = 0;
    status = vaQueryConfigEntrypoints(mDisplay, profile, entryPoints,
                                      &numEntryPoints);
    if (status != VA_STATUS_SUCCESS) {
      FFMPEG_LOG("  vaQueryConfigEntrypoints() failed: '%s' for profile %d",
                 vaErrorStr(status), (int)profile);
      continue;
    }
    numEntryPoints = std::min(numEntryPoints, maxEntryPoints);

    FFMPEG_LOG("  Profile %s:", VAProfileName(profile));
    for (int e = 0; e < numEntryPoints; e++) {
      VAConfigID config = VA_INVALID_ID;
      status = vaCreateConfig(mDisplay, profile, entryPoints[e], nullptr, 0,
                              &config);
      if (status != VA_STATUS_SUCCESS) {
        FFMPEG_LOG("  vaCreateConfig() failed: '%s' for profile %d",
                   vaErrorStr(status), (int)profile);
        continue;
      }
      hwconfig->config_id = config;
      AddAcceleratedFormats(supportedHWCodecs, codecID, hwconfig);
      vaDestroyConfig(mDisplay, config);
    }
  }

  return supportedHWCodecs;
}

#endif

#ifdef MOZ_ENABLE_D3D11VA
MediaResult FFmpegVideoDecoder<LIBAV_VER>::InitD3D11VADecoder() {
  MOZ_DIAGNOSTIC_ASSERT(XRE_IsGPUProcess());
  FFMPEG_LOG("Initialising D3D11VA FFmpeg decoder");
  StaticMutexAutoLock mon(sMutex);

  if (!mImageAllocator || !mImageAllocator->SupportsD3D11()) {
    FFMPEG_LOG("  no KnowsCompositor or it doesn't support D3D11");
    return NS_ERROR_DOM_MEDIA_FATAL_ERR;
  }

  if (mInfo.mColorDepth > gfx::ColorDepth::COLOR_10) {
    return MediaResult(NS_ERROR_DOM_MEDIA_FATAL_ERR,
                       RESULT_DETAIL("not supported color depth"));
  }

  AVCodec* codec = FindVideoHardwareAVCodec(mLib, mCodecID);
  if (!codec) {
    FFMPEG_LOG("  couldn't find d3d11va decoder for %s",
               AVCodecToString(mCodecID));
    return MediaResult(NS_ERROR_DOM_MEDIA_FATAL_ERR,
                       RESULT_DETAIL("unable to find codec"));
  }
  FFMPEG_LOG("  codec %s : %s", codec->name, codec->long_name);

  if (!(mCodecContext = mLib->avcodec_alloc_context3(codec))) {
    FFMPEG_LOG("  couldn't init d3d11va ffmpeg context");
    return NS_ERROR_OUT_OF_MEMORY;
  }
  mCodecContext->opaque = this;
  InitHWCodecContext(ContextType::D3D11VA);

  auto releaseResources = MakeScopeExit([&] {
    if (mCodecContext) {
      mLib->av_freep(&mCodecContext);
    }
    if (mD3D11VADeviceContext) {
      AVHWDeviceContext* hwctx =
          reinterpret_cast<AVHWDeviceContext*>(mD3D11VADeviceContext->data);
      AVD3D11VADeviceContext* d3d11vactx =
          reinterpret_cast<AVD3D11VADeviceContext*>(hwctx->hwctx);
      d3d11vactx->device = nullptr;
      mLib->av_buffer_unref(&mD3D11VADeviceContext);
      mD3D11VADeviceContext = nullptr;
    }
    mDXVA2Manager.reset();
  });

  FFMPEG_LOG("  creating device context");
  mD3D11VADeviceContext = mLib->av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
  if (!mD3D11VADeviceContext) {
    FFMPEG_LOG("  av_hwdevice_ctx_alloc failed.");
    return NS_ERROR_DOM_MEDIA_FATAL_ERR;
  }

  nsAutoCString failureReason;
  mDXVA2Manager.reset(
      DXVA2Manager::CreateD3D11DXVA(mImageAllocator, failureReason));
  if (!mDXVA2Manager) {
    FFMPEG_LOG("  failed to create dxva manager.");
    return NS_ERROR_DOM_MEDIA_FATAL_ERR;
  }

  ID3D11Device* device = mDXVA2Manager->GetD3D11Device();
  if (!device) {
    FFMPEG_LOG("  failed to get D3D11 device.");
    return NS_ERROR_DOM_MEDIA_FATAL_ERR;
  }

  AVHWDeviceContext* hwctx = (AVHWDeviceContext*)mD3D11VADeviceContext->data;
  AVD3D11VADeviceContext* d3d11vactx = (AVD3D11VADeviceContext*)hwctx->hwctx;
  d3d11vactx->device = device;

  if (mLib->av_hwdevice_ctx_init(mD3D11VADeviceContext) < 0) {
    FFMPEG_LOG("  av_hwdevice_ctx_init failed.");
    return NS_ERROR_DOM_MEDIA_FATAL_ERR;
  }

  mCodecContext->hw_device_ctx = mLib->av_buffer_ref(mD3D11VADeviceContext);
  MediaResult ret = AllocateExtraData();
  if (NS_FAILED(ret)) {
    FFMPEG_LOG("  failed to allocate extradata.");
    return ret;
  }

  if (mLib->avcodec_open2(mCodecContext, codec, nullptr) < 0) {
    FFMPEG_LOG("  avcodec_open2 failed for d3d11va decoder");
    return NS_ERROR_DOM_MEDIA_FATAL_ERR;
  }

  FFMPEG_LOG("  D3D11VA FFmpeg init successful");
  releaseResources.release();
  return NS_OK;
}

MediaResult FFmpegVideoDecoder<LIBAV_VER>::CreateImageD3D11(
    int64_t aOffset, int64_t aPts, int64_t aDuration,
    MediaDataDecoder::DecodedData& aResults) {
  MOZ_DIAGNOSTIC_ASSERT(mFrame);
  MOZ_DIAGNOSTIC_ASSERT(mDXVA2Manager);

  HRESULT hr = mDXVA2Manager->ConfigureForSize(
      GetSurfaceFormat(), GetFrameColorSpace(), GetFrameColorRange(),
      mInfo.mColorDepth, mFrame->width, mFrame->height);
  if (FAILED(hr)) {
    nsPrintfCString msg("Failed to configure DXVA2Manager, hr=%lx", hr);
    FFMPEG_LOG("%s", msg.get());
    return MediaResult(NS_ERROR_DOM_MEDIA_DECODE_ERR, msg);
  }

  if (!mFrame->data[0]) {
    nsPrintfCString msg("Frame data shouldn't be null!");
    FFMPEG_LOG("%s", msg.get());
    return MediaResult(NS_ERROR_DOM_MEDIA_DECODE_ERR, msg);
  }

  ID3D11Resource* resource = reinterpret_cast<ID3D11Resource*>(mFrame->data[0]);
  RefPtr<ID3D11Texture2D> texture;
  hr = resource->QueryInterface(
      static_cast<ID3D11Texture2D**>(getter_AddRefs(texture)));
  if (FAILED(hr)) {
    nsPrintfCString msg("Failed to get ID3D11Texture2D, hr=%lx", hr);
    FFMPEG_LOG("%s", msg.get());
    return MediaResult(NS_ERROR_DOM_MEDIA_DECODE_ERR, msg);
  }

  RefPtr<Image> image;
  gfx::IntRect pictureRegion =
      mInfo.ScaledImageRect(mFrame->width, mFrame->height);
  UINT index = (uintptr_t)mFrame->data[1];

  if (CanUseZeroCopyVideoFrame()) {
    mNumOfHWTexturesInUse++;
    FFMPEGV_LOG("CreateImageD3D11, zero copy, index=%u (texInUse=%u)", index,
                mNumOfHWTexturesInUse.load());
    hr = mDXVA2Manager->WrapTextureWithImage(
        new D3D11TextureWrapper(
            mFrame, mLib, texture, index,
            [self = RefPtr<FFmpegVideoDecoder>(this), this]() {
              MOZ_ASSERT(mNumOfHWTexturesInUse > 0);
              mNumOfHWTexturesInUse--;
            }),
        pictureRegion, getter_AddRefs(image));
  } else {
    FFMPEGV_LOG("CreateImageD3D11, copy output to a shared texture");
    hr = mDXVA2Manager->CopyToImage(texture, index, pictureRegion,
                                    getter_AddRefs(image));
  }
  if (FAILED(hr)) {
    nsPrintfCString msg("Failed to create a D3D image");
    FFMPEG_LOG("%s", msg.get());
    return MediaResult(NS_ERROR_DOM_MEDIA_DECODE_ERR, msg);
  }
  MOZ_ASSERT(image);

  RefPtr<VideoData> v = VideoData::CreateFromImage(
      mInfo.mDisplay, aOffset, TimeUnit::FromMicroseconds(aPts),
      TimeUnit::FromMicroseconds(aDuration), image, !!mFrame->key_frame,
      TimeUnit::FromMicroseconds(mFrame->pkt_dts));
  if (!v) {
    nsPrintfCString msg("D3D image allocation error");
    FFMPEG_LOG("%s", msg.get());
    return MediaResult(NS_ERROR_DOM_MEDIA_DECODE_ERR, msg);
  }
  aResults.AppendElement(std::move(v));
  return NS_OK;
}

bool FFmpegVideoDecoder<LIBAV_VER>::CanUseZeroCopyVideoFrame() const {
  // When zero-copy is available, we use a hybrid approach that combines
  // zero-copy and texture copying. This prevents scenarios where all
  // zero-copy frames remain unreleased, which could block ffmpeg from
  // allocating new textures for subsequent frames. Zero-copy should only be
  // used when there is sufficient space available in the texture pool.
  return gfx::gfxVars::HwDecodedVideoZeroCopy() && mImageAllocator &&
         mImageAllocator->UsingHardwareWebRender() && mDXVA2Manager &&
         mDXVA2Manager->SupportsZeroCopyNV12Texture() &&
         mNumOfHWTexturesInUse <= EXTRA_HW_FRAMES / 2;
}
#endif

#if MOZ_USE_HWDECODE
/* static */ AVCodec* FFmpegVideoDecoder<LIBAV_VER>::FindVideoHardwareAVCodec(
    FFmpegLibWrapper* aLib, AVCodecID aCodec, AVHWDeviceType aDeviceType) {
#  ifdef MOZ_WIDGET_GTK
  if (aDeviceType == AV_HWDEVICE_TYPE_NONE) {
    switch (aCodec) {
      case AV_CODEC_ID_H264:
        return aLib->avcodec_find_decoder_by_name("h264_v4l2m2m");
      case AV_CODEC_ID_VP8:
        return aLib->avcodec_find_decoder_by_name("vp8_v4l2m2m");
      case AV_CODEC_ID_VP9:
        return aLib->avcodec_find_decoder_by_name("vp9_v4l2m2m");
      case AV_CODEC_ID_HEVC:
        return aLib->avcodec_find_decoder_by_name("hevc_v4l2m2m");
      default:
        return nullptr;
    }
  }
#  endif
  return FindHardwareAVCodec(aLib, aCodec, aDeviceType);
}
#endif

}  // namespace mozilla
