/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

import React, { Component } from "devtools/client/shared/vendor/react";
import { div, button } from "devtools/client/shared/vendor/react-dom-factories";
import PropTypes from "devtools/client/shared/vendor/react-prop-types";

import { connect } from "devtools/client/shared/vendor/react-redux";
import { features, prefs } from "../../utils/prefs";
import {
  getIsWaitingOnBreak,
  getSkipPausing,
  getCurrentThread,
  isTopFrameSelected,
  getIsCurrentThreadPaused,
} from "../../selectors/index";
import { formatKeyShortcut } from "../../utils/text";
import actions from "../../actions/index";
import { debugBtn } from "../shared/Button/CommandBarButton";
import AccessibleImage from "../shared/AccessibleImage";

const classnames = require("resource://devtools/client/shared/classnames.js");
const MenuButton = require("resource://devtools/client/shared/components/menu/MenuButton.js");
const MenuItem = require("resource://devtools/client/shared/components/menu/MenuItem.js");
const MenuList = require("resource://devtools/client/shared/components/menu/MenuList.js");

const isMacOS = Services.appinfo.OS === "Darwin";

// NOTE: the "resume" command will call either the resume or breakOnNext action
// depending on whether or not the debugger is paused or running
const COMMANDS = ["resume", "stepOver", "stepIn", "stepOut"];

const KEYS = {
  WINNT: {
    resume: "F8",
    stepOver: "F10",
    stepIn: "F11",
    stepOut: "Shift+F11",
    trace: "Ctrl+Shift+5",
  },
  Darwin: {
    resume: "Cmd+\\",
    stepOver: "Cmd+'",
    stepIn: "Cmd+;",
    stepOut: "Cmd+Shift+:",
    stepOutDisplay: "Cmd+Shift+;",
    trace: "Ctrl+Shift+5",
  },
  Linux: {
    resume: "F8",
    stepOver: "F10",
    stepIn: "F11",
    stepOut: "Shift+F11",
    trace: "Ctrl+Shift+5",
  },
};

function getKey(action) {
  return getKeyForOS(Services.appinfo.OS, action);
}

function getKeyForOS(os, action) {
  const osActions = KEYS[os] || KEYS.Linux;
  return osActions[action];
}

function formatKey(action) {
  const key = getKey(`${action}Display`) || getKey(action);

  // On MacOS, we bind both Windows and MacOS/Darwin key shortcuts
  // Display them both, but only when they are different
  if (isMacOS) {
    const winKey =
      getKeyForOS("WINNT", `${action}Display`) || getKeyForOS("WINNT", action);
    if (key != winKey) {
      return formatKeyShortcut([key, winKey].join(" "));
    }
  }
  return formatKeyShortcut(key);
}

class CommandBar extends Component {
  constructor() {
    super();

    this.state = {};
  }
  static get propTypes() {
    return {
      breakOnNext: PropTypes.func.isRequired,
      horizontal: PropTypes.bool.isRequired,
      isPaused: PropTypes.bool.isRequired,
      isWaitingOnBreak: PropTypes.bool.isRequired,
      javascriptEnabled: PropTypes.bool.isRequired,
      resume: PropTypes.func.isRequired,
      skipPausing: PropTypes.bool.isRequired,
      stepIn: PropTypes.func.isRequired,
      stepOut: PropTypes.func.isRequired,
      stepOver: PropTypes.func.isRequired,
      toggleEditorWrapping: PropTypes.func.isRequired,
      toggleInlinePreview: PropTypes.func.isRequired,
      toggleJavaScriptEnabled: PropTypes.func.isRequired,
      toggleSkipPausing: PropTypes.any.isRequired,
      toggleSourceMapsEnabled: PropTypes.func.isRequired,
      topFrameSelected: PropTypes.bool.isRequired,
      setHideOrShowIgnoredSources: PropTypes.func.isRequired,
      toggleSourceMapIgnoreList: PropTypes.func.isRequired,
      togglePausedOverlay: PropTypes.func.isRequired,
    };
  }

  componentWillUnmount() {
    const { shortcuts } = this.context;

    COMMANDS.forEach(action => shortcuts.off(getKey(action)));

    if (isMacOS) {
      COMMANDS.forEach(action => shortcuts.off(getKeyForOS("WINNT", action)));
    }
  }

