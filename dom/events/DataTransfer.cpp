/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/ArrayUtils.h"
#include "mozilla/BasePrincipal.h"
#include "mozilla/BasicEvents.h"
#include "mozilla/CheckedInt.h"
#include "mozilla/Span.h"
#include "mozilla/StaticPrefs_dom.h"
#include "DataTransfer.h"

#include "nsISupportsPrimitives.h"
#include "nsIScriptSecurityManager.h"
#include "mozilla/dom/DOMStringList.h"
#include "nsArray.h"
#include "nsBaseClipboard.h"
#include "nsError.h"
#include "nsIDragService.h"
#include "nsIClipboard.h"
#include "nsIXPConnect.h"
#include "nsContentUtils.h"
#include "nsIContent.h"
#include "nsIContentAnalysis.h"
#include "nsIObjectInputStream.h"
#include "nsIObjectOutputStream.h"
#include "nsIStorageStream.h"
#include "nsStringStream.h"
#include "nsCRT.h"
#include "nsIScriptObjectPrincipal.h"
#include "nsIScriptContext.h"
#include "mozilla/dom/Document.h"
#include "nsIScriptGlobalObject.h"
#include "nsQueryObject.h"
#include "nsVariant.h"
#include "mozilla/ClipboardContentAnalysisChild.h"
#include "mozilla/ClipboardReadRequestChild.h"
#include "mozilla/dom/ContentChild.h"
#include "mozilla/dom/DataTransferBinding.h"
#include "mozilla/dom/DataTransferItemList.h"
#include "mozilla/dom/Directory.h"
#include "mozilla/dom/DocumentInlines.h"
#include "mozilla/dom/Element.h"
#include "mozilla/dom/Event.h"
#include "mozilla/dom/FileList.h"
#include "mozilla/dom/IPCBlobUtils.h"
#include "mozilla/dom/BindingUtils.h"
#include "mozilla/dom/OSFileSystem.h"
#include "mozilla/dom/Promise.h"
#include "mozilla/dom/WindowContext.h"
#include "mozilla/Unused.h"
#include "nsComponentManagerUtils.h"
#include "nsNetUtil.h"
#include "nsReadableUtils.h"
#include "nsPresContext.h"

namespace mozilla::dom {

NS_IMPL_CYCLE_COLLECTION_WRAPPERCACHE_CLASS(DataTransfer)

NS_IMPL_CYCLE_COLLECTION_UNLINK_BEGIN(DataTransfer)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mParent)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mItems)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mDragTarget)
  NS_IMPL_CYCLE_COLLECTION_UNLINK(mDragImage)
  NS_IMPL_CYCLE_COLLECTION_UNLINK_PRESERVED_WRAPPER
NS_IMPL_CYCLE_COLLECTION_UNLINK_END
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_BEGIN(DataTransfer)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mParent)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mItems)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mDragTarget)
  NS_IMPL_CYCLE_COLLECTION_TRAVERSE(mDragImage)
NS_IMPL_CYCLE_COLLECTION_TRAVERSE_END

NS_IMPL_CYCLE_COLLECTING_ADDREF(DataTransfer)
NS_IMPL_CYCLE_COLLECTING_RELEASE(DataTransfer)

NS_INTERFACE_MAP_BEGIN_CYCLE_COLLECTION(DataTransfer)
  NS_WRAPPERCACHE_INTERFACE_MAP_ENTRY
  NS_INTERFACE_MAP_ENTRY(mozilla::dom::DataTransfer)
  NS_INTERFACE_MAP_ENTRY(nsISupports)
NS_INTERFACE_MAP_END

// the size of the array
const char DataTransfer::sEffects[8][9] = {
    "none", "copy", "move", "copyMove", "link", "copyLink", "linkMove", "all"};

// Used for custom clipboard types.
enum CustomClipboardTypeId {
  eCustomClipboardTypeId_None,
  eCustomClipboardTypeId_String
};

static DataTransfer::Mode ModeForEvent(EventMessage aEventMessage) {
  switch (aEventMessage) {
    case eCut:
    case eCopy:
    case eDragStart:
      // For these events, we want to be able to add data to the data transfer,
      // Otherwise, the data is already present.
      return DataTransfer::Mode::ReadWrite;
    case eDrop:
    case ePaste:
    case ePasteNoFormatting:
    case eEditorInput:
      // For these events we want to be able to read the data which is stored in
      // the DataTransfer, rather than just the type information.
      return DataTransfer::Mode::ReadOnly;
    default:
      return StaticPrefs::dom_events_dataTransfer_protected_enabled()
                 ? DataTransfer::Mode::Protected
                 : DataTransfer::Mode::ReadOnly;
  }
}

DataTransfer::DataTransfer(
    nsISupports* aParent, EventMessage aEventMessage, bool aIsExternal,
    mozilla::Maybe<nsIClipboard::ClipboardType> aClipboardType)
    : mParent(aParent),
      mDropEffect(nsIDragService::DRAGDROP_ACTION_NONE),
      mEffectAllowed(nsIDragService::DRAGDROP_ACTION_UNINITIALIZED),
      mEventMessage(aEventMessage),
      mCursorState(false),
      mMode(ModeForEvent(aEventMessage)),
      mIsExternal(aIsExternal),
      mUserCancelled(false),
      mIsCrossDomainSubFrameDrop(false),
      mClipboardType(aClipboardType),
      mDragImageX(0),
      mDragImageY(0) {
  mItems = new DataTransferItemList(this);

  // For external usage, cache the data from the native clipboard or drag.
  if (mIsExternal && mMode != Mode::ReadWrite) {
    if (aEventMessage == ePasteNoFormatting) {
      mEventMessage = ePaste;
      CacheExternalClipboardFormats(true);
    } else if (aEventMessage == ePaste) {
      CacheExternalClipboardFormats(false);
    } else if (aEventMessage >= eDragDropEventFirst &&
               aEventMessage <= eDragDropEventLast) {
      CacheExternalDragFormats();
    }
  }
}

DataTransfer::DataTransfer(nsISupports* aParent, EventMessage aEventMessage,
                           nsITransferable* aTransferable)
    : mParent(aParent),
      mTransferable(aTransferable),
      mDropEffect(nsIDragService::DRAGDROP_ACTION_NONE),
      mEffectAllowed(nsIDragService::DRAGDROP_ACTION_UNINITIALIZED),
      mEventMessage(aEventMessage),
      mCursorState(false),
      mMode(ModeForEvent(aEventMessage)),
      mIsExternal(true),
      mUserCancelled(false),
      mIsCrossDomainSubFrameDrop(false),
      mDragImageX(0),
      mDragImageY(0) {
  mItems = new DataTransferItemList(this);

  // XXX Currently, we cannot make DataTransfer grabs mTransferable for long
  //     time because nsITransferable is not cycle collectable but this may
  //     be grabbed by JS.  Additionally, the data initializing path is too
  //     complicated (too optimized) for D&D and clipboard.  They are cached
  //     only formats first, then, data of all items will be filled by the
  //     items later and by themselves.  However, we shouldn't duplicate such
  //     path for saving the maintenance cost.  Therefore, we need to treat
  //     that DataTransfer and its items are in external mode.  Finally,
  //     release mTransferable and make them in internal mode.
  CacheTransferableFormats();
  FillAllExternalData();
  // Now, we have all necessary data of mTransferable.  So, we can work as
  // internal mode.
  mIsExternal = false;
  // Release mTransferable because it won't be referred anymore.
  mTransferable = nullptr;
}

DataTransfer::DataTransfer(nsISupports* aParent, EventMessage aEventMessage,
                           const nsAString& aString)
    : mParent(aParent),
      mDropEffect(nsIDragService::DRAGDROP_ACTION_NONE),
      mEffectAllowed(nsIDragService::DRAGDROP_ACTION_UNINITIALIZED),
      mEventMessage(aEventMessage),
      mCursorState(false),
      mMode(ModeForEvent(aEventMessage)),
      mIsExternal(false),
      mUserCancelled(false),
      mIsCrossDomainSubFrameDrop(false),
      mDragImageX(0),
      mDragImageY(0) {
  mItems = new DataTransferItemList(this);

  nsCOMPtr<nsIPrincipal> sysPrincipal = nsContentUtils::GetSystemPrincipal();

  RefPtr<nsVariantCC> variant = new nsVariantCC();
  variant->SetAsAString(aString);
  DebugOnly<nsresult> rvIgnored =
      SetDataWithPrincipal(u"text/plain"_ns, variant, 0, sysPrincipal, false);
  NS_WARNING_ASSERTION(NS_SUCCEEDED(rvIgnored),
                       "Failed to set given string to the DataTransfer object");
}

