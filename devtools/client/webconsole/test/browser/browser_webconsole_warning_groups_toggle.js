/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/ */

// Test that filtering the console output when there are warning groups works as expected.

"use strict";
requestLongerTimeout(2);

const { PrefObserver } = require("resource://devtools/client/shared/prefs.js");

const TEST_FILE =
  "browser/devtools/client/webconsole/test/browser/test-warning-groups.html";
const TEST_URI = "https://example.org/" + TEST_FILE;

const TRACKER_URL = "https://tracking.example.com/";
const IMG_FILE =
  "browser/devtools/client/webconsole/test/browser/test-image.png";
const CONTENT_BLOCKED_BY_ETP_URL = TRACKER_URL + IMG_FILE;
const WARNING_GROUP_PREF = "devtools.webconsole.groupSimilarMessages";

const { UrlClassifierTestUtils } = ChromeUtils.importESModule(
  "resource://testing-common/UrlClassifierTestUtils.sys.mjs"
);
UrlClassifierTestUtils.addTestTrackers();
registerCleanupFunction(function () {
  UrlClassifierTestUtils.cleanupTestTrackers();
});

pushPref("privacy.trackingprotection.enabled", true);

const ENHANCED_TRACKING_PROTECTION_GROUP_LABEL =
  "The resource at “<URL>” was blocked because Enhanced Tracking Protection is enabled.";

