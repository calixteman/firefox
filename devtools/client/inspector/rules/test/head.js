/* Any copyright is dedicated to the Public Domain.
 http://creativecommons.org/publicdomain/zero/1.0/ */
/* eslint no-unused-vars: [2, {"vars": "local"}] */

"use strict";

// Import the inspector's head.js first (which itself imports shared-head.js).
Services.scriptloader.loadSubScript(
  "chrome://mochitests/content/browser/devtools/client/inspector/test/head.js",
  this
);

var {
  getInplaceEditorForSpan: inplaceEditor,
} = require("resource://devtools/client/shared/inplace-editor.js");

const {
  COMPATIBILITY_TOOLTIP_MESSAGE,
} = require("resource://devtools/client/inspector/rules/constants.js");

const ROOT_TEST_DIR = getRootDirectory(gTestPath);

const STYLE_INSPECTOR_L10N = new LocalizationHelper(
  "devtools/shared/locales/styleinspector.properties"
);

/**
 * When a tooltip is closed, this ends up "commiting" the value changed within
 * the tooltip (e.g. the color in case of a colorpicker) which, in turn, ends up
 * setting the value of the corresponding css property in the rule-view.
 * Use this function to close the tooltip and make sure the test waits for the
 * ruleview-changed event.
 * @param {SwatchBasedEditorTooltip} editorTooltip
 * @param {CSSRuleView} view
 */
async function hideTooltipAndWaitForRuleViewChanged(editorTooltip, view) {
  const onModified = view.once("ruleview-changed");
  const onHidden = editorTooltip.tooltip.once("hidden");
  editorTooltip.hide();
  await onModified;
  await onHidden;
}

/**
 * Polls a given generator function waiting for it to return true.
 *
 * @param {Function} validatorFn
 *        A validator generator function that returns a boolean.
 *        This is called every few milliseconds to check if the result is true.
 *        When it is true, the promise resolves.
 * @param {String} name
 *        Optional name of the test. This is used to generate
 *        the success and failure messages.
 * @return a promise that resolves when the function returned true or rejects
 * if the timeout is reached
 */
var waitForSuccess = async function (validatorFn, desc = "untitled") {
  let i = 0;
  while (true) {
    info("Checking: " + desc);
    if (await validatorFn()) {
      ok(true, "Success: " + desc);
      break;
    }
    i++;
    if (i > 10) {
      ok(false, "Failure: " + desc);
      break;
    }
    await new Promise(r => setTimeout(r, 200));
  }
};

/**
 * Simulate a color change in a given color picker tooltip, and optionally wait
 * for a given element in the page to have its style changed as a result.
 * Note that this function assumes that the colorpicker popup is already open
 * and it won't close it after having selected the new color.
 *
 * @param {RuleView} ruleView
 *        The related rule view instance
 * @param {SwatchColorPickerTooltip} colorPicker
 * @param {Array} newRgba
 *        The new color to be set [r, g, b, a]
 * @param {Object} expectedChange
 *        Optional object that needs the following props:
 *          - {String} selector The selector to the element in the page that
 *            will have its style changed.
 *          - {String} name The style name that will be changed
 *          - {String} value The expected style value
 * The style will be checked like so: getComputedStyle(element)[name] === value
 */
var simulateColorPickerChange = async function (
  ruleView,
  colorPicker,
  newRgba,
  expectedChange
) {
  let onComputedStyleChanged;
  if (expectedChange) {
    const { selector, name, value } = expectedChange;
    onComputedStyleChanged = waitForComputedStyleProperty(
      selector,
      null,
      name,
      value
    );
  }
  const onRuleViewChanged = ruleView.once("ruleview-changed");
  info("Getting the spectrum colorpicker object");
  const spectrum = colorPicker.spectrum;
  info("Setting the new color");
  spectrum.rgb = newRgba;
  info("Applying the change");
  spectrum.updateUI();
  spectrum.onChange();
  info("Waiting for rule-view to update");
  await onRuleViewChanged;

  if (expectedChange) {
    info("Waiting for the style to be applied on the page");
    await onComputedStyleChanged;
  }
};

/**
 * Open the color picker popup for a given property in a given rule and
 * simulate a color change. Optionally wait for a given element in the page to
 * have its style changed as a result.
 *
 * @param {RuleView} view
 *        The related rule view instance
 * @param {Number} ruleIndex
 *        Which rule to target in the rule view
 * @param {Number} propIndex
 *        Which property to target in the rule
 * @param {Array} newRgba
 *        The new color to be set [r, g, b, a]
 * @param {Object} expectedChange
 *        Optional object that needs the following props:
 *          - {String} selector The selector to the element in the page that
 *            will have its style changed.
 *          - {String} name The style name that will be changed
 *          - {String} value The expected style value
 * The style will be checked like so: getComputedStyle(element)[name] === value
 */
var openColorPickerAndSelectColor = async function (
  view,
  ruleIndex,
  propIndex,
  newRgba,
  expectedChange
) {
  const ruleEditor = getRuleViewRuleEditor(view, ruleIndex);
  const propEditor = ruleEditor.rule.textProps[propIndex].editor;
  const swatch = propEditor.valueSpan.querySelector(".inspector-colorswatch");
  const cPicker = view.tooltips.getTooltip("colorPicker");

  info("Opening the colorpicker by clicking the color swatch");
  const onColorPickerReady = cPicker.once("ready");
  swatch.click();
  await onColorPickerReady;

  await simulateColorPickerChange(view, cPicker, newRgba, expectedChange);

  return { propEditor, swatch, cPicker };
};

