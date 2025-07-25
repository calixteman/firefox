/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

// Load the shared-head file first.
Services.scriptloader.loadSubScript(
  "chrome://mochitests/content/browser/devtools/client/shared/test/shared-head.js",
  this
);

/* import-globals-from helper-mocks.js */
Services.scriptloader.loadSubScript(CHROME_URL_ROOT + "helper-mocks.js", this);

Services.scriptloader.loadSubScript(
  "chrome://mochitests/content/browser/devtools/client/webconsole/test/browser/shared-head.js",
  this
);

// Make sure the ADB addon is removed and ADB is stopped when the test ends.
registerCleanupFunction(async function () {
  try {
    const {
      adbAddon,
    } = require("resource://devtools/client/shared/remote-debugging/adb/adb-addon.js");
    await adbAddon.uninstall();
  } catch (e) {
    // Will throw if the addon is already uninstalled, ignore exceptions here.
  }
  const {
    adbProcess,
  } = require("resource://devtools/client/shared/remote-debugging/adb/adb-process.js");
  await adbProcess.kill();

  const {
    remoteClientManager,
  } = require("resource://devtools/client/shared/remote-debugging/remote-client-manager.js");
  await remoteClientManager.removeAllClients();
});

async function openAboutDebugging({
  enableWorkerUpdates,
  enableLocalTabs = true,
} = {}) {
  if (!enableWorkerUpdates) {
    silenceWorkerUpdates();
  }

  // This preference changes value depending on the build type, tests need to use a
  // consistent value regarless of the build used.
  await pushPref(
    "devtools.aboutdebugging.local-tab-debugging",
    enableLocalTabs
  );

  info("opening about:debugging");

  const tab = await addTab("about:debugging");
  const browser = tab.linkedBrowser;
  const document = browser.contentDocument;
  const window = browser.contentWindow;

  info("Wait until Connect page is displayed");
  await waitUntil(() => document.querySelector(".qa-connect-page"));

  return { tab, document, window };
}

async function openAboutDevtoolsToolbox(
  doc,
  tab,
  win,
  targetText = "about:debugging",
  shouldWaitToolboxReady = true
) {
  info("Open about:devtools-toolbox page");

  info("Wait for the target to appear: " + targetText);
  await waitUntil(() => findDebugTargetByText(targetText, doc));

  const target = findDebugTargetByText(targetText, doc);
  ok(target, `${targetText} target appeared`);

  const {
    DEBUG_TARGETS,
  } = require("resource://devtools/client/aboutdebugging/src/constants.js");
  const isWebExtension = target.dataset.qaTargetType == DEBUG_TARGETS.EXTENSION;

  const inspectButton = target.querySelector(".qa-debug-target-inspect-button");
  ok(inspectButton, `Inspect button for ${targetText} appeared`);
  inspectButton.click();
  const onToolboxReady = gDevTools.once("toolbox-ready");

  info("Wait for about debugging requests to settle");
  await waitForAboutDebuggingRequests(win.AboutDebugging.store);

  if (shouldWaitToolboxReady) {
    info("Wait for onToolboxReady");
    await onToolboxReady;
  }

  const { runtimes } = win.AboutDebugging.store.getState();
  const isOnThisFirefox = runtimes.selectedRuntimeId === "this-firefox";
  const isLocalWebExtension = isWebExtension && isOnThisFirefox;

  // Local WebExtension toolboxes open in a dedicated window
  if (isLocalWebExtension) {
    const toolbox = await onToolboxReady;
    // For some reason the test helpers prevents the toolbox from being automatically focused on opening,
    // whereas it is IRL.
    const focusedWin = Services.focus.focusedWindow;
    if (focusedWin?.top != toolbox.win) {
      info("Wait for the toolbox window to be focused");
      await new Promise(r => {
        // focus event only fired on the chrome event handler and in capture phase
        toolbox.win.docShell.chromeEventHandler.addEventListener("focus", r, {
          once: true,
          capture: true,
        });
        toolbox.win.focus();
      });
      info("The toolbox is focused");
    }
    return {
      devtoolsBrowser: null,
      devtoolsDocument: toolbox.doc,
      devtoolsTab: null,
      devtoolsWindow: toolbox.win,
    };
  }

  info("Wait until a new tab is opened");
  await waitUntil(() => tab.nextElementSibling);

  info("Wait for about:devtools-toolbox tab will be selected");
  const devtoolsTab = tab.nextElementSibling;
  await waitUntil(() => gBrowser.selectedTab === devtoolsTab);
  const devtoolsBrowser = gBrowser.selectedBrowser;
  info("Wait for about:devtools-toolbox tab to have the expected URL");
  await waitUntil(() =>
    devtoolsBrowser.contentWindow.location.href.startsWith(
      "about:devtools-toolbox?"
    )
  );

  if (!shouldWaitToolboxReady) {
    // Wait for show error page.
    await waitUntil(() =>
      devtoolsBrowser.contentDocument.querySelector(".qa-error-page")
    );
  }

  return {
    devtoolsBrowser,
    devtoolsDocument: devtoolsBrowser.contentDocument,
    devtoolsTab,
    devtoolsWindow: devtoolsBrowser.contentWindow,
  };
}