add_task(async function testContentBlockingMessage() {
  // Enable persist log
  await pushPref("devtools.webconsole.persistlog", true);

  // Start with the warningGroup pref set to false.
  await pushPref(WARNING_GROUP_PREF, false);

  const hud = await openNewTabAndConsole(TEST_URI);

  info("Log a few tracking protection messages and simple ones");
  let onContentBlockedByETPWarningMessage = waitForMessageByType(
    hud,
    `${CONTENT_BLOCKED_BY_ETP_URL}?1`,
    ".warn"
  );
  emitEnhancedTrackingProtectionMessage(hud);
  await onContentBlockedByETPWarningMessage;
  await logString(hud, "simple message 1");

  onContentBlockedByETPWarningMessage = waitForMessageByType(
    hud,
    `${CONTENT_BLOCKED_BY_ETP_URL}?2`,
    ".warn"
  );
  emitEnhancedTrackingProtectionMessage(hud);
  await onContentBlockedByETPWarningMessage;

  onContentBlockedByETPWarningMessage = waitForMessageByType(
    hud,
    `${CONTENT_BLOCKED_BY_ETP_URL}?3`,
    ".warn"
  );
  emitEnhancedTrackingProtectionMessage(hud);
  await onContentBlockedByETPWarningMessage;

  await checkConsoleOutputForWarningGroup(hud, [
    `${CONTENT_BLOCKED_BY_ETP_URL}?1`,
    `simple message 1`,
    `${CONTENT_BLOCKED_BY_ETP_URL}?2`,
    `${CONTENT_BLOCKED_BY_ETP_URL}?3`,
  ]);

  info("Enable the warningGroup feature pref and check warnings were grouped");
  await toggleWarningGroupPreference(hud);
  let warningGroupMessage1 = await waitFor(() =>
    findWarningMessage(hud, ENHANCED_TRACKING_PROTECTION_GROUP_LABEL)
  );
  is(
    warningGroupMessage1.querySelector(".warning-group-badge").textContent,
    "3",
    "The badge has the expected text"
  );

  await checkConsoleOutputForWarningGroup(hud, [
    `▶︎⚠ ${ENHANCED_TRACKING_PROTECTION_GROUP_LABEL}`,
    `simple message 1`,
  ]);

  info("Add a new warning message and check it's placed in the closed group");
  emitEnhancedTrackingProtectionMessage(hud);
  await waitForBadgeNumber(warningGroupMessage1, "4");

  info(
    "Re-enable the warningGroup feature pref and check warnings are displayed"
  );
  await toggleWarningGroupPreference(hud);
  await waitFor(() =>
    findWarningMessage(hud, `${CONTENT_BLOCKED_BY_ETP_URL}?4`)
  );

  // Warning messages are displayed at the expected positions.
  await checkConsoleOutputForWarningGroup(hud, [
    `${CONTENT_BLOCKED_BY_ETP_URL}?1`,
    `simple message 1`,
    `${CONTENT_BLOCKED_BY_ETP_URL}?2`,
    `${CONTENT_BLOCKED_BY_ETP_URL}?3`,
    `${CONTENT_BLOCKED_BY_ETP_URL}?4`,
  ]);

  info("Re-disable the warningGroup feature pref");
  await toggleWarningGroupPreference(hud);
  console.log("toggle successful");
  warningGroupMessage1 = await waitFor(() =>
    findWarningMessage(hud, ENHANCED_TRACKING_PROTECTION_GROUP_LABEL)
  );

  await checkConsoleOutputForWarningGroup(hud, [
    `▶︎⚠ ${ENHANCED_TRACKING_PROTECTION_GROUP_LABEL}`,
    `simple message 1`,
  ]);

  info("Expand the warning group");
  warningGroupMessage1.querySelector(".arrow").click();
  await waitFor(() => findWarningMessage(hud, CONTENT_BLOCKED_BY_ETP_URL));

  await checkConsoleOutputForWarningGroup(hud, [
    `▼︎⚠ ${ENHANCED_TRACKING_PROTECTION_GROUP_LABEL}`,
    `| ${CONTENT_BLOCKED_BY_ETP_URL}?1`,
    `| ${CONTENT_BLOCKED_BY_ETP_URL}?2`,
    `| ${CONTENT_BLOCKED_BY_ETP_URL}?3`,
    `| ${CONTENT_BLOCKED_BY_ETP_URL}?4`,
    `simple message 1`,
  ]);

  info("Reload the page and wait for it to be ready");
  await reloadPage();

  // Wait for the navigation message to be displayed.
  await waitFor(() =>
    findMessageByType(hud, "Navigated to", ".navigationMarker")
  );

  info("Disable the warningGroup feature pref again");
  await toggleWarningGroupPreference(hud);

  info("Add one warning message and one simple message");
  await waitFor(() =>
    findWarningMessage(hud, `${CONTENT_BLOCKED_BY_ETP_URL}?4`)
  );
  onContentBlockedByETPWarningMessage = waitForMessageByType(
    hud,
    CONTENT_BLOCKED_BY_ETP_URL,
    ".warn"
  );
  emitEnhancedTrackingProtectionMessage(hud);
  await onContentBlockedByETPWarningMessage;
  await logString(hud, "simple message 2");

  // nothing is grouped.
  await checkConsoleOutputForWarningGroup(hud, [
    `${CONTENT_BLOCKED_BY_ETP_URL}?1`,
    `simple message 1`,
    `${CONTENT_BLOCKED_BY_ETP_URL}?2`,
    `${CONTENT_BLOCKED_BY_ETP_URL}?3`,
    `${CONTENT_BLOCKED_BY_ETP_URL}?4`,
    `Navigated to`,
    `${CONTENT_BLOCKED_BY_ETP_URL}?5`,
    `simple message 2`,
  ]);

  info(
    "Enable the warningGroup feature pref to check that the group is still expanded"
  );
  await toggleWarningGroupPreference(hud);
  await waitFor(() =>
    findWarningMessage(hud, ENHANCED_TRACKING_PROTECTION_GROUP_LABEL)
  );

  await checkConsoleOutputForWarningGroup(hud, [
    `▼︎⚠ ${ENHANCED_TRACKING_PROTECTION_GROUP_LABEL}`,
    `| ${CONTENT_BLOCKED_BY_ETP_URL}?1`,
    `| ${CONTENT_BLOCKED_BY_ETP_URL}?2`,
    `| ${CONTENT_BLOCKED_BY_ETP_URL}?3`,
    `| ${CONTENT_BLOCKED_BY_ETP_URL}?4`,
    `simple message 1`,
    `Navigated to`,
    `| ${CONTENT_BLOCKED_BY_ETP_URL}?5`,
    `simple message 2`,
  ]);

  info(
    "Add a second warning and check it's placed in the second, closed, group"
  );
  const onContentBlockedByETPWarningGroupMessage = waitForMessageByType(
    hud,
    ENHANCED_TRACKING_PROTECTION_GROUP_LABEL,
    ".warn"
  );
  emitEnhancedTrackingProtectionMessage(hud);
  const warningGroupMessage2 = (await onContentBlockedByETPWarningGroupMessage)
    .node;
  await waitForBadgeNumber(warningGroupMessage2, "2");

  await checkConsoleOutputForWarningGroup(hud, [
    `▼︎⚠ ${ENHANCED_TRACKING_PROTECTION_GROUP_LABEL}`,
    `| ${CONTENT_BLOCKED_BY_ETP_URL}?1`,
    `| ${CONTENT_BLOCKED_BY_ETP_URL}?2`,
    `| ${CONTENT_BLOCKED_BY_ETP_URL}?3`,
    `| ${CONTENT_BLOCKED_BY_ETP_URL}?4`,
    `simple message 1`,
    `Navigated to`,
    `▶︎⚠ ${ENHANCED_TRACKING_PROTECTION_GROUP_LABEL}`,
    `simple message 2`,
  ]);

  info(
    "Disable the warningGroup pref and check all warning messages are visible"
  );
  await toggleWarningGroupPreference(hud);
  await waitFor(() =>
    findWarningMessage(hud, `${CONTENT_BLOCKED_BY_ETP_URL}?6`)
  );

  await checkConsoleOutputForWarningGroup(hud, [
    `${CONTENT_BLOCKED_BY_ETP_URL}?1`,
    `simple message 1`,
    `${CONTENT_BLOCKED_BY_ETP_URL}?2`,
    `${CONTENT_BLOCKED_BY_ETP_URL}?3`,
    `${CONTENT_BLOCKED_BY_ETP_URL}?4`,
    `Navigated to`,
    `${CONTENT_BLOCKED_BY_ETP_URL}?5`,
    `simple message 2`,
    `${CONTENT_BLOCKED_BY_ETP_URL}?6`,
  ]);

  // Clean the pref for the next tests.
  Services.prefs.clearUserPref(WARNING_GROUP_PREF);
});