DataTransfer::DataTransfer(
    nsISupports* aParent, EventMessage aEventMessage,
    const uint32_t aEffectAllowed, bool aCursorState, bool aIsExternal,
    bool aUserCancelled, bool aIsCrossDomainSubFrameDrop,
    mozilla::Maybe<nsIClipboard::ClipboardType> aClipboardType,
    nsCOMPtr<nsIClipboardDataSnapshot> aClipboardDataSnapshot,
    DataTransferItemList* aItems, Element* aDragImage, uint32_t aDragImageX,
    uint32_t aDragImageY, bool aShowFailAnimation)
    : mParent(aParent),
      mDropEffect(nsIDragService::DRAGDROP_ACTION_NONE),
      mEffectAllowed(aEffectAllowed),
      mEventMessage(aEventMessage),
      mCursorState(aCursorState),
      mMode(ModeForEvent(aEventMessage)),
      mIsExternal(aIsExternal),
      mUserCancelled(aUserCancelled),
      mIsCrossDomainSubFrameDrop(aIsCrossDomainSubFrameDrop),
      mClipboardType(aClipboardType),
      mClipboardDataSnapshot(std::move(aClipboardDataSnapshot)),
      mDragImage(aDragImage),
      mDragImageX(aDragImageX),
      mDragImageY(aDragImageY),
      mShowFailAnimation(aShowFailAnimation) {
  MOZ_ASSERT(mParent);
  MOZ_ASSERT(aItems);

  // We clone the items array after everything else, so that it has a valid
  // mParent value
  mItems = aItems->Clone(this);
  // The items are copied from aItems into mItems. There is no need to copy
  // the actual data in the items as the data transfer will be read only. The
  // dragstart event is the only time when items are
  // modifiable, but those events should have been using the first constructor
  // above.
  NS_ASSERTION(aEventMessage != eDragStart,
               "invalid event type for DataTransfer constructor");
}

DataTransfer::~DataTransfer() = default;

// static
already_AddRefed<DataTransfer> DataTransfer::Constructor(
    const GlobalObject& aGlobal) {
  RefPtr<DataTransfer> transfer =
      new DataTransfer(aGlobal.GetAsSupports(), eCopy, /* is external */ false,
                       /* clipboard type */ Nothing());
  transfer->mEffectAllowed = nsIDragService::DRAGDROP_ACTION_NONE;
  return transfer.forget();
}

JSObject* DataTransfer::WrapObject(JSContext* aCx,
                                   JS::Handle<JSObject*> aGivenProto) {
  return DataTransfer_Binding::Wrap(aCx, this, aGivenProto);
}

void DataTransfer::SetDropEffect(const nsAString& aDropEffect) {
  // the drop effect can only be 'none', 'copy', 'move' or 'link'.
  for (uint32_t e = 0; e <= nsIDragService::DRAGDROP_ACTION_LINK; e++) {
    if (aDropEffect.EqualsASCII(sEffects[e])) {
      // don't allow copyMove
      if (e != (nsIDragService::DRAGDROP_ACTION_COPY |
                nsIDragService::DRAGDROP_ACTION_MOVE)) {
        mDropEffect = e;
      }
      break;
    }
  }
}

void DataTransfer::SetEffectAllowed(const nsAString& aEffectAllowed) {
  if (aEffectAllowed.EqualsLiteral("uninitialized")) {
    mEffectAllowed = nsIDragService::DRAGDROP_ACTION_UNINITIALIZED;
    return;
  }

  static_assert(nsIDragService::DRAGDROP_ACTION_NONE == 0,
                "DRAGDROP_ACTION_NONE constant is wrong");
  static_assert(nsIDragService::DRAGDROP_ACTION_COPY == 1,
                "DRAGDROP_ACTION_COPY constant is wrong");
  static_assert(nsIDragService::DRAGDROP_ACTION_MOVE == 2,
                "DRAGDROP_ACTION_MOVE constant is wrong");
  static_assert(nsIDragService::DRAGDROP_ACTION_LINK == 4,
                "DRAGDROP_ACTION_LINK constant is wrong");

  for (uint32_t e = 0; e < std::size(sEffects); e++) {
    if (aEffectAllowed.EqualsASCII(sEffects[e])) {
      mEffectAllowed = e;
      break;
    }
  }
}

void DataTransfer::GetMozTriggeringPrincipalURISpec(
    nsAString& aPrincipalURISpec) {
  auto* dragSession = GetOwnerDragSession();
  if (!dragSession) {
    aPrincipalURISpec.Truncate(0);
    return;
  }

  nsCOMPtr<nsIPrincipal> principal;
  dragSession->GetTriggeringPrincipal(getter_AddRefs(principal));
  if (!principal) {
    aPrincipalURISpec.Truncate(0);
    return;
  }

  nsAutoCString spec;
  principal->GetAsciiSpec(spec);
  CopyUTF8toUTF16(spec, aPrincipalURISpec);
}

nsIPolicyContainer* DataTransfer::GetPolicyContainer() {
  auto* dragSession = GetOwnerDragSession();
  if (!dragSession) {
    return nullptr;
  }
  nsCOMPtr<nsIPolicyContainer> policyContainer;
  dragSession->GetPolicyContainer(getter_AddRefs(policyContainer));
  return policyContainer;
}

already_AddRefed<FileList> DataTransfer::GetFiles(
    nsIPrincipal& aSubjectPrincipal) {
  return mItems->Files(&aSubjectPrincipal);
}

void DataTransfer::GetTypes(nsTArray<nsString>& aTypes,
                            CallerType aCallerType) const {
  // When called from bindings, aTypes will be empty, but since we might have
  // Gecko-internal callers too, clear it to be safe.
  aTypes.Clear();

  return mItems->GetTypes(aTypes, aCallerType);
}

bool DataTransfer::HasType(const nsAString& aType) const {
  return mItems->HasType(aType);
}

bool DataTransfer::HasFile() const { return mItems->HasFile(); }

void DataTransfer::GetData(const nsAString& aFormat, nsAString& aData,
                           nsIPrincipal& aSubjectPrincipal,
                           ErrorResult& aRv) const {
  // return an empty string if data for the format was not found
  aData.Truncate();

  nsCOMPtr<nsIVariant> data;
  nsresult rv =
      GetDataAtInternal(aFormat, 0, &aSubjectPrincipal, getter_AddRefs(data));
  if (NS_FAILED(rv)) {
    if (rv != NS_ERROR_DOM_INDEX_SIZE_ERR) {
      aRv.Throw(rv);
    }
    return;
  }

  if (data) {
    nsAutoString stringdata;
    data->GetAsAString(stringdata);

    // for the URL type, parse out the first URI from the list. The URIs are
    // separated by newlines
    nsAutoString lowercaseFormat;
    nsContentUtils::ASCIIToLower(aFormat, lowercaseFormat);

    if (lowercaseFormat.EqualsLiteral("url")) {
      int32_t lastidx = 0, idx;
      int32_t length = stringdata.Length();
      while (lastidx < length) {
        idx = stringdata.FindChar('\n', lastidx);
        // lines beginning with # are comments
        if (stringdata[lastidx] == '#') {
          if (idx == -1) {
            break;
          }
        } else {
          if (idx == -1) {
            aData.Assign(Substring(stringdata, lastidx));
          } else {
            aData.Assign(Substring(stringdata, lastidx, idx - lastidx));
          }
          aData =
              nsContentUtils::TrimWhitespace<nsCRT::IsAsciiSpace>(aData, true);
          return;
        }
        lastidx = idx + 1;
      }
    } else {
      aData = stringdata;
    }
  }
}