async function closeAboutDevtoolsToolbox(
  aboutDebuggingDocument,
  devtoolsTab,
  win
) {
  // Wait for all requests to settle on the opened about:devtools toolbox.
  const devtoolsBrowser = devtoolsTab.linkedBrowser;
  const devtoolsWindow = devtoolsBrowser.contentWindow;
  const toolbox = getToolbox(devtoolsWindow);

  info("Wait for requests to settle");
  await toolbox.commands.client.waitForRequestsToSettle({
    ignoreOrphanedFronts: true,
  });

  info("Close about:devtools-toolbox page");
  const onToolboxDestroyed = gDevTools.once("toolbox-destroyed");

  info("Wait for removeTab");
  await removeTab(devtoolsTab);

  info("Wait for toolbox destroyed");
  await onToolboxDestroyed;

  // Changing the tab will also trigger a request to list tabs, so wait until the selected
  // tab has changed to wait for requests to settle.
  info("Wait until aboutdebugging is selected");
  await waitUntil(() => gBrowser.selectedTab !== devtoolsTab);

  // Wait for removing about:devtools-toolbox tab info from about:debugging.
  info("Wait until about:devtools-toolbox is removed from debug targets");
  await waitUntil(
    () => !findDebugTargetByText("Toolbox - ", aboutDebuggingDocument)
  );

  await waitForAboutDebuggingRequests(win.AboutDebugging.store);
}

async function closeWebExtAboutDevtoolsToolbox(devtoolsWindow, win) {
  // Wait for all requests to settle on the opened about:devtools toolbox.
  const toolbox = getToolbox(devtoolsWindow);
  await toolbox.commands.client.waitForRequestsToSettle();

  info("Close the toolbox and wait for its destruction");
  await toolbox.destroy();

  await waitForAboutDebuggingRequests(win.AboutDebugging.store);
}

async function reloadAboutDebugging(tab) {
  info("reload about:debugging");

  await reloadBrowser(tab.linkedBrowser);
  const browser = tab.linkedBrowser;
  const document = browser.contentDocument;
  const window = browser.contentWindow;
  info("wait for the initial about:debugging requests to settle");
  await waitForAboutDebuggingRequests(window.AboutDebugging.store);

  return document;
}

// Wait for all about:debugging target request actions to succeed.
// They will typically be triggered after watching a new runtime or loading
// about:debugging.
function waitForRequestsSuccess(store) {
  return Promise.all([
    waitForDispatch(store, "REQUEST_EXTENSIONS_SUCCESS"),
    waitForDispatch(store, "REQUEST_TABS_SUCCESS"),
    waitForDispatch(store, "REQUEST_WORKERS_SUCCESS"),
  ]);
}

/**
 * Wait for all aboutdebugging REQUEST_*_SUCCESS actions to settle, meaning here
 * that no new request has been dispatched after the provided delay.
 */
async function waitForAboutDebuggingRequests(store, delay = 500) {
  let hasSettled = false;

  // After each iteration of this while loop, we check is the timerPromise had the time
  // to resolve or if we captured a REQUEST_*_SUCCESS action before.
  while (!hasSettled) {
    let timer;

    // This timer will be executed only if no REQUEST_*_SUCCESS action is dispatched
    // during the delay. We consider that when no request are received for some time, it
    // means there are no ongoing requests anymore.
    const timerPromise = new Promise(resolve => {
      timer = setTimeout(() => {
        hasSettled = true;
        resolve();
      }, delay);
    });

    // Wait either for a REQUEST_*_SUCCESS to be dispatched, or for the timer to resolve.
    await Promise.race([
      waitForDispatch(store, "REQUEST_EXTENSIONS_SUCCESS"),
      waitForDispatch(store, "REQUEST_TABS_SUCCESS"),
      waitForDispatch(store, "REQUEST_WORKERS_SUCCESS"),
      timerPromise,
    ]);

    // Clear the timer to avoid setting hasSettled to true accidently unless timerPromise
    // was the first to resolve.
    clearTimeout(timer);
  }
}

/**
 * Navigate to "This Firefox"
 */