  componentDidMount() {
    const { shortcuts } = this.context;

    COMMANDS.forEach(action =>
      shortcuts.on(getKey(action), e => this.handleEvent(e, action))
    );

    if (isMacOS) {
      // The Mac supports both the Windows Function keys
      // as well as the Mac non-Function keys
      COMMANDS.forEach(action =>
        shortcuts.on(getKeyForOS("WINNT", action), e =>
          this.handleEvent(e, action)
        )
      );
    }
  }

  handleEvent(e, action) {
    e.preventDefault();
    e.stopPropagation();
    if (action === "resume") {
      this.props.isPaused ? this.props.resume() : this.props.breakOnNext();
    } else {
      this.props[action]();
    }
  }

  renderStepButtons() {
    const { isPaused, topFrameSelected } = this.props;
    const className = isPaused ? "active" : "disabled";
    const isDisabled = !isPaused;

    return [
      this.renderPauseButton(),
      debugBtn(
        () => this.props.stepOver(),
        "stepOver",
        className,
        L10N.getFormatStr("stepOverTooltip", formatKey("stepOver")),
        isDisabled
      ),
      debugBtn(
        () => this.props.stepIn(),
        "stepIn",
        className,
        L10N.getFormatStr("stepInTooltip", formatKey("stepIn")),
        isDisabled || !topFrameSelected
      ),
      debugBtn(
        () => this.props.stepOut(),
        "stepOut",
        className,
        L10N.getFormatStr("stepOutTooltip", formatKey("stepOut")),
        isDisabled
      ),
    ];
  }

  resume() {
    this.props.resume();
  }

  renderPauseButton() {
    const { breakOnNext, isWaitingOnBreak } = this.props;

    if (this.props.isPaused) {
      return debugBtn(
        () => this.resume(),
        "resume",
        "active",
        L10N.getFormatStr("resumeButtonTooltip", formatKey("resume"))
      );
    }

    if (isWaitingOnBreak) {
      return debugBtn(
        null,
        "pause",
        "disabled",
        L10N.getStr("pausePendingButtonTooltip"),
        true
      );
    }

    return debugBtn(
      () => breakOnNext(),
      "pause",
      "active",
      L10N.getFormatStr("pauseButtonTooltip", formatKey("resume"))
    );
  }

  renderSkipPausingButton() {
    const { skipPausing, toggleSkipPausing } = this.props;
    return button(
      {
        className: classnames(
          "command-bar-button",
          "command-bar-skip-pausing",
          {
            active: skipPausing,
          }
        ),
        title: skipPausing
          ? L10N.getStr("undoSkipPausingTooltip.label")
          : L10N.getStr("skipPausingTooltip.label"),
        onClick: toggleSkipPausing,
      },
      React.createElement(AccessibleImage, {
        className: skipPausing ? "enable-pausing" : "disable-pausing",
      })
    );
  }

  renderSettingsButton() {
    const { toolboxDoc } = this.context;
    return React.createElement(
      MenuButton,
      {
        menuId: "debugger-settings-menu-button",
        toolboxDoc,
        className:
          "devtools-button command-bar-button debugger-settings-menu-button",
        title: L10N.getStr("settings.button.label"),
      },
      () => this.renderSettingsMenuItems()
    );
  }

