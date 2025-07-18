/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_SelectionMovementUtils_h
#define mozilla_SelectionMovementUtils_h

#include "mozilla/Attributes.h"
#include "mozilla/EnumSet.h"
#include "mozilla/RangeBoundary.h"
#include "mozilla/Result.h"
#include "mozilla/intl/BidiEmbeddingLevel.h"
#include "nsIFrame.h"

struct nsPrevNextBidiLevels;

namespace mozilla {

class PresShell;
enum class PeekOffsetOption : uint16_t;

namespace intl {
class BidiEmbeddingLevel;
}

struct MOZ_STACK_CLASS PrimaryFrameData {
  // The frame which should be used to layout the caret.
  nsIFrame* mFrame = nullptr;
  // The offset in content of mFrame.  This is valid only when mFrame is not
  // nullptr.
  uint32_t mOffsetInFrameContent = 0;
  // Whether the caret should be put before or after the point. This is valid
  // only when mFrame is not nullptr.
  CaretAssociationHint mHint{0};  // Before
};

struct MOZ_STACK_CLASS CaretFrameData : public PrimaryFrameData {
  // The frame which is found only from a DOM point.  This frame becomes
  // different from mFrame when the point is around end of a line or
  // at a bidi text boundary.
  nsIFrame* mUnadjustedFrame = nullptr;
};

enum class ForceEditableRegion : bool { No, Yes };

class SelectionMovementUtils final {
 public:
  using PeekOffsetOptions = EnumSet<PeekOffsetOption>;

  /**
   * @brief Creates a new `RangeBoundary` which moves `aAmount` into
   * `aDirection` from the input range boundary.
   *
   * @param aRangeBoundary   The input range boundary.
   * @param aDirection       The direction into which the new boundary should be
   *                         moved.
   * @param aHint            The `CaretAssociationHint` (is the caret before or
   *                         after the boundary point)
   * @param aCaretBidiLevel  The `BidiEmbeddingLevel`.
   * @param aAmount          The amount which the range boundary should be
   *                         moved.
   * @param aOptions         Additional options, see `PeekOffsetOption`.
   * @param aAncestorLimiter The content node that limits where Selection may be
   *                         expanded to.
   *
   * @return Returns a new `RangeBoundary` which is moved from `aRangeBoundary`
   *         by `aAmount` into `aDirection`.
   */
  template <typename ParentType, typename RefType>
  static Result<RangeBoundaryBase<ParentType, RefType>, nsresult>
  MoveRangeBoundaryToSomewhere(
      const RangeBoundaryBase<ParentType, RefType>& aRangeBoundary,
      nsDirection aDirection, CaretAssociationHint aHint,
      intl::BidiEmbeddingLevel aCaretBidiLevel, nsSelectionAmount aAmount,
      PeekOffsetOptions aOptions,
      const dom::Element* aAncestorLimiter = nullptr);

  /**
   * Given a node and its child offset, return the nsIFrame and the offset into
   * that frame.
   *
   * @param aNode input parameter for the node to look at
   *              TODO: Make this `const nsIContent*` for `ContentEventHandler`.
   * @param aOffset offset into above node.
   * @param aReturnOffset will contain offset into frame.
   */
  static nsIFrame* GetFrameForNodeOffset(nsIContent* aNode, uint32_t aOffset,
                                         CaretAssociationHint aHint,
                                         uint32_t* aReturnOffset = nullptr);

  /**
   * GetPrevNextBidiLevels will return the frames and associated Bidi levels of
   * the characters logically before and after a (collapsed) selection.
   *
   * @param aNode is the node containing the selection
   * @param aContentOffset is the offset of the selection in the node
   * @param aJumpLines
   *   If true, look across line boundaries.
   *   If false, behave as if there were base-level frames at line edges.
   * @param aAncestorLimiter  If set, this refers only the descendants.
   *
   * @return A struct holding the before/after frame and the before/after
   * level.
   *
   * At the beginning and end of each line there is assumed to be a frame with
   * Bidi level equal to the paragraph embedding level.
   *
   * In these cases the before frame and after frame respectively will be
   * nullptr.
   */
  static nsPrevNextBidiLevels GetPrevNextBidiLevels(
      nsIContent* aNode, uint32_t aContentOffset, CaretAssociationHint aHint,
      bool aJumpLines, const dom::Element* aAncestorLimiter);