/**
 * Open the cubicbezier popup for a given property in a given rule and
 * simulate a curve change. Optionally wait for a given element in the page to
 * have its style changed as a result.
 *
 * @param {RuleView} view
 *        The related rule view instance
 * @param {Number} ruleIndex
 *        Which rule to target in the rule view
 * @param {Number} propIndex
 *        Which property to target in the rule
 * @param {Array} coords
 *        The new coordinates to be used, e.g. [0.1, 2, 0.9, -1]
 * @param {Object} expectedChange
 *        Optional object that needs the following props:
 *          - {String} selector The selector to the element in the page that
 *            will have its style changed.
 *          - {String} name The style name that will be changed
 *          - {String} value The expected style value
 * The style will be checked like so: getComputedStyle(element)[name] === value
 */
var openCubicBezierAndChangeCoords = async function (
  view,
  ruleIndex,
  propIndex,
  coords,
  expectedChange
) {
  const ruleEditor = getRuleViewRuleEditor(view, ruleIndex);
  const propEditor = ruleEditor.rule.textProps[propIndex].editor;
  const swatch = propEditor.valueSpan.querySelector(".inspector-bezierswatch");
  const bezierTooltip = view.tooltips.getTooltip("cubicBezier");

  info("Opening the cubicBezier by clicking the swatch");
  const onBezierWidgetReady = bezierTooltip.once("ready");
  swatch.click();
  await onBezierWidgetReady;

  const widget = await bezierTooltip.widget;

  info("Simulating a change of curve in the widget");
  const onRuleViewChanged = view.once("ruleview-changed");
  widget.coordinates = coords;
  await onRuleViewChanged;

  if (expectedChange) {
    info("Waiting for the style to be applied on the page");
    const { selector, name, value } = expectedChange;
    await waitForComputedStyleProperty(selector, null, name, value);
  }

  return { propEditor, swatch, bezierTooltip };
};

/**
 * Simulate adding a new property in an existing rule in the rule-view.
 *
 * @param {CssRuleView} view
 *        The instance of the rule-view panel
 * @param {Number} ruleIndex
 *        The index of the rule to use.
 * @param {String} name
 *        The name for the new property
 * @param {String} value
 *        The value for the new property
 * @param {Object=} options
 * @param {String=} options.commitValueWith
 *        Which key should be used to commit the new value. VK_TAB is used by
 *        default, but tests might want to use another key to test cancelling
 *        for exemple.
 *        If set to null, no keys will be hit, so the input will still be focused
 *        at the end of this function
 * @param {Boolean=} options.blurNewProperty
 *        After the new value has been added, a new property would have been
 *        focused. This parameter is true by default, and that causes the new
 *        property to be blurred. Set to false if you don't want this.
 * @return {TextProperty} The instance of the TextProperty that was added
 */
var addProperty = async function (
  view,
  ruleIndex,
  name,
  value,
  { commitValueWith = "VK_TAB", blurNewProperty = true } = {}
) {
  info("Adding new property " + name + ":" + value + " to rule " + ruleIndex);

  const ruleEditor = getRuleViewRuleEditor(view, ruleIndex);
  let editor = await focusNewRuleViewProperty(ruleEditor);
  const numOfProps = ruleEditor.rule.textProps.length;

  const onMutations = new Promise(r => {
    // If the rule index is 0, then we are updating the rule for the "element"
    // selector in the rule view.
    // This rule is actually updating the style attribute of the element, and
    // therefore we can expect mutations.
    // For any other rule index, no mutation should be created, we can resolve
    // immediately.
    if (ruleIndex !== 0) {
      r();
    }

    // Use CSS.escape for the name in order to match the logic at
    // devtools/client/fronts/inspector/rule-rewriter.js
    // This leads to odd values in the style attribute and might change in the
    // future. See https://bugzilla.mozilla.org/show_bug.cgi?id=1765943
    const expectedAttributeValue = `${CSS.escape(name)}: ${value}`;
    view.inspector.walker.on(
      "mutations",
      function onWalkerMutations(mutations) {
        // Wait until we receive a mutation which updates the style attribute
        // with the expected value.
        const receivedLastMutation = mutations.some(
          mut =>
            mut.attributeName === "style" &&
            mut.newValue.includes(expectedAttributeValue)
        );
        if (receivedLastMutation) {
          view.inspector.walker.off("mutations", onWalkerMutations);
          r();
        }
      }
    );
  });

  info("Adding name " + name);
  editor.input.value = name;
  is(
    editor.input.getAttribute("aria-label"),
    "New property name",
    "New property name input has expected aria-label"
  );

  const onNameAdded = view.once("ruleview-changed");
  EventUtils.synthesizeKey("VK_TAB", {}, view.styleWindow);
  await onNameAdded;

  // Focus has moved to the value inplace-editor automatically.
  editor = inplaceEditor(view.styleDocument.activeElement);
  const textProps = ruleEditor.rule.textProps;
  const textProp = textProps[textProps.length - 1];

  is(
    ruleEditor.rule.textProps.length,
    numOfProps + 1,
    "A new test property was added"
  );
  is(
    editor,
    inplaceEditor(textProp.editor.valueSpan),
    "The inplace editor appeared for the value"
  );

  info("Adding value " + value);
  // Setting the input value schedules a preview to be shown in 10ms which
  // triggers a ruleview-changed event (see bug 1209295).
  const onPreview = view.once("ruleview-changed");
  editor.input.value = value;

  ok(
    !!editor.input.getAttribute("aria-labelledby"),
    "The value input has an aria-labelledby attribute…"
  );
  is(
    editor.input.getAttribute("aria-labelledby"),
    textProp.editor.nameSpan.id,
    "…which references the property name input"
  );

  view.debounce.flush();
  await onPreview;

  if (commitValueWith === null) {
    return textProp;
  }

  const onRuleViewChanged = view.once("ruleview-changed");
  EventUtils.synthesizeKey(commitValueWith, {}, view.styleWindow);
  await onRuleViewChanged;

  info(
    "Waiting for DOM mutations in case the property was added to the element style"
  );
  await onMutations;

  if (blurNewProperty) {
    view.styleDocument.activeElement.blur();
  }

  return textProp;
};

