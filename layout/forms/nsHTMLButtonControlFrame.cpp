/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsHTMLButtonControlFrame.h"

#include <algorithm>

#include "mozilla/Baseline.h"
#include "mozilla/PresShell.h"
#include "nsCSSRendering.h"
#include "nsContainerFrame.h"
#include "nsDisplayList.h"
#include "nsGkAtoms.h"
#include "nsIFrameInlines.h"
#include "nsLayoutUtils.h"
#include "nsPresContext.h"
#include "nsPresContextInlines.h"

using namespace mozilla;
using namespace mozilla::image;

nsContainerFrame* NS_NewHTMLButtonControlFrame(PresShell* aPresShell,
                                               ComputedStyle* aStyle) {
  return new (aPresShell)
      nsHTMLButtonControlFrame(aStyle, aPresShell->GetPresContext());
}

NS_IMPL_FRAMEARENA_HELPERS(nsHTMLButtonControlFrame)

nsHTMLButtonControlFrame::nsHTMLButtonControlFrame(ComputedStyle* aStyle,
                                                   nsPresContext* aPresContext,
                                                   nsIFrame::ClassID aID)
    : nsContainerFrame(aStyle, aPresContext, aID) {}

nsHTMLButtonControlFrame::~nsHTMLButtonControlFrame() = default;

NS_QUERYFRAME_HEAD(nsHTMLButtonControlFrame)
  NS_QUERYFRAME_ENTRY(nsHTMLButtonControlFrame)
NS_QUERYFRAME_TAIL_INHERITING(nsContainerFrame)

#ifdef ACCESSIBILITY
a11y::AccType nsHTMLButtonControlFrame::AccessibleType() {
  return a11y::eHTMLButtonType;
}
#endif

nsresult nsHTMLButtonControlFrame::HandleEvent(nsPresContext* aPresContext,
                                               WidgetGUIEvent* aEvent,
                                               nsEventStatus* aEventStatus) {
  if (mContent->AsElement()->IsDisabled()) {
    // If disabled do nothing
    return NS_OK;
  }

  // mouse clicks are handled by content
  // we don't want our children to get any events. So just pass it to frame.
  return nsIFrame::HandleEvent(aPresContext, aEvent, aEventStatus);
}

bool nsHTMLButtonControlFrame::ShouldClipPaintingToBorderBox() const {
  // FIXME(emilio): probably should account for per-axis clipping...
  return StyleDisplay()->mOverflowX != StyleOverflow::Visible;
}

void nsHTMLButtonControlFrame::BuildDisplayList(
    nsDisplayListBuilder* aBuilder, const nsDisplayListSet& aLists) {
  if (IsVisibleForPainting()) {
    // Clip the button itself to its border area for event hit testing.
    Maybe<DisplayListClipState::AutoSaveRestore> eventClipState;
    if (aBuilder->IsForEventDelivery()) {
      eventClipState.emplace(aBuilder);
      nsRect rect(aBuilder->ToReferenceFrame(this), GetSize());
      nscoord radii[8];
      bool hasRadii = GetBorderRadii(radii);
      eventClipState->ClipContainingBlockDescendants(
          rect, hasRadii ? radii : nullptr);
    }

    DisplayBorderBackgroundOutline(aBuilder, aLists);
  }

  {
    DisplayListClipState::AutoSaveRestore clipState(aBuilder);

    if (ShouldClipPaintingToBorderBox()) {
      nsMargin border = StyleBorder()->GetComputedBorder();
      nsRect rect(aBuilder->ToReferenceFrame(this), GetSize());
      rect.Deflate(border);
      nscoord radii[8];
      const bool hasRadii = GetPaddingBoxBorderRadii(radii);
      clipState.ClipContainingBlockDescendants(rect,
                                               hasRadii ? radii : nullptr);
    }

    BuildDisplayListForChild(aBuilder, mFrames.FirstChild(), aLists,
                             DisplayChildFlag::ForcePseudoStackingContext);
  }

  // To draw border when selected in editor
  DisplaySelectionOverlay(aBuilder, aLists.Content());
}