void DataTransfer::SetData(const nsAString& aFormat, const nsAString& aData,
                           nsIPrincipal& aSubjectPrincipal, ErrorResult& aRv) {
  RefPtr<nsVariantCC> variant = new nsVariantCC();
  variant->SetAsAString(aData);

  aRv = SetDataAtInternal(aFormat, variant, 0, &aSubjectPrincipal);
}

void DataTransfer::ClearData(const Optional<nsAString>& aFormat,
                             nsIPrincipal& aSubjectPrincipal,
                             ErrorResult& aRv) {
  if (IsReadOnly()) {
    aRv.Throw(NS_ERROR_DOM_NO_MODIFICATION_ALLOWED_ERR);
    return;
  }

  if (MozItemCount() == 0) {
    return;
  }

  if (aFormat.WasPassed()) {
    MozClearDataAtHelper(aFormat.Value(), 0, aSubjectPrincipal, aRv);
  } else {
    MozClearDataAtHelper(u""_ns, 0, aSubjectPrincipal, aRv);
  }
}

void DataTransfer::SetMozCursor(const nsAString& aCursorState) {
  // Lock the cursor to an arrow during the drag.
  mCursorState = aCursorState.EqualsLiteral("default");
}

already_AddRefed<nsINode> DataTransfer::GetMozSourceNode() {
  auto* dragSession = GetOwnerDragSession();
  if (!dragSession) {
    return nullptr;
  }

  nsCOMPtr<nsINode> sourceNode;
  dragSession->GetSourceNode(getter_AddRefs(sourceNode));
  if (sourceNode && !nsContentUtils::LegacyIsCallerNativeCode() &&
      !nsContentUtils::CanCallerAccess(sourceNode)) {
    return nullptr;
  }

  return sourceNode.forget();
}

already_AddRefed<WindowContext> DataTransfer::GetSourceTopWindowContext() {
  auto* dragSession = GetOwnerDragSession();
  if (!dragSession) {
    return nullptr;
  }

  RefPtr<WindowContext> sourceTopWindowContext;
  dragSession->GetSourceTopWindowContext(
      getter_AddRefs(sourceTopWindowContext));
  return sourceTopWindowContext.forget();
}

already_AddRefed<DOMStringList> DataTransfer::MozTypesAt(
    uint32_t aIndex, ErrorResult& aRv) const {
  // Only the first item is valid for clipboard events
  if (aIndex > 0 && (mEventMessage == eCut || mEventMessage == eCopy ||
                     mEventMessage == ePaste)) {
    aRv.Throw(NS_ERROR_DOM_INDEX_SIZE_ERR);
    return nullptr;
  }

  RefPtr<DOMStringList> types = new DOMStringList();
  if (aIndex < MozItemCount()) {
    // note that you can retrieve the types regardless of their principal
    const nsTArray<RefPtr<DataTransferItem>>& items =
        *mItems->MozItemsAt(aIndex);

    bool addFile = false;
    for (uint32_t i = 0; i < items.Length(); i++) {
      // NOTE: The reason why we get the internal type here is because we want
      // kFileMime to appear in the types list for backwards compatibility
      // reasons.
      nsAutoString type;
      items[i]->GetInternalType(type);
      if (NS_WARN_IF(!types->Add(type))) {
        aRv.Throw(NS_ERROR_FAILURE);
        return nullptr;
      }

      if (items[i]->Kind() == DataTransferItem::KIND_FILE) {
        addFile = true;
      }
    }

    if (addFile) {
      types->Add(u"Files"_ns);
    }
  }

  return types.forget();
}

nsresult DataTransfer::GetDataAtNoSecurityCheck(const nsAString& aFormat,
                                                uint32_t aIndex,
                                                nsIVariant** aData) const {
  return GetDataAtInternal(aFormat, aIndex,
                           nsContentUtils::GetSystemPrincipal(), aData);
}

nsresult DataTransfer::GetDataAtInternal(const nsAString& aFormat,
                                         uint32_t aIndex,
                                         nsIPrincipal* aSubjectPrincipal,
                                         nsIVariant** aData) const {
  *aData = nullptr;

  if (aFormat.IsEmpty()) {
    return NS_OK;
  }

  if (aIndex >= MozItemCount()) {
    return NS_ERROR_DOM_INDEX_SIZE_ERR;
  }

  // Only the first item is valid for clipboard events
  if (aIndex > 0 && (mEventMessage == eCut || mEventMessage == eCopy ||
                     mEventMessage == ePaste)) {
    return NS_ERROR_DOM_INDEX_SIZE_ERR;
  }

  nsAutoString format;
  GetRealFormat(aFormat, format);

  MOZ_ASSERT(aSubjectPrincipal);

  RefPtr<DataTransferItem> item = mItems->MozItemByTypeAt(format, aIndex);
  if (!item) {
    // The index exists but there's no data for the specified format, in this
    // case we just return undefined
    return NS_OK;
  }

  // If we have chrome only content, and we aren't chrome, don't allow access
  if (!aSubjectPrincipal->IsSystemPrincipal() && item->ChromeOnly()) {
    return NS_OK;
  }

  // DataTransferItem::Data() handles the principal checks
  ErrorResult result;
  nsCOMPtr<nsIVariant> data = item->Data(aSubjectPrincipal, result);
  if (NS_WARN_IF(!data || result.Failed())) {
    return result.StealNSResult();
  }

  data.forget(aData);
  return NS_OK;
}

void DataTransfer::MozGetDataAt(JSContext* aCx, const nsAString& aFormat,
                                uint32_t aIndex,
                                JS::MutableHandle<JS::Value> aRetval,
                                mozilla::ErrorResult& aRv) {
  nsCOMPtr<nsIVariant> data;
  aRv = GetDataAtInternal(aFormat, aIndex, nsContentUtils::GetSystemPrincipal(),
                          getter_AddRefs(data));
  if (aRv.Failed()) {
    return;
  }

  if (!data) {
    aRetval.setNull();
    return;
  }

  JS::Rooted<JS::Value> result(aCx);
  if (!VariantToJsval(aCx, data, aRetval)) {
    aRv = NS_ERROR_FAILURE;
    return;
  }
}

/* static */
bool DataTransfer::PrincipalMaySetData(const nsAString& aType,
                                       nsIVariant* aData,
                                       nsIPrincipal* aPrincipal) {
  if (!aPrincipal->IsSystemPrincipal()) {
    DataTransferItem::eKind kind = DataTransferItem::KindFromData(aData);
    if (kind == DataTransferItem::KIND_OTHER) {
      NS_WARNING("Disallowing adding non string/file types to DataTransfer");
      return false;
    }

    // Don't allow adding internal types of the form */x-moz-*, but
    // special-case the url types as they are simple variations of urls.
    // In addition, allow x-moz-place flavors to be added by WebExtensions.
    if (FindInReadable(kInternal_Mimetype_Prefix, aType) &&
        !StringBeginsWith(aType, u"text/x-moz-url"_ns)) {
      auto principal = BasePrincipal::Cast(aPrincipal);
      if (!principal->AddonPolicy() ||
          !StringBeginsWith(aType, u"text/x-moz-place"_ns)) {
        NS_WARNING("Disallowing adding this type to DataTransfer");
        return false;
      }
    }
  }

  return true;
}

void DataTransfer::TypesListMayHaveChanged() {
  DataTransfer_Binding::ClearCachedTypesValue(this);
}

already_AddRefed<DataTransfer> DataTransfer::MozCloneForEvent(
    const nsAString& aEvent, ErrorResult& aRv) {
  RefPtr<nsAtom> atomEvt = NS_Atomize(aEvent);
  if (!atomEvt) {
    aRv.Throw(NS_ERROR_OUT_OF_MEMORY);
    return nullptr;
  }
  EventMessage eventMessage = nsContentUtils::GetEventMessage(atomEvt);

  RefPtr<DataTransfer> dt;
  nsresult rv = Clone(mParent, eventMessage, false, false, getter_AddRefs(dt));
  if (NS_FAILED(rv)) {
    aRv.Throw(rv);
    return nullptr;
  }
  return dt.forget();
}