/**
 * Change the name of a property in a rule in the rule-view.
 *
 * @param {CssRuleView} view
 *        The instance of the rule-view panel.
 * @param {TextProperty} textProp
 *        The instance of the TextProperty to be changed.
 * @param {String} name
 *        The new property name.
 */
var renameProperty = async function (view, textProp, name) {
  await focusEditableField(view, textProp.editor.nameSpan);

  const onNameDone = view.once("ruleview-changed");
  info(`Rename the property to ${name}`);
  EventUtils.sendString(name, view.styleWindow);
  EventUtils.synthesizeKey("VK_RETURN", {}, view.styleWindow);
  info("Wait for property name.");
  await onNameDone;

  if (
    !Services.prefs.getBoolPref("devtools.inspector.rule-view.focusNextOnEnter")
  ) {
    return;
  }

  // Renaming the property auto-advances the focus to the value input. Exiting without
  // committing will still fire a change event. @see TextPropertyEditor._onValueDone().
  // Wait for that event too before proceeding.
  const onValueDone = view.once("ruleview-changed");
  EventUtils.synthesizeKey("VK_ESCAPE", {}, view.styleWindow);
  info("Wait for property value.");
  await onValueDone;
};

/**
 * Simulate removing a property from an existing rule in the rule-view.
 *
 * @param {CssRuleView} view
 *        The instance of the rule-view panel
 * @param {TextProperty} textProp
 *        The instance of the TextProperty to be removed
 * @param {Boolean} blurNewProperty
 *        After the property has been removed, a new property would have been
 *        focused. This parameter is true by default, and that causes the new
 *        property to be blurred. Set to false if you don't want this.
 */
var removeProperty = async function (view, textProp, blurNewProperty = true) {
  await focusEditableField(view, textProp.editor.nameSpan);

  const onModifications = view.once("ruleview-changed");
  info("Deleting the property name now");
  EventUtils.synthesizeKey("VK_DELETE", {}, view.styleWindow);
  EventUtils.synthesizeKey("VK_TAB", {}, view.styleWindow);
  await onModifications;

  if (blurNewProperty) {
    view.styleDocument.activeElement.blur();
  }
};

/**
 * Simulate clicking the enable/disable checkbox next to a property in a rule.
 *
 * @param {CssRuleView} view
 *        The instance of the rule-view panel
 * @param {TextProperty} textProp
 *        The instance of the TextProperty to be enabled/disabled
 */
var togglePropStatus = async function (view, textProp) {
  const onRuleViewRefreshed = view.once("ruleview-changed");
  textProp.editor.enable.click();
  await onRuleViewRefreshed;
};

/**
 * Create a new rule by clicking on the "add rule" button.
 * This will leave the selector inplace-editor active.
 *
 * @param {InspectorPanel} inspector
 *        The instance of InspectorPanel currently loaded in the toolbox
 * @param {CssRuleView} view
 *        The instance of the rule-view panel
 * @returns {Rule} a promise that resolves the new model Rule after the rule has
 *          been added
 */
async function addNewRule(inspector, view) {
  const onNewRuleAdded = view.once("new-rule-added");
  info("Adding the new rule using the button");
  view.addRuleButton.click();

  info("Waiting for new-rule-added event…");
  const rule = await onNewRuleAdded;
  info("…received new-rule-added");

  return rule;
}

/**
 * Create a new rule by clicking on the "add rule" button, dismiss the editor field and
 * verify that the selector is correct.
 *
 * @param {InspectorPanel} inspector
 *        The instance of InspectorPanel currently loaded in the toolbox
 * @param {CssRuleView} view
 *        The instance of the rule-view panel
 * @param {String} expectedSelector
 *        The value we expect the selector to have
 * @param {Number} expectedIndex
 *        The index we expect the rule to have in the rule-view
 * @returns {Rule} a promise that resolves the new model Rule after the rule has
 *          been added
 */
async function addNewRuleAndDismissEditor(
  inspector,
  view,
  expectedSelector,
  expectedIndex
) {
  const rule = await addNewRule(inspector, view);

  info("Getting the new rule at index " + expectedIndex);
  const ruleEditor = getRuleViewRuleEditor(view, expectedIndex);
  const editor = ruleEditor.selectorText.ownerDocument.activeElement;
  is(
    editor.value,
    expectedSelector,
    "The editor for the new selector has the correct value: " + expectedSelector
  );

  info("Pressing escape to leave the editor");
  EventUtils.synthesizeKey("KEY_Escape");

  is(
    ruleEditor.selectorText.textContent,
    expectedSelector,
    "The new selector has the correct text: " + expectedSelector
  );

  return rule;
}