nscoord nsHTMLButtonControlFrame::IntrinsicISize(
    const IntrinsicSizeInput& aInput, IntrinsicISizeType aType) {
  if (Maybe<nscoord> containISize = ContainIntrinsicISize()) {
    return *containISize;
  }
  return nsLayoutUtils::IntrinsicForContainer(
      aInput.mContext, mFrames.FirstChild(), aType,
      aInput.mPercentageBasisForChildren);
}

void nsHTMLButtonControlFrame::Reflow(nsPresContext* aPresContext,
                                      ReflowOutput& aDesiredSize,
                                      const ReflowInput& aReflowInput,
                                      nsReflowStatus& aStatus) {
  MarkInReflow();
  DO_GLOBAL_REFLOW_COUNT("nsHTMLButtonControlFrame");
  MOZ_ASSERT(aStatus.IsEmpty(), "Caller should pass a fresh reflow status!");

  // Reflow the child
  nsIFrame* firstKid = mFrames.FirstChild();

  MOZ_ASSERT(firstKid, "Button should have a child frame for its contents");
  MOZ_ASSERT(!firstKid->GetNextSibling(),
             "Button should have exactly one child frame");
  MOZ_ASSERT(
      firstKid->Style()->GetPseudoType() == PseudoStyleType::buttonContent,
      "Button's child frame has unexpected pseudo type!");

  // XXXbz Eventually we may want to check-and-bail if
  // !aReflowInput.ShouldReflowAllKids() &&
  // !firstKid->IsSubtreeDirty().
  // We'd need to cache our ascent for that, of course.

  // Reflow the contents of the button.
  // (This populates our aDesiredSize, too.)
  ReflowButtonContents(aPresContext, aDesiredSize, aReflowInput, firstKid);

  if (!ShouldClipPaintingToBorderBox()) {
    ConsiderChildOverflow(aDesiredSize.mOverflowAreas, firstKid);
  }
  // else, we ignore child overflow -- anything that overflows beyond our
  // own border-box will get clipped when painting.

  FinishReflowWithAbsoluteFrames(aPresContext, aDesiredSize, aReflowInput,
                                 aStatus);

  // We're always complete and we don't support overflow containers
  // so we shouldn't have a next-in-flow ever.
  aStatus.Reset();
  MOZ_ASSERT(!GetNextInFlow());
}

