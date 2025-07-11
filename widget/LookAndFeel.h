/* -*- Mode: C++; tab-width: 2; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef __LookAndFeel
#define __LookAndFeel

#ifndef MOZILLA_INTERNAL_API
#  error "This header is only usable from within libxul (MOZILLA_INTERNAL_API)."
#endif

#include "nsDebug.h"
#include "nsColor.h"
#include "nsString.h"
#include "nsTArray.h"
#include "mozilla/Maybe.h"
#include "mozilla/widget/ThemeChangeKind.h"
#include "mozilla/ColorScheme.h"

struct gfxFontStyle;

class nsIFrame;

namespace mozilla {

using Modifiers = uint16_t;
struct StyleColorSchemeFlags;

namespace dom {
class Document;
}

namespace widget {
class FullLookAndFeel;
class LookAndFeelFont;
}  // namespace widget

enum class StyleSystemColor : uint8_t;
enum class StyleSystemColorScheme : uint8_t;
enum class StyleSystemFont : uint8_t;

class LookAndFeel {
 public:
  using ColorID = StyleSystemColor;
  using ColorScheme = mozilla::ColorScheme;

  // When modifying this list, also modify nsXPLookAndFeel::sIntPrefs
  // in widget/xpwidgts/nsXPLookAndFeel.cpp.
  enum class IntID {
    // default, may be overriden by OS
    CaretBlinkTime,
    // Amount of blinks that happen before the caret stops blinking.
    CaretBlinkCount,
    // pixel width of caret
    CaretWidth,
    // select textfields when focused via tab/accesskey?
    SelectTextfieldsOnKeyFocus,
    // delay before submenus open
    SubmenuDelay,
    // can popups overlap menu/task bar?
    MenusCanOverlapOSBar,
    // should overlay scrollbars be used?
    UseOverlayScrollbars,
    // allow H and V overlay scrollbars to overlap?
    AllowOverlayScrollbarsOverlap,
    // skip navigating to disabled menu item?
    SkipNavigatingDisabledMenuItem,
    // begin a drag if the mouse is moved further than the threshold while the
    // button is down
    DragThresholdX,
    DragThresholdY,
    // Accessibility theme being used?
    UseAccessibilityTheme,

    // position of scroll arrows in a scrollbar
    ScrollArrowStyle,

    // each button can take one of four values:
    ScrollButtonLeftMouseButtonAction,
    // 0 - scrolls one  line, 1 - scrolls one page
    ScrollButtonMiddleMouseButtonAction,
    // 2 - scrolls to end, 3 - button ignored
    ScrollButtonRightMouseButtonAction,

    // delay for opening spring loaded folders
    TreeOpenDelay,
    // delay for closing spring loaded folders
    TreeCloseDelay,
    // delay for triggering the tree scrolling
    TreeLazyScrollDelay,
    // delay for scrolling the tree
    TreeScrollDelay,
    // the maximum number of lines to be scrolled at ones
    TreeScrollLinesMax,
    // Should menu items blink when they're chosen?
    ChosenMenuItemsShouldBlink,

    /*
     * A Boolean value to determine whether the Windows accent color
     * should be applied to the title bar.
     *
     * The value of this metric is not used on other platforms. These platforms
     * should return NS_ERROR_NOT_IMPLEMENTED when queried for this metric.
     */
    WindowsAccentColorInTitlebar,

    /* Whether Windows mica effect is enabled and available */
    WindowsMica,

    /* Whether Windows mica effect is enabled and available on popups */
    WindowsMicaPopups,

    /*
     * A Boolean value to determine whether the macOS Big Sur-specific
     * theming should be used.
     */
    MacBigSurTheme,

    /*
     * A Boolean value to determine whether macOS is in RTL mode or not.
     */
    MacRTL,

    /* Native macOS titlebar height. */
    MacTitlebarHeight,

    /*
     * AlertNotificationOrigin indicates from which corner of the
     * screen alerts slide in, and from which direction (horizontal/vertical).
     * 0, the default, represents bottom right, sliding vertically.
     * Use any bitwise combination of the following constants:
     * NS_ALERT_HORIZONTAL (1), NS_ALERT_LEFT (2), NS_ALERT_TOP (4).
     *
     *       6       4
     *     +-----------+
     *    7|           |5
     *     |           |
     *    3|           |1
     *     +-----------+
     *       2       0
     */
    AlertNotificationOrigin,

    /**
     * If true, clicking on a scrollbar (not as in dragging the thumb) defaults
     * to scrolling the view corresponding to the clicked point. Otherwise, we
     * only do so if the scrollbar is clicked using the middle mouse button or
     * if shift is pressed when the scrollbar is clicked.
     */
    ScrollToClick,

    /**
     * IME and spell checker underline styles, the values should be
     * NS_DECORATION_LINE_STYLE_*.  They are defined below.
     */
    IMERawInputUnderlineStyle,
    IMESelectedRawTextUnderlineStyle,
    IMEConvertedTextUnderlineStyle,
    IMESelectedConvertedTextUnderline,
    SpellCheckerUnderlineStyle,

    /**
     * If this metric != 0, support window dragging on the menubar.
     */
    MenuBarDrag,
    /**
     * 0: scrollbar button repeats to scroll only when cursor is on the button.
     * 1: scrollbar button repeats to scroll even if cursor is outside of it.
     */
    ScrollbarButtonAutoRepeatBehavior,
    /*
     * A Boolean value to determine whether swipe animations should be used.
     */
    SwipeAnimationEnabled,

    /*
     * Controls whether overlay scrollbars display when the user moves
     * the mouse in a scrollable frame.
     */
    ScrollbarDisplayOnMouseMove,

    /*
     * Overlay scrollbar animation constants.
     */
    ScrollbarFadeBeginDelay,
    ScrollbarFadeDuration,

    /**
     * Distance in pixels to offset the context menu from the cursor
     * on open.
     */
    ContextMenuOffsetVertical,
    ContextMenuOffsetHorizontal,
    TooltipOffsetVertical,

    /*
     * A boolean value indicating whether client-side decorations are
     * supported by the user's GTK version.
     */
    GTKCSDAvailable,

    /*
     * A boolean value indicating whether semi-transparent
     * windows are available.
     */
    GTKCSDTransparencyAvailable,

    /*
     * A boolean value indicating whether client-side decorations should
     * contain a minimize button.
     */
    GTKCSDMinimizeButton,

    /*
     * A boolean value indicating whether client-side decorations should
     * contain a maximize button.
     */
    GTKCSDMaximizeButton,

    /*
     * A boolean value indicating whether client-side decorations should
     * contain a close button.
     */
    GTKCSDCloseButton,

    /**
     * An Integer value that will represent the position of the Minimize button
     * in GTK Client side decoration header.
     */
    GTKCSDMinimizeButtonPosition,

    /**
     * An Integer value that will represent the position of the Maximize button
     * in GTK Client side decoration header.
     */
    GTKCSDMaximizeButtonPosition,

    /**
     * An Integer value that will represent the position of the Close button
     * in GTK Client side decoration header.
     */
    GTKCSDCloseButtonPosition,

    /*
     * A boolean value indicating whether titlebar buttons are located
     * in left titlebar corner.
     */
    GTKCSDReversedPlacement,

    /*
     * A boolean value indicating whether or not the OS is using a dark theme,
     * which we may want to switch to as well if not overridden by the user.
     */
    SystemUsesDarkTheme,

    /**
     * Corresponding to prefers-reduced-motion.
     * https://drafts.csswg.org/mediaqueries-5/#prefers-reduced-motion
     * 0: no-preference
     * 1: reduce
     */
    PrefersReducedMotion,

    /**
     * Corresponding to prefers-reduced-transparency.
     * https://drafts.csswg.org/mediaqueries-5/#prefers-reduced-transparency
     * 0: no-preference
     * 1: reduce
     */
    PrefersReducedTransparency,

    /**
     * Corresponding to inverted-colors.
     * https://drafts.csswg.org/mediaqueries-5/#inverted
     * 0: none
     * 1: inverted
     */
    InvertedColors,

    /**
     * Corresponding to PointerCapabilities in ServoTypes.h
     * 0: None
     * 1: Coarse
     * 2: Fine
     * 4: Hover
     */
    PrimaryPointerCapabilities,
    /**
     * Corresponding to union of PointerCapabilities values in ServoTypes.h
     * E.g. if there is a mouse and a digitizer, the value will be
     * 'Coarse | Fine | Hover'.
     */
    AllPointerCapabilities,

    /** The scrollbar size, in CSS pixels. */
    SystemScrollbarSize,

    /** A boolean value to determine whether a touch device is present */
    TouchDeviceSupportPresent,

    /** GTK titlebar radius */
    TitlebarRadius,

    /** GTK tooltip radius */
    TooltipRadius,

    /**
     * Corresponding to dynamic-range.
     * https://drafts.csswg.org/mediaqueries-5/#dynamic-range
     * 0: Standard
     * 1: High
     */
    DynamicRange,

    /** Whether XUL panel animations are enabled. */
    PanelAnimations,

    /* Whether we should hide the cursor while typing */
    HideCursorWhileTyping,

    /* The StyleGtkThemeFamily of the current GTK theme. */
    GTKThemeFamily,

    /* Whether macOS' full keyboard access is enabled */
    FullKeyboardAccess,

    // TODO(krosylight): This should ultimately be able to replace
    // IntID::AllPointerCapabilities. (Bug 1918207)
    //
    // Note that PrimaryPointerCapabilities may not be replaceable as it has a
    // bit more system specific heuristic, e.g. IsTabletMode on Windows.
    PointingDeviceKinds,

    /* Whether the menubar is native / outside the application */
    NativeMenubar,

    /*
     * Not an ID; used to define the range of valid IDs.  Must be last.
     */
    End,
  };

  // This is a common enough integer that seems worth the shortcut.
  static bool UseOverlayScrollbars() {
    return GetInt(IntID::UseOverlayScrollbars);
  }

  static constexpr int32_t kDefaultTooltipOffset = 21;
  static int32_t TooltipOffsetVertical() {
    return GetInt(IntID::TooltipOffsetVertical, kDefaultTooltipOffset);
  }

  // Returns keyCode value of a modifier key which is used for accesskey.
  // Returns 0 if the platform doesn't support access key.
  static uint32_t GetMenuAccessKey();
  // Modifier mask for the menu accesskey.
  static Modifiers GetMenuAccessKeyModifiers();

  enum {
    eScrollArrow_None = 0,
    eScrollArrow_StartBackward = 0x1000,
    eScrollArrow_StartForward = 0x0100,
    eScrollArrow_EndBackward = 0x0010,
    eScrollArrow_EndForward = 0x0001
  };

  enum {
    // single arrow at each end
    eScrollArrowStyle_Single =
        eScrollArrow_StartBackward | eScrollArrow_EndForward,
    // both arrows at bottom/right, none at top/left
    eScrollArrowStyle_BothAtBottom =
        eScrollArrow_EndBackward | eScrollArrow_EndForward,
    // both arrows at both ends
    eScrollArrowStyle_BothAtEachEnd =
        eScrollArrow_EndBackward | eScrollArrow_EndForward |
        eScrollArrow_StartBackward | eScrollArrow_StartForward,
    // both arrows at top/left, none at bottom/right
    eScrollArrowStyle_BothAtTop =
        eScrollArrow_StartBackward | eScrollArrow_StartForward
  };

  // When modifying this list, also modify nsXPLookAndFeel::sFloatPrefs
  // in widget/nsXPLookAndFeel.cpp.
  enum class FloatID {
    IMEUnderlineRelativeSize,
    SpellCheckerUnderlineRelativeSize,

    // The width/height ratio of the cursor. If used, the CaretWidth int metric
    // should be added to the calculated caret width.
    CaretAspectRatio,

    // GTK text scale factor.
    TextScaleFactor,

    // Mouse pointer scaling factor.
    CursorScale,

    // Not an ID; used to define the range of valid IDs.  Must be last.
    End,
  };

  using FontID = mozilla::StyleSystemFont;

  enum class PointingDeviceKinds : uint8_t {
    None = 0,
    Mouse = 1 << 0,
    Touch = 1 << 1,
    Pen = 1 << 2,
  };

  static ColorScheme SystemColorScheme() {
    return GetInt(IntID::SystemUsesDarkTheme) ? ColorScheme::Dark
                                              : ColorScheme::Light;
  }

  static bool IsDarkColor(nscolor);

  static ColorScheme ColorSchemeForStyle(
      const dom::Document&, const StyleColorSchemeFlags&,
      ColorSchemeMode = ColorSchemeMode::Used);
  static ColorScheme ColorSchemeForFrame(
      const nsIFrame*, ColorSchemeMode = ColorSchemeMode::Used);

  // Whether standins for native colors should be used (that is, colors faked,
  // taken from win7, mostly). This forces light appearance, effectively.
  enum class UseStandins : bool { No, Yes };
  static UseStandins ShouldUseStandins(const dom::Document&, ColorID);

  // Returns a native color value (might be overwritten by prefs) for a given
  // color id.
  //
  // NOTE:
  //   ColorID::TextSelectForeground might return NS_SAME_AS_FOREGROUND_COLOR.
  //   ColorID::IME* might return NS_TRANSPARENT, NS_SAME_AS_FOREGROUND_COLOR or
  //   NS_40PERCENT_FOREGROUND_COLOR.
  //   These values have particular meaning.  Then, they are not an actual
  //   color value.
  static Maybe<nscolor> GetColor(ColorID, ColorScheme, UseStandins);

  // Gets the color with appropriate defaults for UseStandins, ColorScheme etc
  // for a given frame.
  static Maybe<nscolor> GetColor(ColorID, const nsIFrame*);

  // Versions of the above which returns the color if found, or a default (which
  // defaults to opaque black) otherwise.
  static nscolor Color(ColorID aId, ColorScheme aScheme,
                       UseStandins aUseStandins,
                       nscolor aDefault = NS_RGB(0, 0, 0)) {
    return GetColor(aId, aScheme, aUseStandins).valueOr(aDefault);
  }

  static nscolor Color(ColorID aId, nsIFrame* aFrame,
                       nscolor aDefault = NS_RGB(0, 0, 0)) {
    return GetColor(aId, aFrame).valueOr(aDefault);
  }

  static float GetTextScaleFactor() {
    float f = GetFloat(FloatID::TextScaleFactor, 1.0f);
    if (MOZ_UNLIKELY(f <= 0.0f)) {
      return 1.0f;
    }
    return f;
  }

  struct ZoomSettings {
    float mFullZoom = 1.0f;
    float mTextZoom = 1.0f;
  };

  static ZoomSettings SystemZoomSettings();

  /**
   * GetInt() and GetFloat() return a int or float value for aID.  The result
   * might be distance, time, some flags or a int value which has particular
   * meaning.  See each document at definition of each ID for the detail.
   * The result is always 0 when they return error.  Therefore, if you want to
   * use a value for the default value, you should use the other method which
   * returns int or float directly.
   */
  static nsresult GetInt(IntID, int32_t* aResult);
  static nsresult GetFloat(FloatID aID, float* aResult);

  static int32_t GetInt(IntID aID, int32_t aDefault = 0) {
    int32_t result;
    if (NS_FAILED(GetInt(aID, &result))) {
      return aDefault;
    }
    return result;
  }

  static float GetFloat(FloatID aID, float aDefault = 0.0f) {
    float result;
    if (NS_FAILED(GetFloat(aID, &result))) {
      return aDefault;
    }
    return result;
  }

  /**
   * Retrieve the name and style of a system-theme font.  Returns true
   * if the system theme specifies this font, false if a default should
   * be used.  In the latter case neither aName nor aStyle is modified.
   *
   * Size of the font should be in CSS pixels, not device pixels.
   *
   * @param aID    Which system-theme font is wanted.
   * @param aName  The name of the font to use.
   * @param aStyle Styling to apply to the font.
   */
  static bool GetFont(FontID aID, nsString& aName, gfxFontStyle& aStyle);
  static void GetFont(FontID, widget::LookAndFeelFont&);

  /**
   * GetPasswordCharacter() returns a unicode character which should be used
   * for a masked character in password editor.  E.g., '*'.
   */
  static char16_t GetPasswordCharacter();

  /**
   * If the latest character in password field shouldn't be hidden by the
   * result of GetPasswordCharacter(), GetEchoPassword() returns TRUE.
   * Otherwise, FALSE.
   */
  static bool GetEchoPassword();

  /** Whether we should be drawing in the titlebar by default. */
  static bool DrawInTitlebar();

  static int32_t CaretBlinkCount() {
    return GetInt(IntID::CaretBlinkCount, -1);
  }

  static int32_t CaretBlinkTime() { return GetInt(IntID::CaretBlinkTime, 500); }

  enum class TitlebarAction {
    None,
    WindowLower,
    WindowMenu,
    WindowMinimize,
    WindowMaximize,
    WindowMaximizeToggle,
    // We don't support more actions (maximize-horizontal, maximize-vertical,..)
    // as they're implemented as part of Wayland gtk_surface1 protocol
    // which is not accessible to us.
  };

  enum class TitlebarEvent {
    Double_Click,
    Middle_Click,
  };

  /**
   * Get system defined action for titlebar events.
   */
  static TitlebarAction GetTitlebarAction(TitlebarEvent aEvent);

  /**
   * The millisecond to mask password value.
   * This value is only valid when GetEchoPassword() returns true.
   */
  static uint32_t GetPasswordMaskDelay();

  /** Gets theme information for about:support */
  static void GetThemeInfo(nsACString&);

  /**
   * When system look and feel is changed, Refresh() must be called.  Then,
   * cached data would be released.
   */
  static void Refresh();

  /**
   * LookAndFeel initialization must be done on the main thread. If you need
   * LookAndFeel to be initialized OMT then you need to call this first.
   */
  static void EnsureInit();

  static void SetData(widget::FullLookAndFeel&& aTables);
  static void NotifyChangedAllWindows(widget::ThemeChangeKind);
  static bool HasPendingGlobalThemeChange() { return sGlobalThemeChanged; }
  static void HandleGlobalThemeChange() {
    if (MOZ_UNLIKELY(HasPendingGlobalThemeChange())) {
      DoHandleGlobalThemeChange();
    }
  }

  static nsresult GetKeyboardLayout(nsACString& aLayout);

 protected:
  static void DoHandleGlobalThemeChange();
  // Set to true when ThemeChanged needs to be called on mTheme (and other
  // global LookAndFeel.  This is used because mTheme is a service, so there's
  // no need to notify it from more than one prescontext.
  static bool sGlobalThemeChanged;
};

MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(LookAndFeel::PointingDeviceKinds);

}  // namespace mozilla

// ---------------------------------------------------------------------
//  Special colors for ColorID::IME* and ColorID::SpellCheckerUnderline
// ---------------------------------------------------------------------

// For background color only.
constexpr nscolor NS_TRANSPARENT = NS_RGBA(0x01, 0x00, 0x00, 0x00);
// For foreground color only.
constexpr nscolor NS_SAME_AS_FOREGROUND_COLOR = NS_RGBA(0x02, 0x00, 0x00, 0x00);
constexpr nscolor NS_40PERCENT_FOREGROUND_COLOR =
    NS_RGBA(0x03, 0x00, 0x00, 0x00);

#define NS_IS_SELECTION_SPECIAL_COLOR(c)                          \
  ((c) == NS_TRANSPARENT || (c) == NS_SAME_AS_FOREGROUND_COLOR || \
   (c) == NS_40PERCENT_FOREGROUND_COLOR)

// ------------------------------------------
//  Bits for IntID::AlertNotificationOrigin
// ------------------------------------------

#define NS_ALERT_HORIZONTAL 1
#define NS_ALERT_LEFT 2
#define NS_ALERT_TOP 4

#endif /* __LookAndFeel */