// The order of the types matters. `kFileMime` needs to be one of the first two
// types. And the order should be the same as the types order defined in
// MandatoryDataTypesAsCStrings() for Clipboard API.
static constexpr nsLiteralCString kNonPlainTextExternalFormats[] = {
    nsLiteralCString(kCustomTypesMime), nsLiteralCString(kFileMime),
    nsLiteralCString(kHTMLMime),        nsLiteralCString(kRTFMime),
    nsLiteralCString(kURLMime),         nsLiteralCString(kURLDataMime),
    nsLiteralCString(kTextMime),        nsLiteralCString(kPNGImageMime),
    nsLiteralCString(kPDFJSMime)};

namespace {
nsresult GetClipboardDataSnapshotWithContentAnalysisSync(
    const nsTArray<nsCString>& aFormats,
    const nsIClipboard::ClipboardType& aClipboardType,
    WindowContext* aWindowContext,
    nsIClipboardDataSnapshot** aClipboardDataSnapshot) {
  MOZ_ASSERT(aWindowContext);
  MOZ_ASSERT(nsIContentAnalysis::MightBeActive());
  nsresult rv;
  nsCOMPtr<nsITransferable> trans =
      do_CreateInstance("@mozilla.org/widget/transferable;1", &rv);
  NS_ENSURE_SUCCESS(rv, rv);
  trans->Init(nullptr);
  // Before anything reads the clipboard contents, do a full
  // content analysis on the clipboard contents (and cache it). This
  // prevents multiple content analysis dialogs from appearing
  // when multiple formats are read (see bug 1915351)
  RefPtr<ClipboardContentAnalysisChild> contentAnalysis =
      ClipboardContentAnalysisChild::GetOrCreate();
  IPCTransferableDataOrError ipcTransferableDataOrError;
  bool result = contentAnalysis->SendGetAllClipboardDataSync(
      aFormats, aClipboardType, aWindowContext->InnerWindowId(),
      &ipcTransferableDataOrError);
  NS_ENSURE_TRUE(result, NS_ERROR_FAILURE);
  if (ipcTransferableDataOrError.type() ==
      IPCTransferableDataOrError::Tnsresult) {
    rv = ipcTransferableDataOrError.get_nsresult();
    // This class expects clipboardDataSnapshot to be non-null, so
    // return an empty one
    if (rv == NS_ERROR_CONTENT_BLOCKED) {
      auto emptySnapshot =
          mozilla::MakeRefPtr<nsBaseClipboard::ClipboardPopulatedDataSnapshot>(
              trans);
      emptySnapshot.forget(aClipboardDataSnapshot);
    }
    return rv;
  }
  rv = nsContentUtils::IPCTransferableDataToTransferable(
      ipcTransferableDataOrError.get_IPCTransferableData(),
      true /* aAddDataFlavor */, trans, false /* aFilterUnknownFlavors */);
  NS_ENSURE_SUCCESS(rv, rv);
  auto snapshot =
      mozilla::MakeRefPtr<nsBaseClipboard::ClipboardPopulatedDataSnapshot>(
          trans);
  snapshot.forget(aClipboardDataSnapshot);
  return rv;
}
}  // namespace

void DataTransfer::GetExternalClipboardFormats(const bool& aPlainTextOnly,
                                               nsTArray<nsCString>& aResult) {
  // NOTE: When you change this method, you may need to change
  //       GetExternalTransferableFormats() too since those methods should
  //       work similarly.

  MOZ_ASSERT(!mClipboardDataSnapshot);

  if (mClipboardType.isNothing()) {
    return;
  }

  RefPtr<WindowContext> wc = GetWindowContext();
  if (NS_WARN_IF(!wc)) {
    MOZ_ASSERT_UNREACHABLE(
        "How could this DataTransfer be created with a non-window global?");
    return;
  }

  nsCOMPtr<nsIClipboard> clipboard =
      do_GetService("@mozilla.org/widget/clipboard;1");
  if (!clipboard) {
    return;
  }

  nsresult rv = NS_ERROR_FAILURE;
  // If we're in the parent process already this content is exempt from
  // content analysis (i.e. pasting into the URL bar)
  bool doContentAnalysis = MOZ_UNLIKELY(nsIContentAnalysis::MightBeActive()) &&
                           XRE_IsContentProcess();

  nsCOMPtr<nsIClipboardDataSnapshot> clipboardDataSnapshot;
  if (aPlainTextOnly) {
    AutoTArray<nsCString, 1> formats{nsLiteralCString(kTextMime)};
    if (doContentAnalysis) {
      rv = GetClipboardDataSnapshotWithContentAnalysisSync(
          formats, *mClipboardType, wc, getter_AddRefs(clipboardDataSnapshot));
    } else {
      rv = clipboard->GetDataSnapshotSync(
          formats, *mClipboardType, wc, getter_AddRefs(clipboardDataSnapshot));
    }
  } else {
    AutoTArray<nsCString, std::size(kNonPlainTextExternalFormats) + 4> formats;
    formats.AppendElements(
        Span<const nsLiteralCString>(kNonPlainTextExternalFormats));
    // We will be using this snapshot to provide the data to paste in
    // EditorBase, so add a few extra formats here to make sure we have
    // everything. Note that these extra formats will not be returned in aResult
    // because of the checks below.
    formats.AppendElement(kNativeHTMLMime);
    formats.AppendElement(kJPEGImageMime);
    formats.AppendElement(kGIFImageMime);
    formats.AppendElement(kMozTextInternal);

    if (doContentAnalysis) {
      rv = GetClipboardDataSnapshotWithContentAnalysisSync(
          formats, *mClipboardType, wc, getter_AddRefs(clipboardDataSnapshot));
    } else {
      rv = clipboard->GetDataSnapshotSync(
          formats, *mClipboardType, wc, getter_AddRefs(clipboardDataSnapshot));
    }
  }

  if (NS_FAILED(rv) || !clipboardDataSnapshot) {
    if (rv == NS_ERROR_CONTENT_BLOCKED) {
      // Use the empty snapshot created in
      // GetClipboardDataSnapshotWithContentAnalysisSync()
      mClipboardDataSnapshot = clipboardDataSnapshot;
    }
    return;
  }

  // Order is important for DataTransfer; ensure the returned list items follow
  // the sequence specified in kNonPlainTextExternalFormats.
  AutoTArray<nsCString, std::size(kNonPlainTextExternalFormats)> flavors;
  clipboardDataSnapshot->GetFlavorList(flavors);
  for (const auto& format : kNonPlainTextExternalFormats) {
    if (flavors.Contains(format)) {
      aResult.AppendElement(format);
    }
  }

  mClipboardDataSnapshot = clipboardDataSnapshot;
}

/* static */
void DataTransfer::GetExternalTransferableFormats(
    nsITransferable* aTransferable, bool aPlainTextOnly,
    nsTArray<nsCString>* aResult) {
  MOZ_ASSERT(aTransferable);
  MOZ_ASSERT(aResult);

  aResult->Clear();

  // NOTE: When you change this method, you may need to change
  //       GetExternalClipboardFormats() too since those methods should
  //       work similarly.

  AutoTArray<nsCString, 10> flavors;
  aTransferable->FlavorsTransferableCanExport(flavors);

  if (aPlainTextOnly) {
    auto index = flavors.IndexOf(nsLiteralCString(kTextMime));
    if (index != flavors.NoIndex) {
      aResult->AppendElement(nsLiteralCString(kTextMime));
    }
    return;
  }

  // If not plain text only, then instead check all the other types
  for (const auto& format : kNonPlainTextExternalFormats) {
    auto index = flavors.IndexOf(format);
    if (index != flavors.NoIndex) {
      aResult->AppendElement(format);
    }
  }
}