void nsHTMLButtonControlFrame::ReflowButtonContents(
    nsPresContext* aPresContext, ReflowOutput& aButtonDesiredSize,
    const ReflowInput& aButtonReflowInput, nsIFrame* aFirstKid) {
  WritingMode wm = GetWritingMode();
  LogicalSize availSize = aButtonReflowInput.ComputedSize(wm);
  availSize.BSize(wm) = NS_UNCONSTRAINEDSIZE;

  // shorthand for a value we need to use in a bunch of places
  const LogicalMargin& clbp =
      aButtonReflowInput.ComputedLogicalBorderPadding(wm);

  LogicalPoint childPos(wm);
  childPos.I(wm) = clbp.IStart(wm);
  availSize.ISize(wm) = std::max(availSize.ISize(wm), 0);

  ReflowInput contentsReflowInput(aPresContext, aButtonReflowInput, aFirstKid,
                                  availSize);

  nsReflowStatus contentsReflowStatus;
  ReflowOutput contentsDesiredSize(aButtonReflowInput);
  childPos.B(wm) = 0;  // This will be set properly later, after reflowing the
                       // child to determine its size.

  if (aFirstKid->IsFlexOrGridContainer()) {
    // XXX: Should we use ResetResizeFlags::Yes?
    contentsReflowInput.SetComputedBSize(aButtonReflowInput.ComputedBSize(),
                                         ReflowInput::ResetResizeFlags::No);
    contentsReflowInput.SetComputedMinBSize(
        aButtonReflowInput.ComputedMinBSize());
    contentsReflowInput.SetComputedMaxBSize(
        aButtonReflowInput.ComputedMaxBSize());
  }

  // We just pass a dummy containerSize here, as the child will be
  // repositioned later by FinishReflowChild.
  nsSize dummyContainerSize;
  ReflowChild(aFirstKid, aPresContext, contentsDesiredSize, contentsReflowInput,
              wm, childPos, dummyContainerSize, ReflowChildFlags::Default,
              contentsReflowStatus);
  MOZ_ASSERT(contentsReflowStatus.IsComplete(),
             "We gave button-contents frame unconstrained available height, "
             "so it should be complete");

  // Compute the button's content-box size:
  LogicalSize buttonContentBox(wm);
  if (aButtonReflowInput.ComputedBSize() != NS_UNCONSTRAINEDSIZE) {
    // Button has a fixed block-size -- that's its content-box bSize.
    buttonContentBox.BSize(wm) = aButtonReflowInput.ComputedBSize();
  } else {
    // Button is intrinsically sized -- it should shrinkwrap the contents'
    // bSize.
    // If we have size containment in block axis, ignore the contents and use
    // contain-intrinsic-block-size. The combobox content size with no content
    // is one line-height, not zero.
    const Maybe<nscoord> containBSize = ContainIntrinsicBSize(
        IsComboboxControlFrame() ? aButtonReflowInput.GetLineHeight() : 0);
    const nscoord bSize = containBSize.valueOr(contentsDesiredSize.BSize(wm));
    // Make sure we obey min/max-bSize in the case when we're doing intrinsic
    // sizing (we get it for free when we have a non-intrinsic
    // aButtonReflowInput.ComputedBSize()).  Note that we do this before
    // adjusting for borderpadding, since mComputedMaxBSize and
    // mComputedMinBSize are content bSizes.
    buttonContentBox.BSize(wm) = aButtonReflowInput.ApplyMinMaxBSize(bSize);
  }
  if (aButtonReflowInput.ComputedISize() != NS_UNCONSTRAINEDSIZE) {
    buttonContentBox.ISize(wm) = aButtonReflowInput.ComputedISize();
  } else {
    nscoord iSize = aButtonReflowInput.mFrame->ContainIntrinsicISize().valueOr(
        contentsDesiredSize.ISize(wm));
    buttonContentBox.ISize(wm) = aButtonReflowInput.ApplyMinMaxISize(iSize);
  }

  // Center child in the block-direction in the button
  // (technically, inside of the button's focus-padding area)
  nscoord extraSpace =
      buttonContentBox.BSize(wm) - contentsDesiredSize.BSize(wm);

  childPos.B(wm) = std::max(0, extraSpace / 2);

  // Adjust childPos.B() to be in terms of the button's frame-rect:
  childPos.B(wm) += clbp.BStart(wm);

  nsSize containerSize = (buttonContentBox + clbp.Size(wm)).GetPhysicalSize(wm);

  // Place the child
  FinishReflowChild(aFirstKid, aPresContext, contentsDesiredSize,
                    &contentsReflowInput, wm, childPos, containerSize,
                    ReflowChildFlags::Default);

  // Make sure we have a useful 'ascent' value for the child
  if (contentsDesiredSize.BlockStartAscent() ==
      ReflowOutput::ASK_FOR_BASELINE) {
    WritingMode wm = aButtonReflowInput.GetWritingMode();
    contentsDesiredSize.SetBlockStartAscent(aFirstKid->GetLogicalBaseline(wm));
  }

  // OK, we're done with the child frame.
  // Use what we learned to populate the button frame's reflow metrics.
  //  * Button's height & width are content-box size + border-box contribution:
  aButtonDesiredSize.SetSize(
      wm,
      LogicalSize(wm, aButtonReflowInput.ComputedISize() + clbp.IStartEnd(wm),
                  buttonContentBox.BSize(wm) + clbp.BStartEnd(wm)));

  //  * Button's ascent is its child's ascent, plus the child's block-offset
  // within our frame... unless it's orthogonal, in which case we'll use the
  // contents inline-size as an approximation for now.
  // XXX is there a better strategy? should we include border-padding?
  if (!aButtonReflowInput.mStyleDisplay->IsContainLayout()) {
    if (aButtonDesiredSize.GetWritingMode().IsOrthogonalTo(wm)) {
      aButtonDesiredSize.SetBlockStartAscent(
          wm.IsAlphabeticalBaseline() ? contentsDesiredSize.ISize(wm)
                                      : contentsDesiredSize.ISize(wm) / 2);
    } else {
      aButtonDesiredSize.SetBlockStartAscent(
          contentsDesiredSize.BlockStartAscent() + childPos.B(wm));
    }
  }  // else: we're layout-contained, and so we have no baseline.

  aButtonDesiredSize.SetOverflowAreasToDesiredBounds();
}