  renderSettingsMenuItems() {
    return React.createElement(
      MenuList,
      {
        id: "debugger-settings-menu-list",
      },
      React.createElement(MenuItem, {
        key: "debugger-settings-menu-item-disable-javascript",
        className: "menu-item debugger-settings-menu-item-disable-javascript",
        checked: !this.props.javascriptEnabled,
        label: L10N.getStr("settings.disableJavaScript.label"),
        tooltip: L10N.getStr("settings.disableJavaScript.tooltip"),
        onClick: () => {
          this.props.toggleJavaScriptEnabled(!this.props.javascriptEnabled);
        },
      }),
      React.createElement(MenuItem, {
        key: "debugger-settings-menu-item-disable-inline-previews",
        checked: features.inlinePreview,
        label: L10N.getStr("inlinePreview.toggle.label"),
        tooltip: L10N.getStr("inlinePreview.toggle.tooltip"),
        onClick: () => this.props.toggleInlinePreview(!features.inlinePreview),
      }),
      React.createElement(MenuItem, {
        key: "debugger-settings-menu-item-disable-wrap-lines",
        checked: prefs.editorWrapping,
        label: L10N.getStr("editorWrapping.toggle.label"),
        tooltip: L10N.getStr("editorWrapping.toggle.tooltip"),
        onClick: () => this.props.toggleEditorWrapping(!prefs.editorWrapping),
      }),
      React.createElement(MenuItem, {
        key: "debugger-settings-menu-item-disable-sourcemaps",
        checked: prefs.clientSourceMapsEnabled,
        label: L10N.getStr("settings.toggleSourceMaps.label"),
        tooltip: L10N.getStr("settings.toggleSourceMaps.tooltip"),
        onClick: () =>
          this.props.toggleSourceMapsEnabled(!prefs.clientSourceMapsEnabled),
      }),
      React.createElement(MenuItem, {
        key: "debugger-settings-menu-item-hide-ignored-sources",
        className: "menu-item debugger-settings-menu-item-hide-ignored-sources",
        checked: prefs.hideIgnoredSources,
        label: L10N.getStr("settings.hideIgnoredSources.label"),
        tooltip: L10N.getStr("settings.hideIgnoredSources.tooltip"),
        onClick: () =>
          this.props.setHideOrShowIgnoredSources(!prefs.hideIgnoredSources),
      }),
      React.createElement(MenuItem, {
        key: "debugger-settings-menu-item-enable-sourcemap-ignore-list",
        className:
          "menu-item debugger-settings-menu-item-enable-sourcemap-ignore-list",
        checked: prefs.sourceMapIgnoreListEnabled,
        label: L10N.getStr("settings.enableSourceMapIgnoreList.label"),
        tooltip: L10N.getStr("settings.enableSourceMapIgnoreList.tooltip"),
        onClick: () =>
          this.props.toggleSourceMapIgnoreList(
            !prefs.sourceMapIgnoreListEnabled
          ),
      }),
      React.createElement(MenuItem, {
        key: "debugger-settings-menu-item-toggle-pause-overlay",
        className: "menu-item debugger-settings-menu-item-toggle-pause-overlay",
        checked: prefs.pausedOverlayEnabled,
        label: L10N.getStr("settings.showPausedOverlay.label"),
        tooltip: L10N.getStr("settings.showPausedOverlay.tooltip"),
        onClick: () =>
          this.props.togglePausedOverlay(!prefs.pausedOverlayEnabled),
      })
    );
  }

  render() {
    return div(
      {
        className: classnames("command-bar", {
          vertical: !this.props.horizontal,
        }),
      },
      this.renderStepButtons(),
      div({
        className: "filler",
      }),
      this.renderSkipPausingButton(),
      div({
        className: "devtools-separator",
      }),
      this.renderSettingsButton()
    );
  }
}

CommandBar.contextTypes = {
  shortcuts: PropTypes.object,
  toolboxDoc: PropTypes.object,
};

const mapStateToProps = state => ({
  isWaitingOnBreak: getIsWaitingOnBreak(state, getCurrentThread(state)),
  skipPausing: getSkipPausing(state),
  topFrameSelected: isTopFrameSelected(state, getCurrentThread(state)),
  javascriptEnabled: state.ui.javascriptEnabled,
  isPaused: getIsCurrentThreadPaused(state),
});

export default connect(mapStateToProps, {
  resume: actions.resume,
  stepIn: actions.stepIn,
  stepOut: actions.stepOut,
  stepOver: actions.stepOver,
  breakOnNext: actions.breakOnNext,
  pauseOnExceptions: actions.pauseOnExceptions,
  toggleSkipPausing: actions.toggleSkipPausing,
  toggleInlinePreview: actions.toggleInlinePreview,
  toggleEditorWrapping: actions.toggleEditorWrapping,
  toggleSourceMapsEnabled: actions.toggleSourceMapsEnabled,
  toggleJavaScriptEnabled: actions.toggleJavaScriptEnabled,
  setHideOrShowIgnoredSources: actions.setHideOrShowIgnoredSources,
  toggleSourceMapIgnoreList: actions.toggleSourceMapIgnoreList,
  togglePausedOverlay: actions.togglePausedOverlay,
})(CommandBar);