nsresult DataTransfer::SetDataAtInternal(const nsAString& aFormat,
                                         nsIVariant* aData, uint32_t aIndex,
                                         nsIPrincipal* aSubjectPrincipal) {
  if (aFormat.IsEmpty()) {
    return NS_OK;
  }

  if (IsReadOnly()) {
    return NS_ERROR_DOM_NO_MODIFICATION_ALLOWED_ERR;
  }

  // Specifying an index less than the current length will replace an existing
  // item. Specifying an index equal to the current length will add a new item.
  if (aIndex > MozItemCount()) {
    return NS_ERROR_DOM_INDEX_SIZE_ERR;
  }

  // Only the first item is valid for clipboard events
  if (aIndex > 0 && (mEventMessage == eCut || mEventMessage == eCopy ||
                     mEventMessage == ePaste)) {
    return NS_ERROR_DOM_INDEX_SIZE_ERR;
  }

  // Don't allow the custom type to be assigned.
  if (aFormat.EqualsLiteral(kCustomTypesMime)) {
    return NS_ERROR_DOM_NOT_SUPPORTED_ERR;
  }

  if (!PrincipalMaySetData(aFormat, aData, aSubjectPrincipal)) {
    return NS_ERROR_DOM_SECURITY_ERR;
  }

  return SetDataWithPrincipal(aFormat, aData, aIndex, aSubjectPrincipal);
}

void DataTransfer::MozSetDataAt(JSContext* aCx, const nsAString& aFormat,
                                JS::Handle<JS::Value> aData, uint32_t aIndex,
                                ErrorResult& aRv) {
  nsCOMPtr<nsIVariant> data;
  aRv = nsContentUtils::XPConnect()->JSValToVariant(aCx, aData,
                                                    getter_AddRefs(data));
  if (!aRv.Failed()) {
    aRv = SetDataAtInternal(aFormat, data, aIndex,
                            nsContentUtils::GetSystemPrincipal());
  }
}

void DataTransfer::MozClearDataAt(const nsAString& aFormat, uint32_t aIndex,
                                  ErrorResult& aRv) {
  if (IsReadOnly()) {
    aRv.Throw(NS_ERROR_DOM_NO_MODIFICATION_ALLOWED_ERR);
    return;
  }

  if (aIndex >= MozItemCount()) {
    aRv.Throw(NS_ERROR_DOM_INDEX_SIZE_ERR);
    return;
  }

  // Only the first item is valid for clipboard events
  if (aIndex > 0 && (mEventMessage == eCut || mEventMessage == eCopy ||
                     mEventMessage == ePaste)) {
    aRv.Throw(NS_ERROR_DOM_INDEX_SIZE_ERR);
    return;
  }

  MozClearDataAtHelper(aFormat, aIndex, *nsContentUtils::GetSystemPrincipal(),
                       aRv);

  // If we just cleared the 0-th index, and there are still more than 1 indexes
  // remaining, MozClearDataAt should cause the 1st index to become the 0th
  // index. This should _only_ happen when the MozClearDataAt function is
  // explicitly called by script, as this behavior is inconsistent with spec.
  // (however, so is the MozClearDataAt API)

  if (aIndex == 0 && mItems->MozItemCount() > 1 &&
      mItems->MozItemsAt(0)->Length() == 0) {
    mItems->PopIndexZero();
  }
}

void DataTransfer::MozClearDataAtHelper(const nsAString& aFormat,
                                        uint32_t aIndex,
                                        nsIPrincipal& aSubjectPrincipal,
                                        ErrorResult& aRv) {
  MOZ_ASSERT(!IsReadOnly());
  MOZ_ASSERT(aIndex < MozItemCount());
  MOZ_ASSERT(aIndex == 0 || (mEventMessage != eCut && mEventMessage != eCopy &&
                             mEventMessage != ePaste));

  nsAutoString format;
  GetRealFormat(aFormat, format);

  mItems->MozRemoveByTypeAt(format, aIndex, aSubjectPrincipal, aRv);
}

void DataTransfer::SetDragImage(Element& aImage, int32_t aX, int32_t aY) {
  if (!IsReadOnly()) {
    mDragImage = &aImage;
    mDragImageX = aX;
    mDragImageY = aY;
  }
}

void DataTransfer::UpdateDragImage(Element& aImage, int32_t aX, int32_t aY) {
  if (mEventMessage < eDragDropEventFirst ||
      mEventMessage > eDragDropEventLast) {
    return;
  }

  auto* dragSession = GetOwnerDragSession();
  if (dragSession) {
    dragSession->UpdateDragImage(&aImage, aX, aY);
  }
}

void DataTransfer::AddElement(Element& aElement, ErrorResult& aRv) {
  if (IsReadOnly()) {
    aRv.Throw(NS_ERROR_DOM_NO_MODIFICATION_ALLOWED_ERR);
    return;
  }

  mDragTarget = &aElement;
}

nsresult DataTransfer::Clone(nsISupports* aParent, EventMessage aEventMessage,
                             bool aUserCancelled,
                             bool aIsCrossDomainSubFrameDrop,
                             DataTransfer** aNewDataTransfer) {
  RefPtr<DataTransfer> newDataTransfer = new DataTransfer(
      aParent, aEventMessage, mEffectAllowed, mCursorState, mIsExternal,
      aUserCancelled, aIsCrossDomainSubFrameDrop, mClipboardType,
      mClipboardDataSnapshot, mItems, mDragImage, mDragImageX, mDragImageY,
      mShowFailAnimation);

  newDataTransfer.forget(aNewDataTransfer);
  return NS_OK;
}

already_AddRefed<nsIArray> DataTransfer::GetTransferables(
    nsINode* aDragTarget) {
  MOZ_ASSERT(aDragTarget);

  Document* doc = aDragTarget->GetComposedDoc();
  if (!doc) {
    return nullptr;
  }

  return GetTransferables(doc->GetLoadContext());
}

already_AddRefed<nsIArray> DataTransfer::GetTransferables(
    nsILoadContext* aLoadContext) {
  nsCOMPtr<nsIMutableArray> transArray = nsArray::Create();
  if (!transArray) {
    return nullptr;
  }

  uint32_t count = MozItemCount();
  for (uint32_t i = 0; i < count; i++) {
    nsCOMPtr<nsITransferable> transferable = GetTransferable(i, aLoadContext);
    if (transferable) {
      transArray->AppendElement(transferable);
    }
  }

  return transArray.forget();
}