Maybe<nscoord> nsHTMLButtonControlFrame::GetNaturalBaselineBOffset(
    WritingMode aWM, BaselineSharingGroup aBaselineGroup,
    BaselineExportContext aExportContext) const {
  if (StyleDisplay()->IsContainLayout()) {
    return Nothing{};
  }

  nsIFrame* inner = mFrames.FirstChild();
  if (MOZ_UNLIKELY(inner->GetWritingMode().IsOrthogonalTo(aWM))) {
    return Nothing{};
  }
  auto result =
      inner->GetNaturalBaselineBOffset(aWM, aBaselineGroup, aExportContext)
          .valueOrFrom([inner, aWM, aBaselineGroup]() {
            return Baseline::SynthesizeBOffsetFromBorderBox(inner, aWM,
                                                            aBaselineGroup);
          });

  nscoord innerBStart = inner->BStart(aWM, GetSize());
  if (aBaselineGroup == BaselineSharingGroup::First) {
    return Some(result + innerBStart);
  }
  return Some(result + BSize(aWM) - (innerBStart + inner->BSize(aWM)));
}

BaselineSharingGroup nsHTMLButtonControlFrame::GetDefaultBaselineSharingGroup()
    const {
  nsIFrame* firstKid = mFrames.FirstChild();

  MOZ_ASSERT(firstKid, "Button should have a child frame for its contents");
  MOZ_ASSERT(!firstKid->GetNextSibling(),
             "Button should have exactly one child frame");
  return firstKid->GetDefaultBaselineSharingGroup();
}

nscoord nsHTMLButtonControlFrame::SynthesizeFallbackBaseline(
    mozilla::WritingMode aWM, BaselineSharingGroup aBaselineGroup) const {
  return Baseline::SynthesizeBOffsetFromMarginBox(this, aWM, aBaselineGroup);
}

void nsHTMLButtonControlFrame::AppendDirectlyOwnedAnonBoxes(
    nsTArray<OwnedAnonBox>& aResult) {
  MOZ_ASSERT(mFrames.FirstChild(), "Must have our button-content anon box");
  MOZ_ASSERT(!mFrames.FirstChild()->GetNextSibling(),
             "Must only have our button-content anon box");
  aResult.AppendElement(OwnedAnonBox(mFrames.FirstChild()));
}

#ifdef DEBUG
void nsHTMLButtonControlFrame::AppendFrames(ChildListID aListID,
                                            nsFrameList&& aFrameList) {
  MOZ_CRASH("unsupported operation");
}

void nsHTMLButtonControlFrame::InsertFrames(
    ChildListID aListID, nsIFrame* aPrevFrame,
    const nsLineList::iterator* aPrevFrameLine, nsFrameList&& aFrameList) {
  MOZ_CRASH("unsupported operation");
}

void nsHTMLButtonControlFrame::RemoveFrame(DestroyContext&, ChildListID,
                                           nsIFrame*) {
  MOZ_CRASH("unsupported operation");
}
#endif