let cpt = 0;
/**
 * Emit an Enhanced Tracking Protection message. This is done by loading an image from an origin
 * tagged as tracker. The image is loaded with a incremented counter query parameter
 * each time so we can get the warning message.
 */
function emitEnhancedTrackingProtectionMessage() {
  const url = `${CONTENT_BLOCKED_BY_ETP_URL}?${++cpt}`;
  SpecialPowers.spawn(gBrowser.selectedBrowser, [url], function (innerURL) {
    content.wrappedJSObject.loadImage(innerURL);
  });
}

/**
 * Log a string from the content page.
 *
 * @param {WebConsole} hud
 * @param {String} str
 */
function logString(hud, str) {
  const onMessage = waitForMessageByType(hud, str, ".console-api");
  SpecialPowers.spawn(gBrowser.selectedBrowser, [str], function (arg) {
    content.console.log(arg);
  });
  return onMessage;
}

function waitForBadgeNumber(message, expectedNumber) {
  return waitFor(
    () =>
      message.querySelector(".warning-group-badge").textContent ==
      expectedNumber
  );
}

async function toggleWarningGroupPreference(hud) {
  info("Open the settings panel");
  const observer = new PrefObserver("");

  info("Change warning preference");
  const prefChanged = observer.once(WARNING_GROUP_PREF, () => {});

  await toggleConsoleSetting(
    hud,
    ".webconsole-console-settings-menu-item-warning-groups"
  );

  await prefChanged;
  observer.destroy();
}