already_AddRefed<nsITransferable> DataTransfer::GetTransferable(
    uint32_t aIndex, nsILoadContext* aLoadContext) {
  if (aIndex >= MozItemCount()) {
    return nullptr;
  }

  const nsTArray<RefPtr<DataTransferItem>>& item = *mItems->MozItemsAt(aIndex);
  uint32_t count = item.Length();
  if (!count) {
    return nullptr;
  }

  nsCOMPtr<nsITransferable> transferable =
      do_CreateInstance("@mozilla.org/widget/transferable;1");
  if (!transferable) {
    return nullptr;
  }
  transferable->Init(aLoadContext);

  // Set the principal of the global this DataTransfer was created for
  // on the transferable for ReadWrite events (copy, cut, or dragstart).
  //
  // For other events, the data inside the transferable may originate
  // from another origin or from the OS.
  if (mMode == Mode::ReadWrite) {
    if (nsCOMPtr<nsIGlobalObject> global = GetGlobal()) {
      transferable->SetDataPrincipal(global->PrincipalOrNull());
    }
  }

  nsCOMPtr<nsIStorageStream> storageStream;
  nsCOMPtr<nsIObjectOutputStream> stream;

  bool added = false;
  bool handlingCustomFormats = true;

  // When writing the custom data, we need to ensure that there is sufficient
  // space for a (uint32_t) data ending type, and the null byte character at
  // the end of the nsCString. We claim that space upfront and store it in
  // baseLength. This value will be set to zero if a write error occurs
  // indicating that the data and length are no longer valid.
  const uint32_t baseLength = sizeof(uint32_t) + 1;
  uint32_t totalCustomLength = baseLength;

  /*
   * Two passes are made here to iterate over all of the types. First, look for
   * any types that are not in the list of known types. For this pass,
   * handlingCustomFormats will be true. Data that corresponds to unknown types
   * will be pulled out and inserted into a single type (kCustomTypesMime) by
   * writing the data into a stream.
   *
   * The second pass will iterate over the formats looking for known types.
   * These are added as is. The unknown types are all then inserted as a single
   * type (kCustomTypesMime) in the same position of the first custom type. This
   * model is used to maintain the format order as best as possible.
   *
   * The format of the kCustomTypesMime type is one or more of the following
   * stored sequentially:
   *   <32-bit> type (only none or string is supported)
   *   <32-bit> length of format
   *   <wide string> format
   *   <32-bit> length of data
   *   <wide string> data
   * A type of eCustomClipboardTypeId_None ends the list, without any following
   * data.
   */
  do {
    for (uint32_t f = 0; f < count; f++) {
      RefPtr<DataTransferItem> formatitem = item[f];
      nsCOMPtr<nsIVariant> variant = formatitem->DataNoSecurityCheck();
      if (!variant) {  // skip empty items
        continue;
      }

      nsAutoString type;
      formatitem->GetInternalType(type);

      // If the data is of one of the well-known formats, use it directly.
      bool isCustomFormat = true;
      for (const char* format : kKnownFormats) {
        if (type.EqualsASCII(format)) {
          isCustomFormat = false;
          break;
        }
      }

      uint32_t lengthInBytes;
      nsCOMPtr<nsISupports> convertedData;

      if (handlingCustomFormats) {
        if (!ConvertFromVariant(variant, getter_AddRefs(convertedData),
                                &lengthInBytes)) {
          continue;
        }

        // When handling custom types, add the data to the stream if this is a
        // custom type. If totalCustomLength is 0, then a write error occurred
        // on a previous item, so ignore any others.
        if (isCustomFormat && totalCustomLength > 0) {
          // If it isn't a string, just ignore it. The dataTransfer is cached in
          // the drag sesion during drag-and-drop, so non-strings will be
          // available when dragging locally.
          nsCOMPtr<nsISupportsString> str(do_QueryInterface(convertedData));
          if (str) {
            nsAutoString data;
            str->GetData(data);

            if (!stream) {
              // Create a storage stream to write to.
              NS_NewStorageStream(1024, UINT32_MAX,
                                  getter_AddRefs(storageStream));

              nsCOMPtr<nsIOutputStream> outputStream;
              storageStream->GetOutputStream(0, getter_AddRefs(outputStream));

              stream = NS_NewObjectOutputStream(outputStream);
            }

            CheckedInt<uint32_t> formatLength =
                CheckedInt<uint32_t>(type.Length()) *
                sizeof(nsString::char_type);

            // The total size of the stream is the format length, the data
            // length, two integers to hold the lengths and one integer for
            // the string flag. Guard against large data by ignoring any that
            // don't fit.
            CheckedInt<uint32_t> newSize = formatLength + totalCustomLength +
                                           lengthInBytes +
                                           (sizeof(uint32_t) * 3);
            if (newSize.isValid()) {
              // If a write error occurs, set totalCustomLength to 0 so that
              // further processing gets ignored.
              nsresult rv = stream->Write32(eCustomClipboardTypeId_String);
              if (NS_WARN_IF(NS_FAILED(rv))) {
                totalCustomLength = 0;
                continue;
              }
              rv = stream->Write32(formatLength.value());
              if (NS_WARN_IF(NS_FAILED(rv))) {
                totalCustomLength = 0;
                continue;
              }
              MOZ_ASSERT(formatLength.isValid() &&
                             formatLength.value() ==
                                 type.Length() * sizeof(nsString::char_type),
                         "Why is formatLength off?");
              rv = stream->WriteBytes(
                  AsBytes(Span(type.BeginReading(), type.Length())));
              if (NS_WARN_IF(NS_FAILED(rv))) {
                totalCustomLength = 0;
                continue;
              }
              rv = stream->Write32(lengthInBytes);
              if (NS_WARN_IF(NS_FAILED(rv))) {
                totalCustomLength = 0;
                continue;
              }
              // XXXbz it's not obvious to me that lengthInBytes is the actual
              // length of "data" if the variant contained an nsISupportsString
              // as VTYPE_INTERFACE, say.  We used lengthInBytes above for
              // sizing, so just keep doing that.
              rv = stream->WriteBytes(
                  Span(reinterpret_cast<const uint8_t*>(data.BeginReading()),
                       lengthInBytes));
              if (NS_WARN_IF(NS_FAILED(rv))) {
                totalCustomLength = 0;
                continue;
              }

              totalCustomLength = newSize.value();
            }
          }
        }
      } else if (isCustomFormat && stream) {
        // This is the second pass of the loop (handlingCustomFormats is false).
        // When encountering the first custom format, append all of the stream
        // at this position. If totalCustomLength is 0 indicating a write error
        // occurred, or no data has been added to it, don't output anything,
        if (totalCustomLength > baseLength) {
          // Write out an end of data terminator.
          nsresult rv = stream->Write32(eCustomClipboardTypeId_None);
          if (NS_SUCCEEDED(rv)) {
            nsCOMPtr<nsIInputStream> inputStream;
            storageStream->NewInputStream(0, getter_AddRefs(inputStream));

            RefPtr<StringBuffer> stringBuffer =
                StringBuffer::Alloc(totalCustomLength);

            // Subtract off the null terminator when reading.
            totalCustomLength--;

            // Read the data from the stream and add a null-terminator as
            // ToString needs it.
            uint32_t amountRead;
            rv = inputStream->Read(static_cast<char*>(stringBuffer->Data()),
                                   totalCustomLength, &amountRead);
            if (NS_SUCCEEDED(rv)) {
              static_cast<char*>(stringBuffer->Data())[amountRead] = 0;

              nsCString str;
              str.Assign(stringBuffer, totalCustomLength);
              nsCOMPtr<nsISupportsCString> strSupports(
                  do_CreateInstance(NS_SUPPORTS_CSTRING_CONTRACTID));
              strSupports->SetData(str);

              nsresult rv =
                  transferable->SetTransferData(kCustomTypesMime, strSupports);
              if (NS_FAILED(rv)) {
                return nullptr;
              }

              added = true;
            }
          }
        }

        // Clear the stream so it doesn't get used again.
        stream = nullptr;
      } else {
        // This is the second pass of the loop and a known type is encountered.
        // Add it as is.
        if (!ConvertFromVariant(variant, getter_AddRefs(convertedData),
                                &lengthInBytes)) {
          continue;
        }

        NS_ConvertUTF16toUTF8 format(type);

        // If a converter is set for a format, set the converter for the
        // transferable and don't add the item
        nsCOMPtr<nsIFormatConverter> converter =
            do_QueryInterface(convertedData);
        if (converter) {
          transferable->AddDataFlavor(format.get());
          transferable->SetConverter(converter);
          continue;
        }

        nsresult rv =
            transferable->SetTransferData(format.get(), convertedData);
        if (NS_FAILED(rv)) {
          return nullptr;
        }

        added = true;
      }
    }

    handlingCustomFormats = !handlingCustomFormats;
  } while (!handlingCustomFormats);

  // only return the transferable if data was successfully added to it
  if (added) {
    return transferable.forget();
  }

  return nullptr;
}

bool DataTransfer::ConvertFromVariant(nsIVariant* aVariant,
                                      nsISupports** aSupports,
                                      uint32_t* aLength) const {
  *aSupports = nullptr;
  *aLength = 0;

  uint16_t type = aVariant->GetDataType();
  if (type == nsIDataType::VTYPE_INTERFACE ||
      type == nsIDataType::VTYPE_INTERFACE_IS) {
    nsCOMPtr<nsISupports> data;
    if (NS_FAILED(aVariant->GetAsISupports(getter_AddRefs(data)))) {
      return false;
    }

    // For flavour data providers, use 0 as the length.
    if (nsCOMPtr<nsIFlavorDataProvider> fdp = do_QueryInterface(data)) {
      fdp.forget(aSupports);
      *aLength = 0;
      return true;
    }

    // Only use the underlying BlobImpl for transferables.
    if (RefPtr<Blob> blob = do_QueryObject(data)) {
      RefPtr<BlobImpl> blobImpl = blob->Impl();
      blobImpl.forget(aSupports);
    } else {
      data.forget(aSupports);
    }

    *aLength = sizeof(nsISupports*);
    return true;
  }

  nsAutoString str;
  nsresult rv = aVariant->GetAsAString(str);
  if (NS_FAILED(rv)) {
    return false;
  }

  nsCOMPtr<nsISupportsString> strSupports(
      do_CreateInstance(NS_SUPPORTS_STRING_CONTRACTID));
  if (!strSupports) {
    return false;
  }

  strSupports->SetData(str);

  strSupports.forget(aSupports);

  // each character is two bytes
  *aLength = str.Length() * 2;

  return true;
}

