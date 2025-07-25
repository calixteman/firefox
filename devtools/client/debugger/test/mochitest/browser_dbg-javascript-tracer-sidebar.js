/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

// Tests the Javascript Tracing feature.

"use strict";

add_task(async function () {
  // This is preffed off for now, so ensure turning it on
  await pushPref("devtools.debugger.features.javascript-tracing", true);

  const dbg = await initDebugger("doc-scripts.html");

  await SpecialPowers.spawn(gBrowser.selectedBrowser, [], async function () {
    // Register a global event listener to cover listeners set to DOM Element as well as on the global
    // This may regress the DOM Events panel to show more than one entry for the click event.
    content.eval(`window.onclick = () => {};`);
  });

  info("Force the log method to be the debugger sidebar");
  await toggleJsTracerMenuItem(dbg, "#jstracer-menu-item-debugger-sidebar");

  info("Enable the tracing");
  await toggleJsTracer(dbg.toolbox);

  is(
    dbg.selectors.getSelectedPrimaryPaneTab(),
    "tracer",
    "The tracer sidebar is automatically shown on start"
  );

  const argumentSearchInput = findElementWithSelector(
    dbg,
    `#tracer-tab-panel .call-tree-container input`
  );
  is(
    argumentSearchInput.disabled,
    true,
    "The input to search by values is disabled"
  );

  info("Toggle off and on in order to record with values");
  await toggleJsTracer(dbg.toolbox);
  info("Enable values recording");
  await toggleJsTracerMenuItem(dbg, "#jstracer-menu-item-log-values");
  await toggleJsTracer(dbg.toolbox);

  const topLevelThreadActorID =
    dbg.toolbox.commands.targetCommand.targetFront.threadFront.actorID;
  info("Wait for tracing to be enabled");
  await waitForState(dbg, () => {
    return dbg.selectors.getIsThreadCurrentlyTracing(topLevelThreadActorID);
  });

  is(
    argumentSearchInput.disabled,
    false,
    "The input to search by values is no longer disabled"
  );

  let tracerMessage = findElementWithSelector(
    dbg,
    "#tracer-tab-panel .tracer-message"
  );
  is(tracerMessage.textContent, "Waiting for the first JavaScript executions");

  invokeInTab("main");

  info("Wait for the call tree to appear in the tracer panel");
  const tracerTree = await waitForElementWithSelector(
    dbg,
    "#tracer-tab-panel .tree"
  );

  info("Wait for the expected traces to appear in the call tree");
  let traces = await waitFor(() => {
    const elements = tracerTree.querySelectorAll(".trace-line");
    if (elements.length == 3) {
      return elements;
    }
    return false;
  });
  is(traces[0].textContent, "λ main simple1.js:1:16");
  is(traces[1].textContent, "λ foo simple2.js:1:12");
  is(traces[2].textContent, "λ bar simple2.js:3:4");
  ok(
    !findElement(dbg, "tracedLine"),
    "Before selecting any trace, no line is highlighted in CodeMirror"
  );

  info("Select the trace for the call to `foo`");
  EventUtils.synthesizeMouseAtCenter(traces[1], {}, dbg.win);

  let focusedTrace = await waitFor(
    () => tracerTree.querySelector(".tree-node.focused .trace-line"),
    "Wait for the line to be focused in the tracer panel"
  );
  is(focusedTrace, traces[1], "The clicked trace is now focused");
  await waitFor(
    () => findElement(dbg, "tracedLine"),
    "Wait for the traced line to be highlighted in CodeMirror"
  );
  ok(
    findElement(dbg, "tracedLine"),
    "When a trace is selected, the line is highlighted in CodeMirror"
  );

  // Naive sanity checks for inlines previews
  await assertInlinePreviews(
    dbg,
    [
      {
        previews: [
          { identifier: "x:", value: "1" },
          { identifier: "y:", value: "2" },
        ],
        line: 1,
      },
    ],
    "foo"
  );

  // Naive sanity checks for popup previews on hovering
  {
    const { element: popupEl, tokenEl } = await tryHovering(
      dbg,
      1,
      14,
      "previewPopup"
    );
    is(popupEl.querySelector(".objectBox")?.textContent, "1");
    await closePreviewForToken(dbg, tokenEl, "previewPopup");
  }

  {
    const { element: popupEl, tokenEl } = await tryHovering(
      dbg,
      1,
      17,
      "previewPopup"
    );
    is(popupEl.querySelector(".objectBox")?.textContent, "2");
    await closePreviewForToken(dbg, tokenEl, "previewPopup");
  }

  let focusedPausedFrame = findElementWithSelector(
    dbg,
    ".frames .frame.selected"
  );
  ok(!focusedPausedFrame, "Before pausing, there is no selected paused frame");

  info("Trigger a breakpoint");
  const onResumed = SpecialPowers.spawn(
    gBrowser.selectedBrowser,
    [],
    async function () {
      content.eval("debugger;");
    }
  );
  await waitForPaused(dbg);
  await waitForSelectedLocation(dbg, 1, 1);

  focusedPausedFrame = findElementWithSelector(dbg, ".frames .frame.selected");
  ok(
    !!focusedPausedFrame,
    "When paused, a frame is selected in the call stack panel"
  );

  focusedTrace = tracerTree.querySelector(".tree-node.focused .trace-line");
  is(focusedTrace, null, "When pausing, there is no trace selected anymore");

  info("Re select the tracer frame while being paused");
  EventUtils.synthesizeMouseAtCenter(traces[1], {}, dbg.win);

  await waitForSelectedLocation(dbg, 1, 13);
  focusedPausedFrame = findElementWithSelector(dbg, ".frames .frame.selected");
  ok(
    !focusedPausedFrame,
    "While paused, if we select a tracer frame, the paused frame is no longer highlighted in the call stack panel"
  );
  const highlightedPausedFrame = findElementWithSelector(
    dbg,
    ".frames .frame.inactive"
  );
  ok(
    !!highlightedPausedFrame,
    "But it is still highlighted as inactive with a grey background"
  );
  ok(
    findElement(dbg, "tracedLine"),
    "When a trace is selected, while being paused, the line is highlighted as traced in CodeMirror"
  );
  ok(
    !findElement(dbg, "pausedLine"),
    "The traced line is not highlighted as paused"
  );

  await resume(dbg);
  await onResumed;

  ok(
    findElement(dbg, "tracedLine"),
    "After resuming, the traced line is still highlighted in CodeMirror"
  );

  // Trigger a click in the content page to verify we do trace DOM events
  BrowserTestUtils.synthesizeMouseAtCenter(
    "button",
    {},
    gBrowser.selectedBrowser
  );

  const [nodeClickTrace, globalClickTrace] = await waitFor(() => {
    const elts = tracerTree.querySelectorAll(".tracer-dom-event");
    if (elts.length == 2) {
      return elts;
    }
    return false;
  });
  // This is the listener set on the <button> element
  is(nodeClickTrace.textContent, "DOM | node.click");
  // This is the listener set on the window object
  is(globalClickTrace.textContent, "DOM | global.click");

  await BrowserTestUtils.synthesizeKey("x", {}, gBrowser.selectedBrowser);
  const keyTrace = await waitFor(() => {
    // Scroll to bottom to ensure rendering the last elements (otherwise they are not because of VirtualizedTree)
    tracerTree.scrollTop = tracerTree.scrollHeight;
    const elts = tracerTree.querySelectorAll(".tracer-dom-event");
    if (elts.length == 3) {
      return elts[2];
    }
    return false;
  });
  is(keyTrace.textContent, "DOM | global.keypress");

  info("Wait for the key listener function to be displayed");
  await waitFor(() => {
    // Scroll to bottom to ensure rendering the last elements (otherwise they are not because of VirtualizedTree)
    tracerTree.scrollTop = tracerTree.scrollHeight;
    const elements = tracerTree.querySelectorAll(".trace-line");
    // Wait for the expected element to be rendered
    if (elements[elements.length - 1].textContent.includes("keyListener")) {
      return true;
    }
    return false;
  });

  info("Trigger a DOM Mutation");
  await SpecialPowers.spawn(gBrowser.selectedBrowser, [], async function () {
    content.eval(`
      window.doMutation = () => {
        const div = document.createElement("div");
        document.body.appendChild(div);
        //# sourceURL=foo.js
      };
      `);
    content.wrappedJSObject.doMutation();
  });

  // Wait for the `eval` and the `doMutation` calls to be rendered
  traces = await waitFor(() => {
    // Scroll to bottom to ensure rendering the last elements (otherwise they are not because of VirtualizedTree)
    tracerTree.scrollTop = tracerTree.scrollHeight;
    const elements = tracerTree.querySelectorAll(".trace-line");
    // Wait for the expected element to be rendered
    if (
      elements[elements.length - 1].textContent.includes("window.doMutation")
    ) {
      return elements;
    }
    return false;
  });

  const doMutationTrace = traces[traces.length - 1];
  is(doMutationTrace.textContent, "λ window.doMutation eval:2:32");

  // Expand the call to doMutation in order to show the DOM Mutation in the tree
  doMutationTrace.querySelector(".arrow").click();

  const mutationTrace = await waitFor(() =>
    tracerTree.querySelector(".tracer-dom-mutation")
  );
  is(mutationTrace.textContent, "DOM Mutation | add");

  // Click on the mutation trace to open its source
  mutationTrace.click();
  await waitForSelectedSource(dbg, "foo.js");

  info("Open the DOM event list");
  const eventListToggleButton = await waitForElementWithSelector(
    dbg,
    "#tracer-tab-panel #tracer-events-tab"
  );
  // Use synthesizeMouseAtCenter as calling click() method somehow triggers mouse over
  // on the event categories...
  EventUtils.synthesizeMouseAtCenter(eventListToggleButton, {}, dbg.win);

  let domEventCategories = findAllElementsWithSelector(
    dbg,
    "#tracer-tab-panel .event-listener-category"
  );
  is(domEventCategories.length, 2);
  is(domEventCategories[0].textContent, "Keyboard");
  is(domEventCategories[1].textContent, "Mouse");

  info("Expand the Mouse category");
  domEventCategories[1]
    .closest(".event-listener-header")
    .querySelector(".event-listener-expand")
    .click();
  const clickEventName = await waitFor(() => {
    const eventNames = domEventCategories[1]
      .closest(".event-listener-group")
      .querySelectorAll(".event-listener-name");
    if (eventNames.length == 1) {
      return eventNames[0];
    }
    return false;
  }, "There is only one mouse event");
  is(
    clickEventName.textContent,
    "click",
    "and that one mouse event is 'click'"
  );
  info("Fold the Mouse category");
  domEventCategories[1]
    .closest(".event-listener-header")
    .querySelector(".event-listener-expand")
    .click();

  // Test event highlighting on mouse over
  is(
    findAllElementsWithSelector(dbg, ".tracer-slider-event.highlighted").length,
    0,
    "No event is highlighted in the timeline"
  );
  info("Mouse over the Keyboard category");
  EventUtils.synthesizeMouseAtCenter(
    domEventCategories[0],
    { type: "mousemove" },
    dbg.win
  );
  await waitFor(() => {
    return (
      findAllElementsWithSelector(dbg, ".tracer-slider-event.highlighted")
        .length == 1
    );
  }, "The setTimeout event is highlighted in the timeline");

  // Before toggling some DOM events, assert that the three events are displayed in the timeline
  // (node and global click, and node keypress)
  is(findAllElementsWithSelector(dbg, ".tracer-slider-event").length, 3);
  info("Toggle off the Keyboard and then the Mouse events");
  domEventCategories[0].click();
  await waitFor(
    () => findAllElementsWithSelector(dbg, ".tracer-slider-event").length == 2
  );
  domEventCategories[1].click();
  // Now that all events are disabled, there is no more trace displayed in the timeline
  await waitFor(
    () => !findAllElementsWithSelector(dbg, ".tracer-slider-event").length
  );
  tracerMessage = findElementWithSelector(
    dbg,
    "#tracer-tab-panel .tracer-message"
  );
  is(tracerMessage.textContent, "All traces have been filtered out");

  info("Trigger a setTimeout to have a new event category");
  await SpecialPowers.spawn(gBrowser.selectedBrowser, [], async function () {
    content.eval(`
      window.setTimeout(function () {
        console.log("timeout fired");
      });
      `);
  });
  domEventCategories = await waitFor(() => {
    const categories = findAllElementsWithSelector(
      dbg,
      "#tracer-tab-panel .event-listener-category"
    );
    if (categories.length == 3) {
      return categories;
    }
    return false;
  });
  is(domEventCategories[2].textContent, "Timer");
  is(
    findAllElementsWithSelector(dbg, ".tracer-slider-event").length,
    1,
    "The setTimeout callback is displayed in the timeline"
  );

  info(
    "Check each category checked status before enabling only keyboad instead of time"
  );
  const domEventCheckboxes = findAllElementsWithSelector(
    dbg,
    `#tracer-tab-panel .event-listener-label input`
  );
  is(domEventCheckboxes[0].checked, false);
  is(domEventCheckboxes[1].checked, false);
  is(domEventCheckboxes[2].checked, true);

  info(
    "CmdOrCtrl + click on the Keyboard categorie to force selecting only this category"
  );
  EventUtils.synthesizeMouseAtCenter(
    domEventCategories[0],
    { [Services.appinfo.OS === "Darwin" ? "metaKey" : "ctrlKey"]: true },
    dbg.win
  );

  info("Wait for the event checkboxes to be updated");
  await waitFor(() => {
    return domEventCheckboxes[0].checked;
  });
  is(domEventCheckboxes[0].checked, true);
  is(domEventCheckboxes[1].checked, false);
  is(domEventCheckboxes[2].checked, false);

  // Test Disabling tracing
  info("Disable the tracing");
  await toggleJsTracer(dbg.toolbox);
  info("Wait for tracing to be disabled");
  await waitForState(dbg, () => {
    return !dbg.selectors.getIsThreadCurrentlyTracing(topLevelThreadActorID);
  });

  invokeInTab("inline_script2");

  // Let some time for the tracer to appear if we failed disabling the tracing
  await wait(1000);

  info("Reset back to the default value");
  await toggleJsTracerMenuItem(dbg, "#jstracer-menu-item-console");
  await toggleJsTracerMenuItem(dbg, "#jstracer-menu-item-log-values");
});