/**
 * Simulate a sequence of non-character keys (return, escape, tab) and wait for
 * a given element to receive the focus.
 *
 * @param {CssRuleView} view
 *        The instance of the rule-view panel
 * @param {DOMNode} element
 *        The element that should be focused
 * @param {Array} keys
 *        Array of non-character keys, the part that comes after "DOM_VK_" eg.
 *        "RETURN", "ESCAPE"
 * @return a promise that resolves after the element received the focus
 */
async function sendKeysAndWaitForFocus(view, element, keys) {
  const onFocus = once(element, "focus", true);
  for (const key of keys) {
    EventUtils.sendKey(key, view.styleWindow);
  }
  await onFocus;
}

/**
 * Wait for a markupmutation event on the inspector that is for a style modification.
 * @param {InspectorPanel} inspector
 * @return {Promise}
 */
function waitForStyleModification(inspector) {
  return new Promise(function (resolve) {
    function checkForStyleModification(mutations) {
      for (const mutation of mutations) {
        if (
          mutation.type === "attributes" &&
          mutation.attributeName === "style"
        ) {
          inspector.off("markupmutation", checkForStyleModification);
          resolve();
          return;
        }
      }
    }
    inspector.on("markupmutation", checkForStyleModification);
  });
}

/**
 * Click on the icon next to the selector of a CSS rule in the Rules view
 * to toggle the selector highlighter. If a selector highlighter is not already visible
 * for the given selector, wait for it to be shown. Otherwise, wait for it to be hidden.
 *
 * @param {CssRuleView} view
 *        The instance of the Rules view
 * @param {String} selectorText
 *        The selector of the CSS rule to look for
 * @param {Number} index
 *        If there are more CSS rules with the same selector, use this index
 *        to determine which one should be retrieved. Defaults to 0 (first)
 */
async function clickSelectorIcon(view, selectorText, index = 0) {
  const { inspector } = view;
  const rule = getRuleViewRule(view, selectorText, index);

  info(`Waiting for icon to be available for selector: ${selectorText}`);
  const icon = await waitFor(() => {
    return rule.querySelector(".js-toggle-selector-highlighter");
  });

  // Grab the actual selector associated with the matched icon.
  // For inline styles, the CSS rule with the "element" selector actually points to
  // a generated unique selector, for example: "div:nth-child(1)".
  // The selector highlighter is invoked with this unique selector.
  // Continuing to use selectorText ("element") would fail some of the checks below.
  const selector = icon.dataset.computedSelector;

  const { waitForHighlighterTypeShown, waitForHighlighterTypeHidden } =
    getHighlighterTestHelpers(inspector);

  // If there is an active selector highlighter, get its configuration options.
  // Will be undefined if there isn't an active selector highlighter.
  const options = inspector.highlighters.getOptionsForActiveHighlighter(
    inspector.highlighters.TYPES.SELECTOR
  );

  // If there is already a highlighter visible for this selector,
  // wait for hidden event. Otherwise, wait for shown event.
  const waitForEvent =
    options?.selector === selector
      ? waitForHighlighterTypeHidden(inspector.highlighters.TYPES.SELECTOR)
      : waitForHighlighterTypeShown(inspector.highlighters.TYPES.SELECTOR);

  // Boolean flag whether we waited for a highlighter shown event
  const waitedForShown = options?.selector !== selector;

  info(`Click the icon for selector: ${selectorText}`);
  icon.scrollIntoView();
  EventUtils.synthesizeMouseAtCenter(icon, {}, view.styleWindow);

  // Promise resolves with event data from either highlighter shown or hidden event.
  const data = await waitForEvent;
  return { ...data, isShown: waitedForShown };
}
/**
 * Toggle one of the checkboxes inside the class-panel. Resolved after the DOM mutation
 * has been recorded.
 * @param {CssRuleView} view The rule-view instance.
 * @param {String} name The class name to find the checkbox.
 */
async function toggleClassPanelCheckBox(view, name) {
  info(`Clicking on checkbox for class ${name}`);
  const checkBox = [
    ...view.classPanel.querySelectorAll("[type=checkbox]"),
  ].find(box => {
    return box.dataset.name === name;
  });

  const onMutation = view.inspector.once("markupmutation");
  checkBox.click();
  info("Waiting for a markupmutation as a result of toggling this class");
  await onMutation;
}

/**
 * Verify the content of the class-panel.
 * @param {CssRuleView} view The rule-view instance
 * @param {Array} classes The list of expected classes. Each item in this array is an
 * object with the following properties: {name: {String}, state: {Boolean}}
 */
function checkClassPanelContent(view, classes) {
  const checkBoxNodeList = view.classPanel.querySelectorAll("[type=checkbox]");
  is(
    checkBoxNodeList.length,
    classes.length,
    "The panel contains the expected number of checkboxes"
  );

  for (let i = 0; i < classes.length; i++) {
    is(
      checkBoxNodeList[i].dataset.name,
      classes[i].name,
      `Checkbox ${i} has the right class name`
    );
    is(
      checkBoxNodeList[i].checked,
      classes[i].state,
      `Checkbox ${i} has the right state`
    );
  }
}

/**
 * Opens the eyedropper from the colorpicker tooltip
 * by selecting the colorpicker and then selecting the eyedropper icon
 * @param {view} ruleView
 * @param {swatch} color swatch of a particular property
 */