async function selectThisFirefoxPage(doc, store) {
  info("Select This Firefox page");

  const onRequestSuccess = waitForRequestsSuccess(store);
  doc.location.hash = "#/runtime/this-firefox";
  info("Wait for requests to be complete");
  await onRequestSuccess;

  info("Wait for runtime page to be rendered");
  await waitUntil(() => doc.querySelector(".qa-runtime-page"));

  // Navigating to this-firefox will trigger a title change for the
  // about:debugging tab. This title change _might_ trigger a tablist update.
  // If it does, we should make sure to wait for pending tab requests.
  await waitForAboutDebuggingRequests(store);
}

/**
 * Navigate to the Connect page. Resolves when the Connect page is rendered.
 */
async function selectConnectPage(doc) {
  const sidebarItems = doc.querySelectorAll(".qa-sidebar-item");
  const connectSidebarItem = [...sidebarItems].find(element => {
    return element.textContent === "Setup";
  });
  ok(connectSidebarItem, "Sidebar contains a Connect item");
  const connectLink = connectSidebarItem.querySelector(".qa-sidebar-link");
  ok(connectLink, "Sidebar contains a Connect link");

  info("Click on the Connect link in the sidebar");
  connectLink.click();

  info("Wait until Connect page is displayed");
  await waitUntil(() => doc.querySelector(".qa-connect-page"));
}

function getDebugTargetPane(title, document) {
  // removes the suffix "(<NUMBER>)" in debug target pane's title, if needed
  const sanitizeTitle = x => {
    return x.replace(/\s+\(\d+\)$/, "");
  };

  const targetTitle = sanitizeTitle(title);
  for (const titleEl of document.querySelectorAll(
    ".qa-debug-target-pane-title"
  )) {
    if (sanitizeTitle(titleEl.textContent) !== targetTitle) {
      continue;
    }

    return titleEl.closest(".qa-debug-target-pane");
  }

  return null;
}

function findDebugTargetByText(text, document) {
  const targets = [...document.querySelectorAll(".qa-debug-target-item")];
  return targets.find(target => target.textContent.includes(text));
}

function findSidebarItemByText(text, document) {
  const sidebarItems = document.querySelectorAll(".qa-sidebar-item");
  return [...sidebarItems].find(element => {
    return element.textContent.includes(text);
  });
}

function findSidebarItemLinkByText(text, document) {
  const links = document.querySelectorAll(".qa-sidebar-link");
  return [...links].find(element => {
    return element.textContent.includes(text);
  });
}

async function connectToRuntime(deviceName, document) {
  info(`Wait until the sidebar item for ${deviceName} appears`);
  await waitUntil(() => findSidebarItemByText(deviceName, document));
  const sidebarItem = findSidebarItemByText(deviceName, document);
  const connectButton = sidebarItem.querySelector(".qa-connect-button");
  ok(
    connectButton,
    `Connect button is displayed for the runtime ${deviceName}`
  );

  info("Click on the connect button and wait until it disappears");
  connectButton.click();
  await waitUntil(() => !sidebarItem.querySelector(".qa-connect-button"));
}

async function waitForRuntimePage(name, document) {
  await waitUntil(() => {
    const runtimeInfo = document.querySelector(".qa-runtime-name");
    return runtimeInfo && runtimeInfo.textContent.includes(name);
  });
}

async function selectRuntime(deviceName, name, document) {
  const sidebarItem = findSidebarItemByText(deviceName, document);
  const store = document.defaultView.AboutDebugging.store;
  const onSelectPageSuccess = waitForDispatch(store, "SELECT_PAGE_SUCCESS");

  sidebarItem.querySelector(".qa-sidebar-link").click();

  await waitForRuntimePage(name, document);

  info("Wait for SELECT_PAGE_SUCCESS to be dispatched");
  await onSelectPageSuccess;
}

function getToolbox(win) {
  return gDevTools.getToolboxes().find(toolbox => toolbox.win === win);
}

/**
 * Open the performance profiler dialog. Assumes the client is a mocked remote runtime
 * client.
 */
async function openProfilerDialog(client, doc) {
  const onProfilerLoaded = new Promise(r => {
    client.loadPerformanceProfiler = r;
  });

  info("Click on the Profile Runtime button");
  const profileButton = doc.querySelector(".qa-profile-runtime-button");
  profileButton.click();

  info(
    "Wait for the loadPerformanceProfiler callback to be executed on client-wrapper"
  );
  return onProfilerLoaded;
}

/**
 * The "This Firefox" string depends on the brandShortName, which will be different
 * depending on the channel where tests are running.
 */
function getThisFirefoxString(aboutDebuggingWindow) {
  const loader = aboutDebuggingWindow.getBrowserLoaderForWindow();
  const { l10n } = loader.require(
    "resource://devtools/client/aboutdebugging/src/modules/l10n.js"
  );
  return l10n.getString("about-debugging-this-firefox-runtime-name");
}