void DataTransfer::Disconnect() {
  SetMode(Mode::Protected);
  if (StaticPrefs::dom_events_dataTransfer_protected_enabled()) {
    ClearAll();
  }
}

void DataTransfer::ClearAll() {
  mItems->ClearAllItems();
  mClipboardDataSnapshot = nullptr;
}

uint32_t DataTransfer::MozItemCount() const { return mItems->MozItemCount(); }

nsresult DataTransfer::SetDataWithPrincipal(const nsAString& aFormat,
                                            nsIVariant* aData, uint32_t aIndex,
                                            nsIPrincipal* aPrincipal,
                                            bool aHidden) {
  nsAutoString format;
  GetRealFormat(aFormat, format);

  ErrorResult rv;
  RefPtr<DataTransferItem> item =
      mItems->SetDataWithPrincipal(format, aData, aIndex, aPrincipal,
                                   /* aInsertOnly = */ false, aHidden, rv);
  return rv.StealNSResult();
}

void DataTransfer::SetDataWithPrincipalFromOtherProcess(
    const nsAString& aFormat, nsIVariant* aData, uint32_t aIndex,
    nsIPrincipal* aPrincipal, bool aHidden) {
  if (aFormat.EqualsLiteral(kCustomTypesMime)) {
    FillInExternalCustomTypes(aData, aIndex, aPrincipal);
  } else {
    nsAutoString format;
    GetRealFormat(aFormat, format);

    ErrorResult rv;
    RefPtr<DataTransferItem> item =
        mItems->SetDataWithPrincipal(format, aData, aIndex, aPrincipal,
                                     /* aInsertOnly = */ false, aHidden, rv);
    if (NS_WARN_IF(rv.Failed())) {
      rv.SuppressException();
    }
  }
}

void DataTransfer::GetRealFormat(const nsAString& aInFormat,
                                 nsAString& aOutFormat) const {
  // For compatibility, treat text/unicode as equivalent to text/plain
  nsAutoString lowercaseFormat;
  nsContentUtils::ASCIIToLower(aInFormat, lowercaseFormat);
  if (lowercaseFormat.EqualsLiteral("text") ||
      lowercaseFormat.EqualsLiteral("text/unicode")) {
    aOutFormat.AssignLiteral("text/plain");
    return;
  }

  if (lowercaseFormat.EqualsLiteral("url")) {
    aOutFormat.AssignLiteral("text/uri-list");
    return;
  }

  aOutFormat.Assign(lowercaseFormat);
}

already_AddRefed<nsIGlobalObject> DataTransfer::GetGlobal() const {
  nsCOMPtr<nsIGlobalObject> global;
  // This is annoying, but DataTransfer may have various things as parent.
  if (nsCOMPtr<EventTarget> target = do_QueryInterface(mParent)) {
    global = target->GetOwnerGlobal();
  } else if (RefPtr<Event> event = do_QueryObject(mParent)) {
    global = event->GetParentObject();
  }

  return global.forget();
}

already_AddRefed<WindowContext> DataTransfer::GetWindowContext() const {
  nsCOMPtr<nsIGlobalObject> global = GetGlobal();
  if (!global) {
    return nullptr;
  }

  const auto* innerWindow = global->GetAsInnerWindow();
  if (!innerWindow) {
    return nullptr;
  }

  return do_AddRef(innerWindow->GetWindowContext());
}

nsIClipboardDataSnapshot* DataTransfer::GetClipboardDataSnapshot() const {
  return mClipboardDataSnapshot;
}

nsresult DataTransfer::CacheExternalData(const char* aFormat, uint32_t aIndex,
                                         nsIPrincipal* aPrincipal,
                                         bool aHidden) {
  ErrorResult rv;
  RefPtr<DataTransferItem> item;

  if (strcmp(aFormat, kTextMime) == 0) {
    item = mItems->SetDataWithPrincipal(u"text/plain"_ns, nullptr, aIndex,
                                        aPrincipal, false, aHidden, rv);
    if (NS_WARN_IF(rv.Failed())) {
      return rv.StealNSResult();
    }
    return NS_OK;
  }

  if (strcmp(aFormat, kURLDataMime) == 0) {
    item = mItems->SetDataWithPrincipal(u"text/uri-list"_ns, nullptr, aIndex,
                                        aPrincipal, false, aHidden, rv);
    if (NS_WARN_IF(rv.Failed())) {
      return rv.StealNSResult();
    }
    return NS_OK;
  }

  nsAutoString format;
  GetRealFormat(NS_ConvertUTF8toUTF16(aFormat), format);
  item = mItems->SetDataWithPrincipal(format, nullptr, aIndex, aPrincipal,
                                      false, aHidden, rv);
  if (NS_WARN_IF(rv.Failed())) {
    return rv.StealNSResult();
  }
  return NS_OK;
}

void DataTransfer::CacheExternalDragFormats() {
  // Called during the constructor to cache the formats available from an
  // external drag. The data associated with each format will be set to null.
  // This data will instead only be retrieved in FillInExternalDragData when
  // asked for, as it may be time consuming for the source application to
  // generate it.
  auto* dragSession = GetOwnerDragSession();
  if (!dragSession) {
    return;
  }

  // make sure that the system principal is used for external drags
  nsIScriptSecurityManager* ssm = nsContentUtils::GetSecurityManager();
  nsCOMPtr<nsIPrincipal> sysPrincipal;
  ssm->GetSystemPrincipal(getter_AddRefs(sysPrincipal));

  // there isn't a way to get a list of the formats that might be available on
  // all platforms, so just check for the types that can actually be imported
  // XXXndeakin there are some other formats but those are platform specific.
  // NOTE: kFileMime must have index 0
  // TODO: should this be `kNonPlainTextExternalFormats` instead?
  static const char* formats[] = {kFileMime,    kHTMLMime, kURLMime,
                                  kURLDataMime, kTextMime, kPNGImageMime};

  uint32_t count;
  dragSession->GetNumDropItems(&count);
  for (uint32_t c = 0; c < count; c++) {
    bool hasFileData = false;
    dragSession->IsDataFlavorSupported(kFileMime, &hasFileData);

    // First, check for the special format that holds custom types.
    bool supported;
    dragSession->IsDataFlavorSupported(kCustomTypesMime, &supported);
    if (supported) {
      FillInExternalCustomTypes(c, sysPrincipal);
    }

    for (uint32_t f = 0; f < std::size(formats); f++) {
      // IsDataFlavorSupported doesn't take an index as an argument and just
      // checks if any of the items support a particular flavor, even though
      // the GetData method does take an index. Here, we just assume that
      // every item being dragged has the same set of flavors.
      bool supported;
      dragSession->IsDataFlavorSupported(formats[f], &supported);
      // if the format is supported, add an item to the array with null as
      // the data. When retrieved, GetRealData will read the data.
      if (supported) {
        CacheExternalData(formats[f], c, sysPrincipal,
                          /* hidden = */ f && hasFileData);
      }
    }
  }
}

void DataTransfer::CacheExternalClipboardFormats(bool aPlainTextOnly) {
  // Called during the constructor for paste events to cache the formats
  // available on the clipboard. As with CacheExternalDragFormats, the
  // data will only be retrieved when needed.
  NS_ASSERTION(mEventMessage == ePaste,
               "caching clipboard data for invalid event");

  nsCOMPtr<nsIPrincipal> sysPrincipal = nsContentUtils::GetSystemPrincipal();
  nsTArray<nsCString> typesArray;
  GetExternalClipboardFormats(aPlainTextOnly, typesArray);
  if (aPlainTextOnly) {
    // The only thing that will be in types is kTextMime
    MOZ_ASSERT(typesArray.IsEmpty() || typesArray.Length() == 1);
    if (typesArray.Length() == 1) {
      MOZ_ASSERT(typesArray.Contains(kTextMime));
      CacheExternalData(kTextMime, 0, sysPrincipal, false);
    }
    return;
  }

  CacheExternalData(typesArray, sysPrincipal);
}

