/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "InspectorFontFace.h"

#include "brotli/decode.h"
#include "gfxPlatformFontList.h"
#include "gfxTextRun.h"
#include "gfxUserFontSet.h"
#include "harfbuzz/hb-ot.h"
#include "mozilla/ServoBindings.h"
#include "mozilla/Unused.h"
#include "mozilla/dom/CSSFontFaceRule.h"
#include "mozilla/dom/FontFaceSet.h"
#include "mozilla/gfx/2D.h"
#include "nsFontFaceLoader.h"
#include "zlib.h"

namespace mozilla {
namespace dom {

InspectorFontFace::InspectorFontFace(gfxFontEntry* aFontEntry,
                                     gfxFontGroup* aFontGroup,
                                     FontMatchType aMatchType)
    : mFontEntry(aFontEntry), mFontGroup(aFontGroup), mMatchType(aMatchType) {
  MOZ_COUNT_CTOR(InspectorFontFace);
}

InspectorFontFace::~InspectorFontFace() { MOZ_COUNT_DTOR(InspectorFontFace); }

bool InspectorFontFace::FromFontGroup() {
  return bool(mMatchType.kind & FontMatchType::Kind::kFontGroup);
}

bool InspectorFontFace::FromLanguagePrefs() {
  return bool(mMatchType.kind & FontMatchType::Kind::kPrefsFallback);
}

bool InspectorFontFace::FromSystemFallback() {
  return bool(mMatchType.kind & FontMatchType::Kind::kSystemFallback);
}

void InspectorFontFace::GetName(nsAString& aName) {
  if (mFontEntry->IsUserFont() && !mFontEntry->IsLocalUserFont()) {
    NS_ASSERTION(mFontEntry->mUserFontData, "missing userFontData");
    aName.Append(NS_ConvertUTF8toUTF16(mFontEntry->mUserFontData->mRealName));
  } else {
    aName.Append(NS_ConvertUTF8toUTF16(mFontEntry->RealFaceName()));
  }
}

void InspectorFontFace::GetCSSFamilyName(nsAString& aCSSFamilyName) {
  aCSSFamilyName.Append(NS_ConvertUTF8toUTF16(mFontEntry->FamilyName()));
}

void InspectorFontFace::GetCSSGeneric(nsAString& aName) {
  if (mMatchType.generic != StyleGenericFontFamily::None) {
    aName.AssignASCII(gfxPlatformFontList::GetGenericName(mMatchType.generic));
  } else {
    aName.Truncate(0);
  }
}

void InspectorFontFace::GetNameString(uint16_t aNameId, nsAString& aResult) {
  gfxFontEntry::AutoHBFace face = mFontEntry->GetHBFace();
  unsigned int textSize = 0;
  unsigned int len = hb_ot_name_get_utf16(face, aNameId, HB_LANGUAGE_INVALID,
                                          &textSize, nullptr);
  if (len) {
    aResult.SetLength(len + 1);  // Ensure there is space for NUL terminator.
    textSize = len + 1;  // Tell HB the total size of the available buffer.
    len = hb_ot_name_get_utf16(
        face, aNameId, HB_LANGUAGE_INVALID, &textSize,
        reinterpret_cast<uint16_t*>(aResult.BeginWriting()));
    aResult.SetLength(len);  // Size the string to exclude terminator.
  } else {
    aResult.Truncate(0);
  }
}

CSSFontFaceRule* InspectorFontFace::GetRule() {
  if (!mRule) {
    // check whether this font entry is associated with an @font-face rule
    // in the relevant font group's user font set
    StyleLockedFontFaceRule* rule = nullptr;
    if (mFontEntry->IsUserFont()) {
      auto* fontFaceSet =
          static_cast<FontFaceSetImpl*>(mFontGroup->GetUserFontSet());
      if (fontFaceSet) {
        rule = fontFaceSet->FindRuleForEntry(mFontEntry);
      }
    }
    if (rule) {
      // XXX It would be better if we can share this with CSSOM tree,
      // but that may require us to create another map, which is not
      // great either. As far as they would use the same backend, and
      // we don't really support mutating @font-face rule via CSSOM,
      // it's probably fine for now.
      uint32_t line, column;
      Servo_FontFaceRule_GetSourceLocation(rule, &line, &column);
      mRule =
          new CSSFontFaceRule(do_AddRef(rule), nullptr, nullptr, line, column);
    }
  }
  return mRule;
}

int32_t InspectorFontFace::SrcIndex() {
  if (mFontEntry->IsUserFont()) {
    NS_ASSERTION(mFontEntry->mUserFontData, "missing userFontData");
    return mFontEntry->mUserFontData->mSrcIndex;
  }

  return -1;
}

void InspectorFontFace::GetURI(nsAString& aURI) {
  aURI.Truncate();
  if (mFontEntry->IsUserFont() && !mFontEntry->IsLocalUserFont()) {
    NS_ASSERTION(mFontEntry->mUserFontData, "missing userFontData");
    if (mFontEntry->mUserFontData->mURI) {
      nsAutoCString spec;
      mFontEntry->mUserFontData->mURI->GetSpec(spec);
      AppendUTF8toUTF16(spec, aURI);
    }
  }
}

void InspectorFontFace::GetLocalName(nsAString& aLocalName) {
  aLocalName.Truncate();
  if (mFontEntry->IsLocalUserFont()) {
    NS_ASSERTION(mFontEntry->mUserFontData, "missing userFontData");
    aLocalName.Append(
        NS_ConvertUTF8toUTF16(mFontEntry->mUserFontData->mLocalName));
  }
}

void InspectorFontFace::GetFormat(nsAString& aFormat) {
  aFormat.Truncate();
  if (mFontEntry->IsUserFont() && !mFontEntry->IsLocalUserFont()) {
    NS_ASSERTION(mFontEntry->mUserFontData, "missing userFontData");
    switch (mFontEntry->mUserFontData->mFormatHint) {
      case StyleFontFaceSourceFormatKeyword::None:
        break;
      case StyleFontFaceSourceFormatKeyword::Collection:
        aFormat.AssignLiteral("collection");
        break;
      case StyleFontFaceSourceFormatKeyword::Opentype:
        aFormat.AssignLiteral("opentype");
        break;
      case StyleFontFaceSourceFormatKeyword::Truetype:
        aFormat.AssignLiteral("truetype");
        break;
      case StyleFontFaceSourceFormatKeyword::EmbeddedOpentype:
        aFormat.AssignLiteral("embedded-opentype");
        break;
      case StyleFontFaceSourceFormatKeyword::Svg:
        aFormat.AssignLiteral("svg");
        break;
      case StyleFontFaceSourceFormatKeyword::Woff:
        aFormat.AssignLiteral("woff");
        break;
      case StyleFontFaceSourceFormatKeyword::Woff2:
        aFormat.AssignLiteral("woff2");
        break;
      case StyleFontFaceSourceFormatKeyword::Unknown:
        aFormat.AssignLiteral("unknown!");
        break;
    }
  }
}

void InspectorFontFace::GetMetadata(nsAString& aMetadata) {
  aMetadata.Truncate();
  if (mFontEntry->IsUserFont() && !mFontEntry->IsLocalUserFont()) {
    NS_ASSERTION(mFontEntry->mUserFontData, "missing userFontData");
    const gfxUserFontData* userFontData = mFontEntry->mUserFontData.get();
    if (userFontData->mMetadata.Length() && userFontData->mMetaOrigLen) {
      nsAutoCString str;
      str.SetLength(userFontData->mMetaOrigLen);
      if (str.Length() == userFontData->mMetaOrigLen) {
        switch (userFontData->mCompression) {
          case gfxUserFontData::kZlibCompression: {
            uLongf destLen = userFontData->mMetaOrigLen;
            if (uncompress((Bytef*)(str.BeginWriting()), &destLen,
                           (const Bytef*)(userFontData->mMetadata.Elements()),
                           userFontData->mMetadata.Length()) == Z_OK &&
                destLen == userFontData->mMetaOrigLen) {
              AppendUTF8toUTF16(str, aMetadata);
            }
          } break;
          case gfxUserFontData::kBrotliCompression: {
            size_t decodedSize = userFontData->mMetaOrigLen;
            if (BrotliDecoderDecompress(userFontData->mMetadata.Length(),
                                        userFontData->mMetadata.Elements(),
                                        &decodedSize,
                                        (uint8_t*)str.BeginWriting()) == 1 &&
                decodedSize == userFontData->mMetaOrigLen) {
              AppendUTF8toUTF16(str, aMetadata);
            }
          } break;
        }
      }
    }
  }
}

// Append an OpenType tag to a string as a 4-ASCII-character code.
static void AppendTagAsASCII(nsAString& aString, uint32_t aTag) {
  aString.AppendPrintf("%c%c%c%c", (aTag >> 24) & 0xff, (aTag >> 16) & 0xff,
                       (aTag >> 8) & 0xff, aTag & 0xff);
}

void InspectorFontFace::GetVariationAxes(
    nsTArray<InspectorVariationAxis>& aResult, ErrorResult& aRV) {
  if (!mFontEntry->HasVariations()) {
    return;
  }
  AutoTArray<gfxFontVariationAxis, 4> axes;
  mFontEntry->GetVariationAxes(axes);
  MOZ_ASSERT(!axes.IsEmpty());
  if (!aResult.SetCapacity(axes.Length(), mozilla::fallible)) {
    aRV.Throw(NS_ERROR_OUT_OF_MEMORY);
    return;
  }
  for (auto a : axes) {
    InspectorVariationAxis& axis = *aResult.AppendElement();
    AppendTagAsASCII(axis.mTag, a.mTag);
    axis.mName.Append(NS_ConvertUTF8toUTF16(a.mName));
    axis.mMinValue = a.mMinValue;
    axis.mMaxValue = a.mMaxValue;
    axis.mDefaultValue = a.mDefaultValue;
  }
}

void InspectorFontFace::GetVariationInstances(
    nsTArray<InspectorVariationInstance>& aResult, ErrorResult& aRV) {
  if (!mFontEntry->HasVariations()) {
    return;
  }
  AutoTArray<gfxFontVariationInstance, 16> instances;
  mFontEntry->GetVariationInstances(instances);
  if (!aResult.SetCapacity(instances.Length(), mozilla::fallible)) {
    aRV.Throw(NS_ERROR_OUT_OF_MEMORY);
    return;
  }
  for (const auto& i : instances) {
    InspectorVariationInstance& inst = *aResult.AppendElement();
    inst.mName.Append(NS_ConvertUTF8toUTF16(i.mName));
    // inst.mValues is a webidl sequence<>, which is a fallible array,
    // so we are required to use fallible SetCapacity and AppendElement calls,
    // and check the result. In practice we don't expect failure here; the
    // list of values cannot get huge because of limits in the font format.
    if (!inst.mValues.SetCapacity(i.mValues.Length(), mozilla::fallible)) {
      aRV.Throw(NS_ERROR_OUT_OF_MEMORY);
      return;
    }
    for (const auto& v : i.mValues) {
      InspectorVariationValue value;
      AppendTagAsASCII(value.mAxis, v.mAxis);
      value.mValue = v.mValue;
      // This won't fail, because of SetCapacity above.
      Unused << inst.mValues.AppendElement(value, mozilla::fallible);
    }
  }
}

void InspectorFontFace::GetFeatures(nsTArray<InspectorFontFeature>& aResult,
                                    ErrorResult& aRV) {
  AutoTArray<gfxFontFeatureInfo, 64> features;
  mFontEntry->GetFeatureInfo(features);
  if (features.IsEmpty()) {
    return;
  }
  if (!aResult.SetCapacity(features.Length(), mozilla::fallible)) {
    aRV.Throw(NS_ERROR_OUT_OF_MEMORY);
    return;
  }
  for (auto& f : features) {
    InspectorFontFeature& feat = *aResult.AppendElement();
    AppendTagAsASCII(feat.mTag, f.mTag);
    AppendTagAsASCII(feat.mScript, f.mScript);
    AppendTagAsASCII(feat.mLanguageSystem, f.mLangSys);
  }
}

void InspectorFontFace::GetRanges(nsTArray<RefPtr<nsRange>>& aResult) {
  aResult = mRanges.Clone();
}

void InspectorFontFace::AddRange(nsRange* aRange) {
  mRanges.AppendElement(aRange);
}

}  // namespace dom
}  // namespace mozilla
