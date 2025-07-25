/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

/*
 * Tests the Add Search Engine option in the search bar.
 */

"use strict";

const { PromptTestUtils } = ChromeUtils.importESModule(
  "resource://testing-common/PromptTestUtils.sys.mjs"
);

const rootDir = getRootDirectory(gTestPath);
const searchPopup = document.getElementById("PopupSearchAutoComplete");
let searchbar;

add_setup(async function () {
  searchbar = await gCUITestUtils.addSearchBar();

  registerCleanupFunction(async function () {
    gCUITestUtils.removeSearchBar();
    Services.search.restoreDefaultEngines();
  });
});

add_task(async function test_addEngine() {
  let tab = await BrowserTestUtils.openNewForegroundTab(
    gBrowser,
    rootDir + "opensearch.html"
  );

  let shownPromise = promiseEvent(searchPopup, "popupshown");
  let builtPromise = promiseEvent(searchPopup.oneOffButtons, "rebuild");
  EventUtils.synthesizeMouseAtCenter(
    searchbar.querySelector(".searchbar-search-button"),
    {}
  );
  await Promise.all([shownPromise, builtPromise]);

  let addEngineList = searchPopup.querySelectorAll(
    ".searchbar-engine-one-off-add-engine"
  );
  Assert.equal(addEngineList.length, 3, "All items are in addEngineList");
  let item = addEngineList[0];

  let enginePromise = SearchTestUtils.promiseEngine("Foo");
  builtPromise = promiseEvent(searchPopup.oneOffButtons, "rebuild");
  EventUtils.synthesizeMouseAtCenter(item, {});
  info("Waiting for engine to be installed.");
  let engine = await enginePromise;
  Assert.ok(true, "Engine was installed.");
  await builtPromise;

  let oneOffButton = searchPopup.oneOffButtons
    .getSelectableButtons(false)
    .find(b => b.engine?.id == engine.id);

  // Image URL in testEngine.xml is very long, so we only check its end.
  Assert.ok(oneOffButton?.image.endsWith("AElFTkSuQmCC"), "Image is correct");

  await Services.search.removeEngine(engine);
  BrowserTestUtils.removeTab(tab);
});

add_task(async function test_invalidEngine() {
  let tab = await BrowserTestUtils.openNewForegroundTab(
    gBrowser,
    rootDir + "opensearch.html"
  );
  let promise = promiseEvent(searchPopup, "popupshown");
  await EventUtils.synthesizeMouseAtCenter(
    searchbar.querySelector(".searchbar-search-button"),
    {}
  );
  await promise;

  let addEngineList;
  await TestUtils.waitForCondition(() => {
    addEngineList = searchPopup.querySelectorAll(
      ".searchbar-engine-one-off-add-engine"
    );
    return addEngineList.length;
  }, "Wait until at least one item in the addEngineList");
  let item;

  await TestUtils.waitForCondition(() => {
    item = addEngineList[addEngineList.length - 1];
    return item?.tooltipText.includes("engineInvalid");
  }, "Wait until the tooltip will be correct");
  Assert.ok(true, "Last item should be the invalid entry");

  let promptPromise = PromptTestUtils.waitForPrompt(tab.linkedBrowser, {
    modalType: Ci.nsIPromptService.MODAL_TYPE_CONTENT,
    promptType: "alert",
  });

  await EventUtils.synthesizeMouseAtCenter(item, {});

  let prompt = await promptPromise;

  Assert.ok(
    prompt.ui.infoBody.textContent.includes(
      "http://mochi.test:8888/browser/browser/components/search/test/browser/testEngine_404.xml"
    ),
    "Should have included the url in the prompt body"
  );

  await PromptTestUtils.handlePrompt(prompt);
  BrowserTestUtils.removeTab(tab);
});

add_task(async function test_onOnlyDefaultEngine() {
  info("Remove engines except default");
  const defaultEngine = Services.search.defaultEngine;
  const engines = await Services.search.getVisibleEngines();
  for (const engine of engines) {
    if (defaultEngine.name !== engine.name) {
      await Services.search.removeEngine(engine);
    }
  }

  info("Show popup");
  const tab = await BrowserTestUtils.openNewForegroundTab(
    gBrowser,
    rootDir + "opensearch.html"
  );
  const onShown = promiseEvent(searchPopup, "popupshown");
  await EventUtils.synthesizeMouseAtCenter(
    searchbar.querySelector(".searchbar-search-button"),
    {}
  );
  await onShown;

  const addEngineList = searchPopup.querySelectorAll(
    ".searchbar-engine-one-off-add-engine"
  );
  Assert.equal(addEngineList.length, 3, "Add engines should be shown");

  BrowserTestUtils.removeTab(tab);
});