void DataTransfer::CacheTransferableFormats() {
  nsCOMPtr<nsIPrincipal> sysPrincipal = nsContentUtils::GetSystemPrincipal();

  AutoTArray<nsCString, 10> typesArray;
  GetExternalTransferableFormats(mTransferable, false, &typesArray);

  CacheExternalData(typesArray, sysPrincipal);
}

void DataTransfer::CacheExternalData(const nsTArray<nsCString>& aTypes,
                                     nsIPrincipal* aPrincipal) {
  bool hasFileData = false;
  for (const nsCString& type : aTypes) {
    if (type.EqualsLiteral(kCustomTypesMime)) {
      FillInExternalCustomTypes(0, aPrincipal);
    } else if (type.EqualsLiteral(kFileMime) && XRE_IsContentProcess() &&
               !StaticPrefs::dom_events_dataTransfer_mozFile_enabled()) {
      // We will be ignoring any application/x-moz-file files found in the paste
      // datatransfer within e10s, as they will fail top be sent over IPC.
      // Because of that, we will unset hasFileData, whether or not it would
      // have been set. (bug 1308007)
      hasFileData = false;
      continue;
    } else {
      // We expect that if kFileMime is supported, then it will be the either at
      // index 0 or at index 1 in the aTypes returned by
      // GetExternalClipboardFormats
      if (type.EqualsLiteral(kFileMime)) {
        hasFileData = true;
      }

      // If we aren't the file data, and we have file data, we want to be hidden
      CacheExternalData(
          type.get(), 0, aPrincipal,
          /* hidden = */ !type.EqualsLiteral(kFileMime) && hasFileData);
    }
  }
}

void DataTransfer::FillAllExternalData() {
  if (mIsExternal) {
    for (uint32_t i = 0; i < MozItemCount(); ++i) {
      const nsTArray<RefPtr<DataTransferItem>>& items = *mItems->MozItemsAt(i);
      for (uint32_t j = 0; j < items.Length(); ++j) {
        MOZ_ASSERT(items[j]->Index() == i);

        items[j]->FillInExternalData();
      }
    }
  }
}

void DataTransfer::FillInExternalCustomTypes(uint32_t aIndex,
                                             nsIPrincipal* aPrincipal) {
  RefPtr<DataTransferItem> item = new DataTransferItem(
      this, NS_LITERAL_STRING_FROM_CSTRING(kCustomTypesMime),
      DataTransferItem::KIND_STRING);
  item->SetIndex(aIndex);

  nsCOMPtr<nsIVariant> variant = item->DataNoSecurityCheck();
  if (!variant) {
    return;
  }

  FillInExternalCustomTypes(variant, aIndex, aPrincipal);
}

/* static */ void DataTransfer::ParseExternalCustomTypesString(
    mozilla::Span<const char> aString,
    std::function<void(ParseExternalCustomTypesStringData&&)>&& aCallback) {
  CheckedInt<int32_t> checkedLen(aString.Length());
  if (!checkedLen.isValid()) {
    return;
  }

  nsCOMPtr<nsIInputStream> stringStream;
  NS_NewByteInputStream(getter_AddRefs(stringStream), aString,
                        NS_ASSIGNMENT_DEPEND);

  nsCOMPtr<nsIObjectInputStream> stream = NS_NewObjectInputStream(stringStream);

  uint32_t type;
  do {
    nsresult rv = stream->Read32(&type);
    NS_ENSURE_SUCCESS_VOID(rv);
    if (type == eCustomClipboardTypeId_String) {
      uint32_t formatLength;
      rv = stream->Read32(&formatLength);
      NS_ENSURE_SUCCESS_VOID(rv);
      char* formatBytes;
      rv = stream->ReadBytes(formatLength, &formatBytes);
      NS_ENSURE_SUCCESS_VOID(rv);
      nsAutoString format;
      format.Adopt(reinterpret_cast<char16_t*>(formatBytes),
                   formatLength / sizeof(char16_t));

      uint32_t dataLength;
      rv = stream->Read32(&dataLength);
      NS_ENSURE_SUCCESS_VOID(rv);
      char* dataBytes;
      rv = stream->ReadBytes(dataLength, &dataBytes);
      NS_ENSURE_SUCCESS_VOID(rv);
      nsAutoString data;
      data.Adopt(reinterpret_cast<char16_t*>(dataBytes),
                 dataLength / sizeof(char16_t));

      aCallback(ParseExternalCustomTypesStringData(std::move(format),
                                                   std::move(data)));
    }
  } while (type != eCustomClipboardTypeId_None);
}

void DataTransfer::FillInExternalCustomTypes(nsIVariant* aData, uint32_t aIndex,
                                             nsIPrincipal* aPrincipal) {
  char* chrs;
  uint32_t len = 0;
  nsresult rv = aData->GetAsStringWithSize(&len, &chrs);
  if (NS_FAILED(rv)) {
    return;
  }
  auto freeChrs = MakeScopeExit([&]() { free(chrs); });

  ParseExternalCustomTypesString(
      mozilla::Span(chrs, len),
      [&](ParseExternalCustomTypesStringData&& aData) {
        auto [format, data] = std::move(aData);
        RefPtr<nsVariantCC> variant = new nsVariantCC();
        if (NS_FAILED(variant->SetAsAString(data))) {
          return;
        }

        SetDataWithPrincipal(format, variant, aIndex, aPrincipal);
      });
}

void DataTransfer::SetMode(DataTransfer::Mode aMode) {
  if (!StaticPrefs::dom_events_dataTransfer_protected_enabled() &&
      aMode == Mode::Protected) {
    mMode = Mode::ReadOnly;
  } else {
    mMode = aMode;
  }
}

nsIWidget* DataTransfer::GetOwnerWidget() {
  RefPtr<WindowContext> wc = GetWindowContext();
  NS_ENSURE_TRUE(wc, nullptr);
  auto* doc = wc->GetDocument();
  NS_ENSURE_TRUE(doc, nullptr);
  auto* pc = doc->GetPresContext();
  NS_ENSURE_TRUE(pc, nullptr);
  return pc->GetRootWidget();
}

nsIDragSession* DataTransfer::GetOwnerDragSession() {
  auto* widget = GetOwnerWidget();
  nsCOMPtr<nsIDragSession> dragSession = nsContentUtils::GetDragSession(widget);
  return dragSession;
}

void DataTransfer::ClearForPaste() {
  MOZ_ASSERT(mEventMessage == ePaste,
             "ClearForPaste() should only be called on ePaste messages");
  Disconnect();

  // NOTE: Disconnect may not actually clear the DataTransfer if the
  // dom.events.dataTransfer.protected.enabled pref is not on, so we make
  // sure we clear here, as not clearing could provide the DataTransfer
  // access to information from the system clipboard at an arbitrary point
  // in the future.
  ClearAll();
}

bool DataTransfer::HasPrivateHTMLFlavor() const {
  MOZ_ASSERT(mEventMessage == ePaste,
             "Only works for ePaste messages, where the mClipboardDataSnapshot "
             "is available.");
  nsIClipboardDataSnapshot* snapshot = GetClipboardDataSnapshot();
  if (!snapshot) {
    NS_WARNING("DataTransfer::GetClipboardDataSnapshot() returned null");
    return false;
  }
  nsTArray<nsCString> snapshotFlavors;
  if (NS_FAILED(snapshot->GetFlavorList(snapshotFlavors))) {
    NS_WARNING("nsIClipboardDataSnapshot::GetFlavorList() failed");
    return false;
  }
  return snapshotFlavors.Contains(kHTMLContext);
}

}  // namespace mozilla::dom