async function openEyedropper(view, swatch) {
  const tooltip = view.tooltips.getTooltip("colorPicker").tooltip;

  info("Click on the swatch");
  const onColorPickerReady = view.tooltips
    .getTooltip("colorPicker")
    .once("ready");
  EventUtils.synthesizeMouseAtCenter(swatch, {}, swatch.ownerGlobal);
  await onColorPickerReady;

  const dropperButton = tooltip.container.querySelector("#eyedropper-button");

  info("Click on the eyedropper icon");
  const onOpened = tooltip.once("eyedropper-opened");
  dropperButton.click();
  await onOpened;
}

/**
 * Gets a set of declarations for a rule index.
 *
 * @param {ruleView} view
 *        The rule-view instance.
 * @param {Number} ruleIndex
 *        The index we expect the rule to have in the rule-view. If an array, the first
 *        item is the children index in the rule view, and the second item is the child
 *        node index in the retrieved rule view element. This is helpful to select rules
 *        inside the pseudo element section.
 * @param {boolean} addCompatibilityData
 *        Optional argument to add compatibility dat with the property data
 *
 * @returns A Promise that resolves with a Map containing stringified property declarations e.g.
 *          [
 *            {
 *              "color:red":
 *                {
 *                  propertyName: "color",
 *                  propertyValue: "red",
 *                  warning: "This won't work",
 *                  used: true,
 *                  compatibilityData: {
 *                    isCompatible: true,
 *                  },
 *                }
 *            },
 *            ...
 *          ]
 */
async function getPropertiesForRuleIndex(
  view,
  ruleIndex,
  addCompatibilityData = false
) {
  const declaration = new Map();
  let nodeIndex;
  if (Array.isArray(ruleIndex)) {
    [ruleIndex, nodeIndex] = ruleIndex;
  }
  const ruleEditor = getRuleViewRuleEditor(view, ruleIndex, nodeIndex);

  for (const currProp of ruleEditor?.rule?.textProps || []) {
    const icon = currProp.editor.unusedState;
    const unused = currProp.editor.element.classList.contains("unused");

    let compatibilityData;
    let compatibilityIcon;
    if (addCompatibilityData) {
      compatibilityData = await currProp.isCompatible();
      compatibilityIcon = currProp.editor.compatibilityState;
    }

    declaration.set(`${currProp.name}:${currProp.value}`, {
      propertyName: currProp.name,
      propertyValue: currProp.value,
      icon,
      data: currProp.isUsed(),
      warning: unused,
      used: !unused,
      ...(addCompatibilityData
        ? {
            compatibilityData,
            compatibilityIcon,
          }
        : {}),
    });
  }

  return declaration;
}

/**
 * Toggle a declaration disabled or enabled.
 *
 * @param {ruleView} view
 *        The rule-view instance
 * @param {Number} ruleIndex
 *        The index of the CSS rule where we can find the declaration to be
 *        toggled.
 * @param {Object} declaration
 *        An object representing the declaration e.g. { color: "red" }.
 */
async function toggleDeclaration(view, ruleIndex, declaration) {
  const textProp = getTextProperty(view, ruleIndex, declaration);
  const [[name, value]] = Object.entries(declaration);
  const dec = `${name}:${value}`;
  ok(textProp, `Declaration "${dec}" found`);

  const newStatus = textProp.enabled ? "disabled" : "enabled";
  info(`Toggling declaration "${dec}" of rule ${ruleIndex} to ${newStatus}`);

  await togglePropStatus(view, textProp);
  info("Toggled successfully.");
}

/**
 * Update a declaration from a CSS rule in the Rules view
 * by changing its property name, property value or both.
 *
 * @param {RuleView} view
 *        Instance of RuleView.
 * @param {Number} ruleIndex
 *        The index of the CSS rule where to find the declaration.
 * @param {Object} declaration
 *        An object representing the target declaration e.g. { color: red }.
 * @param {Object} newDeclaration
 *        An object representing the desired updated declaration e.g. { display: none }.
 */
async function updateDeclaration(
  view,
  ruleIndex,
  declaration,
  newDeclaration = {}
) {
  const textProp = getTextProperty(view, ruleIndex, declaration);
  const [[name, value]] = Object.entries(declaration);
  const [[newName, newValue]] = Object.entries(newDeclaration);

  if (newName && name !== newName) {
    info(
      `Updating declaration ${name}:${value};
      Changing ${name} to ${newName}`
    );
    await renameProperty(view, textProp, newName);
  }

  if (newValue && value !== newValue) {
    info(
      `Updating declaration ${name}:${value};
      Changing ${value} to ${newValue}`
    );
    await setProperty(view, textProp, newValue);
  }
}

/**
 * Check whether the given CSS declaration is compatible or not
 *
 * @param {ruleView} view
 *        The rule-view instance.
 * @param {Number} ruleIndex
 *        The index we expect the rule to have in the rule-view.
 * @param {Object} declaration
 *        An object representing the declaration e.g. { color: "red" }.
 * @param {Object} options
 * @param {string | undefined} options.expected
 *        Expected message ID for the given incompatible property.
 * If the expected message is not specified (undefined), the given declaration
 * is inferred as cross-browser compatible and is tested for same.
 * @param {string | null | undefined} options.expectedLearnMoreUrl
 *        Expected learn more link. Pass `null` to check that no "Learn more" link is displayed.
 */