function waitUntilUsbDeviceIsUnplugged(deviceName, aboutDebuggingDocument) {
  info("Wait until the USB sidebar item appears as unplugged");
  return waitUntil(() => {
    const sidebarItem = findSidebarItemByText(
      deviceName,
      aboutDebuggingDocument
    );
    return !!sidebarItem.querySelector(".qa-runtime-item-unplugged");
  });
}

/**
 * Changing the selected tab in the current browser will trigger a tablist
 * update.
 * If the currently selected page is "this-firefox", we should wait for the
 * the corresponding REQUEST_TABS_SUCCESS that will be triggered by the change.
 *
 * @param {Browser} browser
 *        The browser instance to update.
 * @param {XULTab} tab
 *        The tab to select.
 * @param {Object} store
 *        The about:debugging redux store.
 */
async function updateSelectedTab(browser, tab, store) {
  info("Update the selected tab");

  const { runtimes, ui } = store.getState();
  const isOnThisFirefox =
    runtimes.selectedRuntimeId === "this-firefox" &&
    ui.selectedPage === "runtime";

  // A tabs request will only be issued if we are on this-firefox.
  const onTabsSuccess = isOnThisFirefox
    ? waitForDispatch(store, "REQUEST_TABS_SUCCESS")
    : null;

  // Update the selected tab.
  browser.selectedTab = tab;

  if (onTabsSuccess) {
    info("Wait for the tablist update after updating the selected tab");
    await onTabsSuccess;
  }
}

/**
 * Synthesizes key input inside the DebugTargetInfo's URL component.
 *
 * @param {DevToolsToolbox} toolbox
 *        The DevToolsToolbox debugging the target.
 * @param {HTMLElement} inputEl
 *        The <input> element to submit the URL with.
 * @param {String}  url
 *        The URL to navigate to.
 */
async function synthesizeUrlKeyInput(toolbox, inputEl, url) {
  const { devtoolsDocument, devtoolsWindow } = toolbox;
  info("Wait for URL input to be focused.");
  const onInputFocused = waitUntil(
    () => devtoolsDocument.activeElement === inputEl
  );
  inputEl.focus();
  await onInputFocused;

  info("Synthesize entering URL into text field");
  const onInputChange = waitUntil(() => inputEl.value === url);
  for (const key of url.split("")) {
    EventUtils.synthesizeKey(key, {}, devtoolsWindow);
  }
  await onInputChange;

  info("Submit URL to navigate to");
  EventUtils.synthesizeKey("KEY_Enter");
}

/**
 * Click on a given add-on widget button so that its browser actor is fired.
 * Typically a popup would open, or a listener would be called in the background page.
 *
 * @param {String} addonId
 *        The ID of the add-on to click on.
 */
function clickOnAddonWidget(addonId) {
  // Devtools are in another window and may have the focus.
  // Ensure focusing the browser window when clicking on the widget.
  const focusedWin = Services.focus.focusedWindow;
  if (focusedWin != window) {
    window.focus();
  }
  // Find the browserAction button that will show the webextension popup.
  const widgetId = addonId.toLowerCase().replace(/[^a-z0-9_-]/g, "_");
  const browserActionId = widgetId + "-browser-action";
  const browserActionEl = window.document.getElementById(browserActionId);
  ok(browserActionEl, "Got the browserAction button from the browser UI");

  info("Show the web extension popup");
  browserActionEl
    .querySelector(".unified-extensions-item-action-button")
    .click();
}

// Create basic addon data as the DevToolsClient would return it.
function createAddonData({
  id,
  name,
  isSystem = false,
  hidden = false,
  temporary = false,
}) {
  return {
    actor: `actorid-${id}`,
    hidden,
    iconURL: `moz-extension://${id}/icon-url.png`,
    id,
    manifestURL: `moz-extension://${id}/manifest-url.json`,
    name,
    isSystem,
    temporarilyInstalled: temporary,
    debuggable: true,
  };
}

async function connectToLocalFirefox({ runtimeId, runtimeName, deviceName }) {
  // This is a client to the current Firefox.
  const clientWrapper = await createLocalClientWrapper();

  // enable USB devices mocks
  const mocks = new Mocks();
  const usbClient = mocks.createUSBRuntime(runtimeId, {
    deviceName,
    name: runtimeName,
    clientWrapper,
  });

  // Wrap a disconnect helper for convenience for the caller.
  const disconnect = doc =>
    disconnectFromLocalFirefox({
      doc,
      runtimeId,
      deviceName,
      mocks,
    });

  return { disconnect, mocks, usbClient };
}
/* exported connectToLocalFirefox */

async function disconnectFromLocalFirefox({
  doc,
  mocks,
  runtimeId,
  deviceName,
}) {
  info("Remove USB runtime");
  mocks.removeUSBRuntime(runtimeId);
  mocks.emitUSBUpdate();
  await waitUntilUsbDeviceIsUnplugged(deviceName, doc);
}
/* exported disconnectFromLocalFirefox */