  /**
   * PeekOffsetForCaretMove() only peek offset for caret move from the specified
   * point of the normal selection.  I.e., won't change selection ranges nor
   * bidi information.
   */
  static Result<PeekOffsetStruct, nsresult> PeekOffsetForCaretMove(
      nsIContent* aContent, uint32_t aOffset, nsDirection aDirection,
      CaretAssociationHint aHint, intl::BidiEmbeddingLevel aCaretBidiLevel,
      const nsSelectionAmount aAmount, const nsPoint& aDesiredCaretPos,
      PeekOffsetOptions aOptions, const dom::Element* aAncestorLimiter);

  /**
   * IsIntraLineCaretMove() is a helper method for PeekOffsetForCaretMove()
   * and CreateRangeExtendedToSomwhereFromNormalSelection().  This returns
   * whether aAmount is intra line move or is crossing hard line break.
   * This returns error if aMount is not supported by the methods.
   */
  static Result<bool, nsresult> IsIntraLineCaretMove(
      nsSelectionAmount aAmount) {
    switch (aAmount) {
      case eSelectCharacter:
      case eSelectCluster:
      case eSelectWord:
      case eSelectWordNoSpace:
      case eSelectBeginLine:
      case eSelectEndLine:
        return true;
      case eSelectLine:
        return false;
      default:
        return Err(NS_ERROR_FAILURE);
    }
  }

  /**
   * Return a frame for considering caret geometry.
   *
   * @param aFrameSelection     [optional] If this is specified and selection in
   *                            aContent is not managed by the specified
   *                            instance, return nullptr.
   * @param aContentNode        The content node where selection is collapsed.
   * @param aOffset             Collapsed position in aContentNode
   * @param aFrameHint          Caret association hint.
   * @param aBidiLevel
   * @param aForceEditableRegion    Whether selection should be limited in
   *                                editable region or not.
   */
  static CaretFrameData GetCaretFrameForNodeOffset(
      const nsFrameSelection* aFrameSelection, nsIContent* aContentNode,
      uint32_t aOffset, CaretAssociationHint aFrameHint,
      intl::BidiEmbeddingLevel aBidiLevel,
      ForceEditableRegion aForceEditableRegion);

  static bool AdjustFrameForLineStart(nsIFrame*& aFrame,
                                      uint32_t& aFrameOffset);

  /**
   * Get primary frame and some other data for putting caret or extending
   * selection at the point.
   */
  static PrimaryFrameData GetPrimaryFrameForCaret(
      nsIContent* aContent, uint32_t aOffset, bool aVisual,
      CaretAssociationHint aHint, intl::BidiEmbeddingLevel aCaretBidiLevel);

 private:
  /**
   * GetFrameFromLevel will scan in a given direction
   * until it finds a frame with a Bidi level less than or equal to a given
   * level. It will return the last frame before this.
   *
   * @param aPresContext is the context to use
   * @param aFrameIn is the frame to start from
   * @param aDirection is the direction to scan
   * @param aBidiLevel is the level to search for
   */
  static Result<nsIFrame*, nsresult> GetFrameFromLevel(
      nsIFrame* aFrameIn, nsDirection aDirection,
      intl::BidiEmbeddingLevel aBidiLevel);

  // This is helper method for GetPrimaryFrameForCaret.
  // If aVisual is true, this returns caret frame.
  // If false, this returns primary frame.
  static PrimaryFrameData GetPrimaryOrCaretFrameForNodeOffset(
      nsIContent* aContent, uint32_t aOffset, bool aVisual,
      CaretAssociationHint aHint, intl::BidiEmbeddingLevel aCaretBidiLevel);
};

}  // namespace mozilla

#endif  // #ifndef mozilla_SelectionMovementUtils_h