async function checkDeclarationCompatibility(
  view,
  ruleIndex,
  declaration,
  { expected, expectedLearnMoreUrl }
) {
  const declarations = await getPropertiesForRuleIndex(view, ruleIndex, true);
  const [[name, value]] = Object.entries(declaration);
  const dec = `${name}:${value}`;
  const { compatibilityData } = declarations.get(dec);

  is(
    !expected,
    compatibilityData.isCompatible,
    `"${dec}" has the correct compatibility status in the payload`
  );

  is(compatibilityData.msgId, expected, `"${dec}" has expected message ID`);

  if (expected) {
    await checkInteractiveTooltip(
      view,
      "compatibility-tooltip",
      ruleIndex,
      declaration
    );
  }

  if (expectedLearnMoreUrl !== undefined) {
    // Show the tooltip
    const tooltip = view.tooltips.getTooltip("interactiveTooltip");
    const onTooltipReady = tooltip.once("shown");
    const { compatibilityIcon } = declarations.get(dec);
    await view.tooltips.onInteractiveTooltipTargetHover(compatibilityIcon);
    tooltip.show(compatibilityIcon);
    await onTooltipReady;

    const learnMoreEl = tooltip.panel.querySelector(".link");
    if (expectedLearnMoreUrl === null) {
      ok(!learnMoreEl, `"${dec}" has no "Learn more" link`);
    } else {
      ok(learnMoreEl, `"${dec}" has a "Learn more" link`);

      const { link } = await simulateLinkClick(learnMoreEl);
      is(
        link,
        expectedLearnMoreUrl,
        `Click on ${dec} "Learn more" link navigates user to expected url`
      );
    }

    // Hide the tooltip.
    const onTooltipHidden = tooltip.once("hidden");
    tooltip.hide();
    await onTooltipHidden;
  }
}

/**
 * Check that a declaration is marked inactive and that it has the expected
 * warning.
 *
 * @param {ruleView} view
 *        The rule-view instance.
 * @param {Number} ruleIndex
 *        The index we expect the rule to have in the rule-view.
 * @param {Object} declaration
 *        An object representing the declaration e.g. { color: "red" }.
 */
async function checkDeclarationIsInactive(view, ruleIndex, declaration) {
  const declarations = await getPropertiesForRuleIndex(view, ruleIndex);
  const [[name, value]] = Object.entries(declaration);
  const dec = `${name}:${value}`;
  const { used, warning } = declarations.get(dec);

  ok(!used, `"${dec}" is inactive`);
  ok(warning, `"${dec}" has a warning`);

  await checkInteractiveTooltip(
    view,
    "inactive-css-tooltip",
    ruleIndex,
    declaration
  );
}

/**
 * Check that a declaration is marked active.
 *
 * @param {ruleView} view
 *        The rule-view instance.
 * @param {Number|Array} ruleIndex
 *        The index we expect the rule to have in the rule-view. If an array, the first
 *        item is the children index in the rule view, and the second item is the child
 *        node index in the retrieved rule view element. This is helpful to select rules
 *        inside the pseudo element section.
 * @param {Object} declaration
 *        An object representing the declaration e.g. { color: "red" }.
 */
async function checkDeclarationIsActive(view, ruleIndex, declaration) {
  const declarations = await getPropertiesForRuleIndex(view, ruleIndex);
  const [[name, value]] = Object.entries(declaration);
  const dec = `${name}:${value}`;
  const { used, warning } = declarations.get(dec);

  ok(used, `${dec} is active`);
  ok(!warning, `${dec} has no warning`);
}

/**
 * Check that a tooltip contains the correct value.
 *
 * @param {ruleView} view
 *        The rule-view instance.
 *  @param {string} type
 *        The interactive tooltip type being tested.
 * @param {Number} ruleIndex
 *        The index we expect the rule to have in the rule-view.
 * @param {Object} declaration
 *        An object representing the declaration e.g. { color: "red" }.
 */
async function checkInteractiveTooltip(view, type, ruleIndex, declaration) {
  // Get the declaration
  const declarations = await getPropertiesForRuleIndex(
    view,
    ruleIndex,
    type === "compatibility-tooltip"
  );
  const [[name, value]] = Object.entries(declaration);
  const dec = `${name}:${value}`;

  // Get the relevant icon and tooltip payload data
  let icon;
  let data;
  if (type === "inactive-css-tooltip") {
    ({ icon, data } = declarations.get(dec));
  } else {
    const { compatibilityIcon, compatibilityData } = declarations.get(dec);
    icon = compatibilityIcon;
    data = compatibilityData;
  }

  // Get the tooltip.
  const tooltip = view.tooltips.getTooltip("interactiveTooltip");

  // Get the necessary tooltip helper to fetch the Fluent template.
  let tooltipHelper;
  if (type === "inactive-css-tooltip") {
    tooltipHelper = view.tooltips.inactiveCssTooltipHelper;
  } else {
    tooltipHelper = view.tooltips.compatibilityTooltipHelper;
  }

  // Get the HTML template.
  const template = tooltipHelper.getTemplate(data, tooltip);

  // Translate the template using Fluent.
  const { doc } = tooltip;
  await doc.l10n.translateFragment(template);

  // Get the expected HTML content of the now translated template.
  const expected = template.firstElementChild.outerHTML;

  // Show the tooltip for the correct icon.
  const onTooltipReady = tooltip.once("shown");
  await view.tooltips.onInteractiveTooltipTargetHover(icon);
  tooltip.show(icon);
  await onTooltipReady;

  // Get the tooltip's actual HTML content.
  const actual = tooltip.panel.firstElementChild.outerHTML;

  // Hide the tooltip.
  const onTooltipHidden = tooltip.once("hidden");
  tooltip.hide();
  await onTooltipHidden;

  // Finally, check the values.
  is(actual, expected, "Tooltip contains the correct value.");
}

/**
 * CSS compatibility test runner.
 *
 * @param {ruleView} view
 *        The rule-view instance.
 * @param {InspectorPanel} inspector
 *        The instance of InspectorPanel currently loaded in the toolbox.
 * @param {Array} tests
 *        An array of test object for this method to consume e.g.
 *          [
 *            {
 *              selector: "#flex-item",
 *              rules: [
 *                // Rule Index: 0
 *                {
 *                  // If the object doesn't include the "expected"
 *                  // key, we consider the declaration as
 *                  // cross-browser compatible and test for same
 *                  color: { value: "green" },
 *                },
 *                // Rule Index: 1
 *                {
 *                  cursor:
 *                  {
 *                    value: "grab",
 *                    expected: INCOMPATIBILITY_TOOLTIP_MESSAGE.default,
 *                    expectedLearnMoreUrl: "https://developer.mozilla.org/en-US/docs/Web/CSS/cursor",
 *                  },
 *                },
 *              ],
 *            },
 *            ...
 *          ]
 */
async function runCSSCompatibilityTests(view, inspector, tests) {
  for (const test of tests) {
    if (test.selector) {
      await selectNode(test.selector, inspector);
    }

    for (const [ruleIndex, rules] of test.rules.entries()) {
      for (const rule in rules) {
        await checkDeclarationCompatibility(
          view,
          ruleIndex,
          {
            [rule]: rules[rule].value,
          },
          {
            expected: rules[rule].expected,
            expectedLearnMoreUrl: rules[rule].expectedLearnMoreUrl,
          }
        );
      }
    }
  }
}

/**
 * Inactive CSS test runner.
 *
 * @param {ruleView} view
 *        The rule-view instance.
 * @param {InspectorPanel} inspector
 *        The instance of InspectorPanel currently loaded in the toolbox.
 * @param {Array} tests
 *        An array of test object for this method to consume e.g.
 *          [
 *            {
 *              selector: "#flex-item",
 *              // or
 *              selectNode: (inspector) => { // custom select logic }
 *              activeDeclarations: [
 *                {
 *                  declarations: {
 *                    "order": "2",
 *                  },
 *                  ruleIndex: 0,
 *                },
 *                {
 *                  declarations: {
 *                    "flex-basis": "auto",
 *                    "flex-grow": "1",
 *                    "flex-shrink": "1",
 *                  },
 *                  ruleIndex: 1,
 *                },
 *              ],
 *              inactiveDeclarations: [
 *                {
 *                  declaration: {
 *                    "flex-direction": "row",
 *                  },
 *                  ruleIndex: [1, 0],
 *                },
 *              ],
 *            },
 *            ...
 *          ]
 */
async function runInactiveCSSTests(view, inspector, tests) {
  for (const test of tests) {
    if (test.selector) {
      await selectNode(test.selector, inspector);
    } else if (typeof test.selectNode === "function") {
      await test.selectNode(inspector);
    }

    if (test.activeDeclarations) {
      info("Checking whether declarations are marked as used.");

      for (const activeDeclarations of test.activeDeclarations) {
        for (const [name, value] of Object.entries(
          activeDeclarations.declarations
        )) {
          await checkDeclarationIsActive(view, activeDeclarations.ruleIndex, {
            [name]: value,
          });
        }
      }
    }

    if (test.inactiveDeclarations) {
      info("Checking that declarations are unused and have a warning.");

      for (const inactiveDeclaration of test.inactiveDeclarations) {
        await checkDeclarationIsInactive(
          view,
          inactiveDeclaration.ruleIndex,
          inactiveDeclaration.declaration
        );
      }
    }
  }
}

/**
 * Return the checkbox element from the Rules view corresponding
 * to the given pseudo-class.
 *
 * @param  {Object} view
 *         Instance of RuleView.
 * @param  {String} pseudo
 *         Pseudo-class, like :hover, :active, :focus, etc.
 * @return {HTMLElement}
 */
function getPseudoClassCheckbox(view, pseudo) {
  return view.pseudoClassCheckboxes.filter(
    checkbox => checkbox.value === pseudo
  )[0];
}

/**
 * Check that the CSS variable output has the expected class name and data attribute.
 *
 * @param {RulesView} view
 *        The RulesView instance.
 * @param {String} selector
 *        Selector name for a rule. (e.g. "div", "div::before" and ".sample" etc);
 * @param {String} propertyName
 *        Property name (e.g. "color" and "padding-top" etc);
 * @param {String} expectedClassName
 *        The class name the variable should have.
 * @param {String} expectedDatasetValue
 *        The variable data attribute value.
 */
function checkCSSVariableOutput(
  view,
  selector,
  propertyName,
  expectedClassName,
  expectedDatasetValue
) {
  const target = getRuleViewProperty(
    view,
    selector,
    propertyName
  ).valueSpan.querySelector(`.${expectedClassName}`);

  ok(target, "The target element should exist");
  is(target.dataset.variable, expectedDatasetValue);
}

/**
 * Return specific rule ancestor data element (i.e. the one containing @layer / @media
 * information) from the Rules view
 *
 * @param {RulesView} view
 *        The RulesView instance.
 * @param {Number} ruleIndex
 * @returns {HTMLElement}
 */
function getRuleViewAncestorRulesDataElementByIndex(view, ruleIndex) {
  return view.styleDocument
    .querySelectorAll(`.ruleview-rule`)
    [ruleIndex]?.querySelector(`.ruleview-rule-ancestor-data`);
}

/**
 * Return specific rule ancestor data text from the Rules view.
 * Will return something like "@layer topLayer\n@media screen\n@layer".
 *
 * @param {RulesView} view
 *        The RulesView instance.
 * @param {Number} ruleIndex
 * @returns {String}
 */
function getRuleViewAncestorRulesDataTextByIndex(view, ruleIndex) {
  return getRuleViewAncestorRulesDataElementByIndex(view, ruleIndex)?.innerText;
}

/**
 * Runs a sequence of tests against the provided property editor.
 *
 * @param {TextPropertyEditor} propertyEditor
 *     The TextPropertyEditor instance to test.
 * @param {RuleView} view
 *     The RuleView which owns the propertyEditor.
 * @param {Array<object>} test
 *     The array of tests to run.
 */
async function runIncrementTest(propertyEditor, view, tests) {
  propertyEditor.valueSpan.scrollIntoView();
  const editor = await focusEditableField(view, propertyEditor.valueSpan);

  for (const testIndex in tests) {
    await testIncrement(editor, view, tests[testIndex], testIndex);
  }

  // Blur the field to put back the UI in its initial state (and avoid pending
  // requests when the test ends).
  const onRuleViewChanged = view.once("ruleview-changed");
  EventUtils.synthesizeKey("VK_ESCAPE", {}, view.styleWindow);
  view.debounce.flush();
  await onRuleViewChanged;
}

/**
 * Individual test runner for increment tests used via runIncrementTest in
 * browser_rules_edit-property-increments.js and similar tests.
 *
 * Will attempt to increment the value of the provided inplace editor based on
 * the test options provided.
 *
 * @param {InplaceEditor} editor
 *     The InplaceEditor instance to test.
 * @param {RuleView} view
 *     The RuleView which owns the editor.
 * @param {object} test
 * @param {boolean=} test.alt
 *     Whether alt should be depressed.
 * @param {boolean=} test.ctrl
 *     Whether ctrl should be depressed.
 * @param {number=} test.deltaX
 *     Only relevant if test.wheel=true, value of the wheel delta on the horizontal axis.
 * @param {number=} test.deltaY
 *     Only relevant if test.wheel=true, value of the wheel delta on the vertical axis.
 * @param {boolean=} test.down
 *     For key increment tests, whether this should simulate pressing the down
 *     arrow, or the up arrow. down, pagedown and pageup are mutually exclusive.
 * @param {string} test.end
 *     The expected value at the end of the test.
 * @param {boolean=} test.pagedown
 *     For key increment tests, whether this should simulate pressing the
 *     pagedown key. down, pagedown and pageup are mutually exclusive.
 * @param {boolean=} test.pageup
 *     For key increment tests, whether this should simulate pressing the
 *     pageup key. down, pagedown and pageup are mutually exclusive.
 * @param {boolean=} test.selectAll
 *     Whether all the input text should be selected. You can also specify a
 *     range with test.selection.
 * @param {Array<number>=} test.selection
 *     An array of two numbers which corresponds to the initial selection range.
 * @param {boolean=} test.shift
 *     Whether shift should be depressed.
 * @param {string} test.start
 *     The input value at the beginning of the test.
 * @param {boolean=} test.wheel
 *     True if the test should use wheel events to increment the value.
 * @param {number} testIndex
 *     The test index, used for logging.
 */
async function testIncrement(editor, view, test, testIndex) {
  editor.input.value = test.start;
  const input = editor.input;

  if (test.selectAll) {
    input.select();
  } else if (test.selection) {
    input.setSelectionRange(test.selection[0], test.selection[1]);
  }

  is(input.value, test.start, "Value initialized at " + test.start);

  const onRuleViewChanged = view.once("ruleview-changed");

  let smallIncrementKey = { ctrlKey: test.ctrl };
  if (AppConstants.platform === "macosx") {
    smallIncrementKey = { altKey: test.alt };
  }

  const options = {
    shiftKey: test.shift,
    ...smallIncrementKey,
  };

  if (test.wheel) {
    // If test.wheel is true, we should increment the value using the wheel.
    const onWheel = once(input, "wheel");
    input.dispatchEvent(
      new view.styleWindow.WheelEvent("wheel", {
        deltaX: test.deltaX,
        deltaY: test.deltaY,
        deltaMode: 0,
        ...options,
      })
    );
    await onWheel;
  } else {
    let key;
    key = test.down ? "VK_DOWN" : "VK_UP";
    if (test.pageDown) {
      key = "VK_PAGE_DOWN";
    } else if (test.pageUp) {
      key = "VK_PAGE_UP";
    }
    const onKeyUp = once(input, "keyup");
    EventUtils.synthesizeKey(key, options, view.styleWindow);

    await onKeyUp;
  }

  // Only expect a change if the value actually changed!
  if (test.start !== test.end) {
    view.debounce.flush();
    await onRuleViewChanged;
  }

  is(input.value, test.end, `[Test ${testIndex}] Value changed to ${test.end}`);
}

function getSmallIncrementKey() {
  if (AppConstants.platform === "macosx") {
    return { alt: true };
  }
  return { ctrl: true };
}
