/* Any copyright is dedicated to the Public Domain.
 * http://creativecommons.org/publicdomain/zero/1.0/
 */

const { AddonTestUtils } = ChromeUtils.importESModule(
  "resource://testing-common/AddonTestUtils.sys.mjs"
);
const { ExtensionPermissions } = ChromeUtils.importESModule(
  "resource://gre/modules/ExtensionPermissions.sys.mjs"
);

const lazy = {};
ChromeUtils.defineLazyGetter(lazy, "l10n", function () {
  return new Localization(
    ["browser/addonNotifications.ftl", "branding/brand.ftl"],
    true
  );
});

const SECUREROOT =
  "https://example.com/browser/toolkit/mozapps/extensions/test/xpinstall/";
const PROGRESS_NOTIFICATION = "addon-progress";

const CHROMEROOT = extractChromeRoot(gTestPath);

AddonTestUtils.initMochitest(this);

let needsCleanupBlocklist = true;

const cleanupBlocklist = async () => {
  if (!needsCleanupBlocklist) {
    return;
  }
  await AddonTestUtils.loadBlocklistRawData({
    extensionsMLBF: [
      {
        stash: { blocked: [], unblocked: [] },
        stash_time: 0,
      },
    ],
  });
  needsCleanupBlocklist = false;
};

function waitForTick() {
  return new Promise(resolve => executeSoon(resolve));
}

function getObserverTopic(aNotificationId) {
  let topic = aNotificationId;
  if (topic == "xpinstall-disabled") {
    topic = "addon-install-disabled";
  } else if (topic == "addon-progress") {
    topic = "addon-install-started";
  } else if (topic == "addon-installed") {
    topic = "webextension-install-notify";
  } else if (topic == "addon-install-failed-blocklist") {
    topic = "addon-install-failed";
  }
  return topic;
}

async function waitForProgressNotification(
  aPanelOpen = false,
  aExpectedCount = 1,
  wantDisabled = true,
  expectedAnchorID = "unified-extensions-button",
  win = window
) {
  let notificationId = PROGRESS_NOTIFICATION;
  info("Waiting for " + notificationId + " notification");

  let topic = getObserverTopic(notificationId);

  let observerPromise = new Promise(resolve => {
    Services.obs.addObserver(function observer(aSubject, aTopic) {
      // Ignore the progress notification unless that is the notification we want
      if (
        notificationId != PROGRESS_NOTIFICATION &&
        aTopic == getObserverTopic(PROGRESS_NOTIFICATION)
      ) {
        return;
      }
      Services.obs.removeObserver(observer, topic);
      resolve();
    }, topic);
  });

  let panelEventPromise;
  if (aPanelOpen) {
    panelEventPromise = Promise.resolve();
  } else {
    panelEventPromise = new Promise(resolve => {
      win.PopupNotifications.panel.addEventListener(
        "popupshowing",
        function () {
          resolve();
        },
        { once: true }
      );
    });
  }

  await observerPromise;
  await panelEventPromise;
  await waitForTick();

  info("Saw a notification");
  ok(win.PopupNotifications.isPanelOpen, "Panel should be open");
  is(
    win.PopupNotifications.panel.childNodes.length,
    aExpectedCount,
    "Should be the right number of notifications"
  );
  if (win.PopupNotifications.panel.childNodes.length) {
    let nodes = Array.from(win.PopupNotifications.panel.childNodes);
    let notification = nodes.find(
      n => n.id == notificationId + "-notification"
    );
    ok(notification, `Should have seen the right notification`);
    is(
      notification.button.hasAttribute("disabled"),
      wantDisabled,
      "The install button should be disabled?"
    );

    let n = win.PopupNotifications.getNotification(PROGRESS_NOTIFICATION);
    is(
      n?.anchorElement?.id || n?.anchorElement?.parentElement?.id,
      expectedAnchorID,
      "expected the right anchor ID"
    );
  }

  return win.PopupNotifications.panel;
}

function testInstallDialogIncognitoCheckbox(
  installDialog,
  {
    toggleIncognito = false,
    incognitoChecked = false,
    incognitoHidden = false,
  } = {}
) {
  // If the incognito toggle is expected to be in the first install dialog
  // verify that it is found and visible and toggle it.
  const privateBrowsingCheckbox = installDialog.querySelector(
    ".webext-perm-privatebrowsing moz-checkbox"
  );
  is(
    !privateBrowsingCheckbox,
    incognitoHidden,
    incognitoHidden
      ? "Expect private browsing checkbox to NOT be found in the first dialog"
      : "Expect private browsing checkbox to be found in the first install dialog"
  );

  if (!incognitoHidden) {
    if (privateBrowsingCheckbox) {
      ok(
        BrowserTestUtils.isVisible(privateBrowsingCheckbox),
        "private browsing checkbox should be visible"
      );
    }
    // SUMO link should always be visible if the incognito checkbox is expected to be
    // shown too (even when there are no other permissions being granted as part of the
    // same install dialog).
    let permsLearnMore = installDialog.querySelector(
      ".popup-notification-learnmore-link"
    );
    is(
      permsLearnMore.href,
      Services.urlFormatter.formatURLPref("app.support.baseURL") +
        "extension-permissions",
      "Learn more link has desired URL"
    );
    ok(
      BrowserTestUtils.isVisible(permsLearnMore),
      "SUMO link expected to be visible"
    );
  } else {
    if (incognitoChecked) {
      throw new Error(
        "incognitoChecked can't be set to true when incognitoHidden is set to true"
      );
    }
    if (toggleIncognito) {
      throw new Error(
        "toggleIncognito can't be set to true when incognitoHidden is set to true"
      );
    }
    return;
  }

  is(
    privateBrowsingCheckbox.checked,
    incognitoChecked,
    incognitoChecked
      ? "Expect private browsing checkbox to be checked"
      : "Expect private browsing checkbox to NOT be checked"
  );

  if (toggleIncognito === true) {
    privateBrowsingCheckbox.click();
  }
}

function acceptAppMenuNotificationWhenShown(
  id,
  extensionId,
  { dismiss = false, global = window } = {}
) {
  const { AppMenuNotifications, PanelUI, document } = global;
  return new Promise(resolve => {
    let permissionChangePromise = null;
    function appMenuPopupHidden() {
      PanelUI.panel.removeEventListener("popuphidden", appMenuPopupHidden);
      ok(
        !PanelUI.menuButton.hasAttribute("badge-status"),
        "badge is not set after addon-installed"
      );
      resolve(permissionChangePromise);
    }
    function appMenuPopupShown() {
      PanelUI.panel.removeEventListener("popupshown", appMenuPopupShown);
      PanelUI.menuButton.click();
    }
    function popupshown() {
      let notification = AppMenuNotifications.activeNotification;
      if (!notification) {
        return;
      }

      is(notification.id, id, `${id} notification shown`);
      ok(PanelUI.isNotificationPanelOpen, "notification panel open");

      PanelUI.notificationPanel.removeEventListener("popupshown", popupshown);

      if (dismiss) {
        // Dismiss the panel by clicking on the appMenu button.
        PanelUI.panel.addEventListener("popupshown", appMenuPopupShown);
        PanelUI.panel.addEventListener("popuphidden", appMenuPopupHidden);
        PanelUI.menuButton.click();
        return;
      }

      // Dismiss the panel by clicking the primary button.
      let popupnotificationID = PanelUI._getPopupId(notification);
      let popupnotification = document.getElementById(popupnotificationID);

      popupnotification.button.click();
      resolve(permissionChangePromise);
    }
    PanelUI.notificationPanel.addEventListener("popupshown", popupshown);
  });
}

async function waitForNotification(
  aId,
  aExpectedCount = 1,
  expectedAnchorID = "unified-extensions-button",
  win = window
) {
  info("Waiting for " + aId + " notification");

  let topic = getObserverTopic(aId);

  let observerPromise;
  if (aId !== "addon-webext-permissions") {
    observerPromise = new Promise(resolve => {
      Services.obs.addObserver(function observer(aSubject, aTopic) {
        // Ignore the progress notification unless that is the notification we want
        if (
          aId != PROGRESS_NOTIFICATION &&
          aTopic == getObserverTopic(PROGRESS_NOTIFICATION)
        ) {
          return;
        }
        Services.obs.removeObserver(observer, topic);
        resolve();
      }, topic);
    });
  }

  let panelEventPromise = new Promise(resolve => {
    win.PopupNotifications.panel.addEventListener(
      "PanelUpdated",
      function eventListener(e) {
        // Skip notifications that are not the one that we are supposed to be looking for
        if (!e.detail.includes(aId)) {
          return;
        }
        win.PopupNotifications.panel.removeEventListener(
          "PanelUpdated",
          eventListener
        );
        resolve();
      }
    );
  });

  await observerPromise;
  await panelEventPromise;
  await waitForTick();

  info("Saw a " + aId + " notification");
  ok(win.PopupNotifications.isPanelOpen, "Panel should be open");
  is(
    win.PopupNotifications.panel.childNodes.length,
    aExpectedCount,
    "Should be the right number of notifications"
  );
  if (win.PopupNotifications.panel.childNodes.length) {
    let nodes = Array.from(win.PopupNotifications.panel.childNodes);
    let notification = nodes.find(n => n.id == aId + "-notification");
    ok(notification, "Should have seen the " + aId + " notification");

    let n = win.PopupNotifications.getNotification(aId);
    is(
      n?.anchorElement?.id || n?.anchorElement?.parentElement?.id,
      expectedAnchorID,
      "expected the right anchor ID"
    );
  }
  await SimpleTest.promiseFocus(win.PopupNotifications.window);

  return win.PopupNotifications.panel;
}

function waitForNotificationClose(win = window) {
  if (!win.PopupNotifications.isPanelOpen) {
    return Promise.resolve();
  }
  return new Promise(resolve => {
    info("Waiting for notification to close");
    win.PopupNotifications.panel.addEventListener(
      "popuphidden",
      function () {
        resolve();
      },
      { once: true }
    );
  });
}

async function waitForInstallDialog(id = "addon-webext-permissions") {
  let panel = await waitForNotification(id);
  // NOTE: the panel may intermittently still be in the "showing" state, and
  // so we explicitly await for the state to become "open" before proceeding
  // with asserting the visibility of the elements we expected to be in the
  // panel.
  if (panel.state === "showing") {
    await TestUtils.waitForCondition(
      () => panel.state === "open",
      `Wait for ${id} panel state to become open`
    );
    is(panel.state, "open", "Panel.state should be open");
  }

  return panel.childNodes[0];
}

function removeTabAndWaitForNotificationClose() {
  let closePromise = waitForNotificationClose();
  BrowserTestUtils.removeTab(gBrowser.selectedTab);
  return closePromise;
}

function acceptInstallDialog(installDialog) {
  installDialog.button.click();
}

async function waitForSingleNotification() {
  while (PopupNotifications.panel.childNodes.length != 1) {
    await new Promise(resolve => executeSoon(resolve));

    info("Waiting for single notification");
    // Notification should never close while we wait
    ok(PopupNotifications.isPanelOpen, "Notification should still be open");
  }
}

function setupRedirect(aSettings) {
  var url =
    "https://example.com/browser/toolkit/mozapps/extensions/test/xpinstall/redirect.sjs?mode=setup";
  for (var name in aSettings) {
    url += "&" + name + "=" + aSettings[name];
  }

  var req = new XMLHttpRequest();
  req.open("GET", url, false);
  req.send(null);
}

async function installAddonWithPrivateBrowsingAccess(xpiUrl, addonId) {
  // Note: this used to be effective, but changed in bug 1974419. Since then,
  // the private browsing permission is only read from the database if an
  // add-on was already installed at the time of prompting.
  await ExtensionPermissions.add(addonId, {
    permissions: ["internal:privateBrowsingAllowed"],
    origins: [],
  });

  let progressPromise = waitForProgressNotification();
  let dialogPromise = waitForInstallDialog();

  gBrowser.selectedTab = BrowserTestUtils.addTab(gBrowser, "about:blank");
  await BrowserTestUtils.browserLoaded(gBrowser.selectedBrowser);
  gURLBar.value = xpiUrl;
  gURLBar.focus();
  EventUtils.synthesizeKey("KEY_Enter");

  await progressPromise;
  let installDialog = await dialogPromise;

  testInstallDialogIncognitoCheckbox(installDialog, {
    incognitoHidden: false,
    // Note: before the change in bug 1974419, this expectation was true.
    incognitoChecked: false,
    toggleIncognito: true,
  });
  let notificationPromise = acceptAppMenuNotificationWhenShown(
    "addon-installed",
    addonId
  );
  let readyPromise = AddonTestUtils.promiseWebExtensionStartup(addonId);
  installDialog.button.click();
  await notificationPromise;
  let installs = await AddonManager.getAllInstalls();
  is(installs.length, 0, "Should be no pending installs");

  await readyPromise;

  let policy = WebExtensionPolicy.getByID(addonId);
  ok(policy.privateBrowsingAllowed, "private browsing permission granted");

  await removeTabAndWaitForNotificationClose();
}

var TESTS = [
  async function test_disabledInstall() {
    await SpecialPowers.pushPrefEnv({
      set: [["xpinstall.enabled", false]],
    });
    let notificationPromise = waitForNotification("xpinstall-disabled");
    let triggers = encodeURIComponent(
      JSON.stringify({
        XPI: "amosigned.xpi",
      })
    );
    BrowserTestUtils.openNewForegroundTab(
      gBrowser,
      TESTROOT + "installtrigger.html?" + triggers
    );
    let panel = await notificationPromise;

    let notification = panel.childNodes[0];
    is(
      notification.button.label,
      "Enable",
      "Should have seen the right button"
    );
    is(
      notification.getAttribute("label"),
      "Software installation is currently disabled. Click Enable and try again.",
      "notification label is correct"
    );

    let closePromise = waitForNotificationClose();
    // Click on Enable
    EventUtils.synthesizeMouseAtCenter(notification.button, {});
    await closePromise;

    try {
      ok(
        Services.prefs.getBoolPref("xpinstall.enabled"),
        "Installation should be enabled"
      );
    } catch (e) {
      ok(false, "xpinstall.enabled should be set");
    }

    BrowserTestUtils.removeTab(gBrowser.selectedTab);
    let installs = await AddonManager.getAllInstalls();
    is(installs.length, 0, "Shouldn't be any pending installs");
    await SpecialPowers.popPrefEnv();
  },

  async function test_blockedInstall() {
    await SpecialPowers.pushPrefEnv({
      set: [["extensions.postDownloadThirdPartyPrompt", false]],
    });

    let notificationPromise = waitForNotification("addon-install-blocked");
    let triggers = encodeURIComponent(
      JSON.stringify({
        XPI: "amosigned.xpi",
      })
    );
    BrowserTestUtils.openNewForegroundTab(
      gBrowser,
      TESTROOT + "installtrigger.html?" + triggers
    );
    let panel = await notificationPromise;

    let notification = panel.childNodes[0];
    is(
      notification.button.label,
      "Continue to Installation",
      "Should have seen the right button"
    );
    is(
      notification
        .querySelector("#addon-install-blocked-info")
        .getAttribute("href"),
      Services.urlFormatter.formatURLPref("app.support.baseURL") +
        "unlisted-extensions-risks",
      "Got the expected SUMO page as a learn more link in the addon-install-blocked panel"
    );
    let message = panel.ownerDocument.getElementById(
      "addon-install-blocked-message"
    );
    is(
      message.textContent,
      "You are attempting to install an add-on from example.com. Make sure you trust this site before continuing.",
      "Should have seen the right message"
    );

    let dialogPromise = waitForInstallDialog();
    // Click on Allow
    EventUtils.synthesizeMouse(notification.button, 20, 10, {});

    // Notification should have changed to progress notification
    ok(PopupNotifications.isPanelOpen, "Notification should still be open");
    notification = panel.childNodes[0];
    is(
      notification.id,
      "addon-progress-notification",
      "Should have seen the progress notification"
    );

    let installDialog = await dialogPromise;

    notificationPromise = acceptAppMenuNotificationWhenShown(
      "addon-installed",
      "amosigned-xpi@tests.mozilla.org"
    );

    installDialog.button.click();
    await notificationPromise;

    let installs = await AddonManager.getAllInstalls();
    is(installs.length, 0, "Should be no pending installs");

    let addon = await AddonManager.getAddonByID(
      "amosigned-xpi@tests.mozilla.org"
    );
    await addon.uninstall();

    await BrowserTestUtils.removeTab(gBrowser.selectedTab);
    await SpecialPowers.popPrefEnv();
  },

  async function test_blockedPostDownload() {
    await SpecialPowers.pushPrefEnv({
      set: [["extensions.postDownloadThirdPartyPrompt", true]],
    });

    let notificationPromise = waitForNotification("addon-install-blocked");
    let triggers = encodeURIComponent(
      JSON.stringify({
        XPI: "amosigned.xpi",
      })
    );
    BrowserTestUtils.openNewForegroundTab(
      gBrowser,
      TESTROOT + "installtrigger.html?" + triggers
    );
    let panel = await notificationPromise;

    let notification = panel.childNodes[0];
    is(
      notification.button.label,
      "Continue to Installation",
      "Should have seen the right button"
    );
    let message = panel.ownerDocument.getElementById(
      "addon-install-blocked-message"
    );
    is(
      message.textContent,
      "You are attempting to install an add-on from example.com. Make sure you trust this site before continuing.",
      "Should have seen the right message"
    );

    let dialogPromise = waitForInstallDialog();
    // Click on Allow
    EventUtils.synthesizeMouse(notification.button, 20, 10, {});

    let installDialog = await dialogPromise;

    notificationPromise = acceptAppMenuNotificationWhenShown(
      "addon-installed",
      "amosigned-xpi@tests.mozilla.org"
    );

    installDialog.button.click();
    await notificationPromise;

    let installs = await AddonManager.getAllInstalls();
    is(installs.length, 0, "Should be no pending installs");

    let addon = await AddonManager.getAddonByID(
      "amosigned-xpi@tests.mozilla.org"
    );
    await addon.uninstall();

    await BrowserTestUtils.removeTab(gBrowser.selectedTab);
    await SpecialPowers.popPrefEnv();
  },

  async function test_recommendedPostDownload() {
    await SpecialPowers.pushPrefEnv({
      set: [["extensions.postDownloadThirdPartyPrompt", true]],
    });

    let triggers = encodeURIComponent(
      JSON.stringify({
        XPI: "recommended.xpi",
      })
    );
    BrowserTestUtils.openNewForegroundTab(
      gBrowser,
      TESTROOT + "installtrigger.html?" + triggers
    );

    let installDialog = await waitForInstallDialog();

    let notificationPromise = acceptAppMenuNotificationWhenShown(
      "addon-installed",
      "{811d77f1-f306-4187-9251-b4ff99bad60b}"
    );

    installDialog.button.click();
    await notificationPromise;

    let installs = await AddonManager.getAllInstalls();
    is(installs.length, 0, "Should be no pending installs");

    let addon = await AddonManager.getAddonByID(
      "{811d77f1-f306-4187-9251-b4ff99bad60b}"
    );
    await addon.uninstall();

    await BrowserTestUtils.removeTab(gBrowser.selectedTab);
    await SpecialPowers.popPrefEnv();
  },

  async function test_priviledgedNo3rdPartyPrompt() {
    await SpecialPowers.pushPrefEnv({
      set: [["extensions.postDownloadThirdPartyPrompt", true]],
    });
    AddonManager.checkUpdateSecurity = false;
    registerCleanupFunction(() => {
      AddonManager.checkUpdateSecurity = true;
    });

    let triggers = encodeURIComponent(
      JSON.stringify({
        XPI: "privileged.xpi",
      })
    );

    let installDialogPromise = waitForInstallDialog();

    try {
      // Prevent install to fail due to privileged.xpi version using
      // an addon version that hits a manifest warning (see PRIV_ADDON_VERSION).
      // TODO(Bug 1824240): remove this once privileged.xpi can be resigned with a
      // version format that does not hit a manifest warning.
      ExtensionTestUtils.failOnSchemaWarnings(false);
      let tab = await BrowserTestUtils.openNewForegroundTab(
        gBrowser,
        TESTROOT + "installtrigger.html?" + triggers
      );

      let notificationPromise = acceptAppMenuNotificationWhenShown(
        "addon-installed",
        "test@tests.mozilla.org"
      );

      const installDialog = await installDialogPromise;

      testInstallDialogIncognitoCheckbox(installDialog, {
        incognitoHidden: true,
      });

      installDialog.button.click();
      await notificationPromise;

      let installs = await AddonManager.getAllInstalls();
      is(installs.length, 0, "Should be no pending installs");

      let addon = await AddonManager.getAddonByID("test@tests.mozilla.org");
      await addon.uninstall();

      await BrowserTestUtils.removeTab(tab);
    } finally {
      ExtensionTestUtils.failOnSchemaWarnings(true);
    }

    await SpecialPowers.popPrefEnv();
    AddonManager.checkUpdateSecurity = true;
  },

  async function test_permaBlockInstall() {
    let notificationPromise = waitForNotification("addon-install-blocked");
    let triggers = encodeURIComponent(
      JSON.stringify({
        XPI: "amosigned.xpi",
      })
    );
    let target = TESTROOT + "installtrigger.html?" + triggers;

    BrowserTestUtils.openNewForegroundTab(gBrowser, target);
    let notification = (await notificationPromise).firstElementChild;
    let neverAllowBtn = notification.menupopup.firstElementChild;

    neverAllowBtn.click();

    await TestUtils.waitForCondition(
      () => !PopupNotifications.isPanelOpen,
      "Waiting for notification to close"
    );

    let installs = await AddonManager.getAllInstalls();
    is(installs.length, 0, "Should be no pending installs");

    let installPerm = PermissionTestUtils.testPermission(
      gBrowser.currentURI,
      "install"
    );
    is(
      installPerm,
      Ci.nsIPermissionManager.DENY_ACTION,
      "Addon installation should be blocked for site"
    );

    await BrowserTestUtils.removeTab(gBrowser.selectedTab);

    PermissionTestUtils.remove(target, "install");
  },

  async function test_permaBlockedInstallNoPrompt() {
    let triggers = encodeURIComponent(
      JSON.stringify({
        XPI: "amosigned.xpi",
      })
    );
    let target = TESTROOT + "installtrigger.html?" + triggers;

    PermissionTestUtils.add(target, "install", Services.perms.DENY_ACTION);
    await BrowserTestUtils.openNewForegroundTab(gBrowser, target);

    let panelOpened;
    try {
      panelOpened = await TestUtils.waitForCondition(
        () => PopupNotifications.isPanelOpen,
        100,
        10
      );
    } catch (ex) {
      panelOpened = false;
    }
    is(panelOpened, false, "Addon prompt should not open");

    let installs = await AddonManager.getAllInstalls();
    is(installs.length, 0, "Should be no pending installs");

    await BrowserTestUtils.removeTab(gBrowser.selectedTab);

    PermissionTestUtils.remove(target, "install");
  },

  async function test_whitelistedInstall() {
    let originalTab = gBrowser.selectedTab;
    let tab;
    gBrowser.selectedTab = originalTab;
    PermissionTestUtils.add(
      "http://example.com/",
      "install",
      Services.perms.ALLOW_ACTION
    );

    let progressPromise = waitForProgressNotification();
    let dialogPromise = waitForInstallDialog();
    let triggers = encodeURIComponent(
      JSON.stringify({
        XPI: "amosigned.xpi",
      })
    );
    BrowserTestUtils.openNewForegroundTab(
      gBrowser,
      TESTROOT + "installtrigger.html?" + triggers
    ).then(newTab => (tab = newTab));
    await progressPromise;
    let installDialog = await dialogPromise;
    await BrowserTestUtils.waitForCondition(
      () => !!tab,
      "tab should be present"
    );

    is(
      gBrowser.selectedTab,
      tab,
      "tab selected in response to the addon-install-confirmation notification"
    );

    let notificationPromise = acceptAppMenuNotificationWhenShown(
      "addon-installed",
      "amosigned-xpi@tests.mozilla.org",
      { dismiss: true }
    );
    let readyPromise = AddonTestUtils.promiseWebExtensionStartup(
      "amosigned-xpi@tests.mozilla.org"
    );
    acceptInstallDialog(installDialog);

    await notificationPromise;

    let installs = await AddonManager.getAllInstalls();
    is(installs.length, 0, "Should be no pending installs");

    let addon = await AddonManager.getAddonByID(
      "amosigned-xpi@tests.mozilla.org"
    );

    await readyPromise;
    let readyPromise2 = AddonTestUtils.promiseWebExtensionStartup(
      "amosigned-xpi@tests.mozilla.org"
    );

    // Test that the addon does not have permission. Reload it to ensure it would
    // have been set if possible.
    await addon.reload();
    await readyPromise2;
    let policy = WebExtensionPolicy.getByID(addon.id);
    ok(
      !policy.privateBrowsingAllowed,
      "private browsing permission was not granted"
    );

    await addon.uninstall();

    PermissionTestUtils.remove("http://example.com/", "install");

    await removeTabAndWaitForNotificationClose();
  },

  async function test_failedDownload() {
    PermissionTestUtils.add(
      "http://example.com/",
      "install",
      Services.perms.ALLOW_ACTION
    );

    let progressPromise = waitForProgressNotification();
    let failPromise = waitForNotification("addon-install-failed");
    let triggers = encodeURIComponent(
      JSON.stringify({
        XPI: "missing.xpi",
      })
    );
    BrowserTestUtils.openNewForegroundTab(
      gBrowser,
      TESTROOT + "installtrigger.html?" + triggers
    );
    await progressPromise;
    let panel = await failPromise;

    let notification = panel.childNodes[0];
    is(
      notification.getAttribute("label"),
      "The add-on could not be downloaded because of a connection failure.",
      "Should have seen the right message"
    );

    PermissionTestUtils.remove("http://example.com/", "install");
    await removeTabAndWaitForNotificationClose();
  },

  async function test_corruptFile() {
    PermissionTestUtils.add(
      "http://example.com/",
      "install",
      Services.perms.ALLOW_ACTION
    );

    let progressPromise = waitForProgressNotification();
    let failPromise = waitForNotification("addon-install-failed");
    let triggers = encodeURIComponent(
      JSON.stringify({
        XPI: "corrupt.xpi",
      })
    );
    BrowserTestUtils.openNewForegroundTab(
      gBrowser,
      TESTROOT + "installtrigger.html?" + triggers
    );
    await progressPromise;
    let panel = await failPromise;

    let notification = panel.childNodes[0];
    is(
      notification.getAttribute("label"),
      "The add-on downloaded from this site could not be installed " +
        "because it appears to be corrupt.",
      "Should have seen the right message"
    );

    PermissionTestUtils.remove("http://example.com/", "install");
    await removeTabAndWaitForNotificationClose();
  },

  async function test_incompatible() {
    PermissionTestUtils.add(
      "http://example.com/",
      "install",
      Services.perms.ALLOW_ACTION
    );

    let progressPromise = waitForProgressNotification();
    let failPromise = waitForNotification("addon-install-failed");
    let triggers = encodeURIComponent(
      JSON.stringify({
        XPI: "incompatible.xpi",
      })
    );
    BrowserTestUtils.openNewForegroundTab(
      gBrowser,
      TESTROOT + "installtrigger.html?" + triggers
    );
    await progressPromise;
    let panel = await failPromise;

    let notification = panel.childNodes[0];
    let brandBundle = Services.strings.createBundle(
      "chrome://branding/locale/brand.properties"
    );
    let brandShortName = brandBundle.GetStringFromName("brandShortName");
    let message = `XPI Test could not be installed because it is not compatible with ${brandShortName} ${Services.appinfo.version}.`;
    is(
      notification.getAttribute("label"),
      message,
      "Should have seen the right message"
    );

    PermissionTestUtils.remove("http://example.com/", "install");
    await removeTabAndWaitForNotificationClose();
  },

  async function test_blocklisted() {
    let addonName = "XPI Test";
    let id = "amosigned-xpi@tests.mozilla.org";
    let version = "2.2";

    const {
      BlocklistPrivate: { ExtensionBlocklistMLBF },
    } = ChromeUtils.importESModule("resource://gre/modules/Blocklist.sys.mjs");

    await SpecialPowers.pushPrefEnv({
      set: [
        [
          "extensions.blocklist.addonItemURL",
          "https://example.com/blocked-addon/%addonID%/%addonVersion%/",
        ],
      ],
    });

    const blocklistURL = ExtensionBlocklistMLBF.createBlocklistURL(id, version);

    info("Verify addon-install-failed on hard-blocked addon");
    await testBlocklistedAddon({
      stash: { blocked: [`${id}:${version}`], unblocked: [] },
      expected: {
        fluentId: "addon-install-error-hard-blocked",
        blocklistURL,
      },
    });

    info("Verify addon-install-failed on soft-blocked blocked addon");
    await SpecialPowers.pushPrefEnv({
      set: [["extensions.blocklist.softblock.enabled", true]],
    });
    await testBlocklistedAddon({
      stash: { softblocked: [`${id}:${version}`], blocked: [], unblocked: [] },
      expected: {
        fluentId: "addon-install-error-soft-blocked",
        blocklistURL,
      },
    });
    // Clear extensions.blocklist.softblock.enabled pref.
    await SpecialPowers.popPrefEnv();
    // Clear extensions.blocklist.addonItemURL pref.
    await SpecialPowers.popPrefEnv();

    async function testBlocklistedAddon({ stash, expected }) {
      await AddonTestUtils.loadBlocklistRawData({
        extensionsMLBF: [{ stash, stash_time: 0 }],
      });
      needsCleanupBlocklist = true;
      registerCleanupFunction(cleanupBlocklist);

      PermissionTestUtils.add(
        "http://example.com/",
        "install",
        Services.perms.ALLOW_ACTION
      );

      let progressPromise = waitForProgressNotification();
      let failPromise = waitForNotification("addon-install-failed-blocklist");
      let triggers = encodeURIComponent(
        JSON.stringify({
          XPI: "amosigned.xpi",
        })
      );
      BrowserTestUtils.openNewForegroundTab(
        gBrowser,
        TESTROOT + "installtrigger.html?" + triggers
      );
      await progressPromise;
      info("Wait for addon-install-failed notification");

      let panel = await failPromise;

      let notification = panel.childNodes[0];
      let message = lazy.l10n.formatValueSync(expected.fluentId, { addonName });
      is(
        notification.getAttribute("label"),
        message,
        "Should have seen the right message"
      );

      await BrowserTestUtils.waitForCondition(
        () => panel.state === "open",
        "Wait for the panel to reach the open state"
      );
      let blocklistURLEl = panel.querySelector(
        "#addon-install-failed-blocklist-info"
      );
      ok(
        BrowserTestUtils.isVisible(blocklistURLEl),
        "Expect blocklist info link to be visible"
      );
      is(
        blocklistURLEl.getAttribute("href"),
        expected.blocklistURL,
        "Blocklist info link href should be set to the expected url"
      );
      is(
        blocklistURLEl.textContent,
        await panel.ownerDocument.l10n.formatValue(
          "popup-notification-xpinstall-prompt-block-url"
        ),
        "Blocklist info link should have the expected localized string"
      );

      // Clicking on the the blocklistURL link is expected to dismiss the
      // popup.
      let closePromise = waitForNotificationClose();

      let newTabPromise = BrowserTestUtils.waitForNewTab(
        gBrowser,
        expected.blocklistURL,
        true
      );
      info(
        `Click on the blocklist "See details" link ${expected.blocklistURL}`
      );
      blocklistURLEl.click();
      const newTab = await newTabPromise;

      is(
        newTab,
        gBrowser.selectedTab,
        "Blocklist info tab is currrently selected"
      );
      BrowserTestUtils.removeTab(newTab);

      await cleanupBlocklist();
      PermissionTestUtils.remove("http://example.com/", "install");

      BrowserTestUtils.removeTab(gBrowser.selectedTab);
      await closePromise;
    }
  },

  async function test_localFile() {
    let cr = Cc["@mozilla.org/chrome/chrome-registry;1"].getService(
      Ci.nsIChromeRegistry
    );
    let path;
    try {
      path = cr.convertChromeURL(makeURI(CHROMEROOT + "corrupt.xpi")).spec;
    } catch (ex) {
      path = CHROMEROOT + "corrupt.xpi";
    }

    let failPromise = new Promise(resolve => {
      Services.obs.addObserver(function observer() {
        Services.obs.removeObserver(observer, "addon-install-failed");
        resolve();
      }, "addon-install-failed");
    });
    gBrowser.selectedTab = BrowserTestUtils.addTab(gBrowser, "about:blank");
    await BrowserTestUtils.browserLoaded(gBrowser.selectedBrowser);
    BrowserTestUtils.startLoadingURIString(gBrowser, path);
    await failPromise;

    // Wait for the browser code to add the failure notification
    await waitForSingleNotification();

    let notification = PopupNotifications.panel.childNodes[0];
    is(
      notification.id,
      "addon-install-failed-notification",
      "Should have seen the install fail"
    );
    is(
      notification.getAttribute("label"),
      "This add-on could not be installed because it appears to be corrupt.",
      "Should have seen the right message"
    );

    await removeTabAndWaitForNotificationClose();
  },

  async function test_urlBar() {
    let progressPromise = waitForProgressNotification();
    let dialogPromise = waitForInstallDialog();

    gBrowser.selectedTab = BrowserTestUtils.addTab(gBrowser, "about:blank");
    await BrowserTestUtils.browserLoaded(gBrowser.selectedBrowser);
    gURLBar.value = TESTROOT + "amosigned.xpi";
    gURLBar.focus();
    EventUtils.synthesizeKey("KEY_Enter");

    await progressPromise;
    let installDialog = await dialogPromise;

    testInstallDialogIncognitoCheckbox(installDialog, {
      incognitoHidden: false,
      incognitoChecked: false,
      toggleIncognito: true,
    });

    let notificationPromise = acceptAppMenuNotificationWhenShown(
      "addon-installed",
      "amosigned-xpi@tests.mozilla.org"
    );
    installDialog.button.click();
    await notificationPromise;

    let installs = await AddonManager.getAllInstalls();
    is(installs.length, 0, "Should be no pending installs");

    let addon = await AddonManager.getAddonByID(
      "amosigned-xpi@tests.mozilla.org"
    );
    // The panel is reloading the addon due to the permission change, we need some way
    // to wait for the reload to finish. addon.startupPromise doesn't do it for
    // us, so we'll just restart again.
    await addon.reload();

    // This addon should have private browsing permission.
    let policy = WebExtensionPolicy.getByID(addon.id);
    ok(policy.privateBrowsingAllowed, "private browsing permission granted");

    await addon.uninstall();

    await removeTabAndWaitForNotificationClose();
  },

  async function test_wrongHost() {
    let requestedUrl = TESTROOT2 + "enabled.html";
    gBrowser.selectedTab = BrowserTestUtils.addTab(gBrowser);

    let loadedPromise = BrowserTestUtils.browserLoaded(
      gBrowser.selectedBrowser,
      false,
      requestedUrl
    );
    BrowserTestUtils.startLoadingURIString(
      gBrowser,
      TESTROOT2 + "enabled.html"
    );
    await loadedPromise;

    let progressPromise = waitForProgressNotification();
    let notificationPromise = waitForNotification("addon-install-failed");
    BrowserTestUtils.startLoadingURIString(gBrowser, TESTROOT + "corrupt.xpi");
    await progressPromise;
    let panel = await notificationPromise;

    let notification = panel.childNodes[0];
    is(
      notification.getAttribute("label"),
      "The add-on downloaded from this site could not be installed " +
        "because it appears to be corrupt.",
      "Should have seen the right message"
    );

    await removeTabAndWaitForNotificationClose();
  },

  async function test_renotifyBlocked() {
    let notificationPromise = waitForNotification("addon-install-blocked");
    let triggers = encodeURIComponent(
      JSON.stringify({
        XPI: "amosigned.xpi",
      })
    );
    BrowserTestUtils.openNewForegroundTab(
      gBrowser,
      TESTROOT + "installtrigger.html?" + triggers
    );
    let panel = await notificationPromise;

    let closePromise = waitForNotificationClose();
    // hide the panel (this simulates the user dismissing it)
    panel.hidePopup();
    await closePromise;

    info("Timeouts after this probably mean bug 589954 regressed");

    await new Promise(resolve => executeSoon(resolve));

    notificationPromise = waitForNotification("addon-install-blocked");
    BrowserTestUtils.startLoadingURIString(
      gBrowser,
      TESTROOT + "installtrigger.html?" + triggers
    );
    await notificationPromise;

    let installs = await AddonManager.getAllInstalls();
    is(installs.length, 2, "Should be two pending installs");

    await removeTabAndWaitForNotificationClose(gBrowser.selectedTab);

    installs = await AddonManager.getAllInstalls();
    is(installs.length, 0, "Should have cancelled the installs");
  },

  async function test_cancel() {
    PermissionTestUtils.add(
      "http://example.com/",
      "install",
      Services.perms.ALLOW_ACTION
    );

    let notificationPromise = waitForNotification(PROGRESS_NOTIFICATION);
    let triggers = encodeURIComponent(
      JSON.stringify({
        XPI: "slowinstall.sjs?file=amosigned.xpi",
      })
    );
    BrowserTestUtils.openNewForegroundTab(
      gBrowser,
      TESTROOT + "installtrigger.html?" + triggers
    );
    let panel = await notificationPromise;
    let notification = panel.childNodes[0];

    ok(PopupNotifications.isPanelOpen, "Notification should still be open");
    is(
      PopupNotifications.panel.childNodes.length,
      1,
      "Should be only one notification"
    );
    is(
      notification.id,
      "addon-progress-notification",
      "Should have seen the progress notification"
    );

    // Cancel the download
    let install = notification.notification.options.installs[0];
    let cancelledPromise = new Promise(resolve => {
      install.addListener({
        onDownloadCancelled() {
          install.removeListener(this);
          resolve();
        },
      });
    });
    EventUtils.synthesizeMouseAtCenter(notification.secondaryButton, {});
    await cancelledPromise;

    await waitForTick();

    ok(!PopupNotifications.isPanelOpen, "Notification should be closed");

    let installs = await AddonManager.getAllInstalls();
    is(installs.length, 0, "Should be no pending install");

    PermissionTestUtils.remove("http://example.com/", "install");
    BrowserTestUtils.removeTab(gBrowser.selectedTab);
  },

  async function test_failedSecurity() {
    await SpecialPowers.pushPrefEnv({
      set: [
        [PREF_INSTALL_REQUIREBUILTINCERTS, false],
        ["extensions.postDownloadThirdPartyPrompt", false],
      ],
    });

    setupRedirect({
      Location: TESTROOT + "amosigned.xpi",
    });

    let notificationPromise = waitForNotification("addon-install-blocked");
    let triggers = encodeURIComponent(
      JSON.stringify({
        XPI: "redirect.sjs?mode=redirect",
      })
    );
    BrowserTestUtils.openNewForegroundTab(
      gBrowser,
      SECUREROOT + "installtrigger.html?" + triggers
    );
    let panel = await notificationPromise;

    let notification = panel.childNodes[0];
    // Click on Allow
    EventUtils.synthesizeMouse(notification.button, 20, 10, {});

    // Notification should have changed to progress notification
    ok(PopupNotifications.isPanelOpen, "Notification should still be open");
    is(
      PopupNotifications.panel.childNodes.length,
      1,
      "Should be only one notification"
    );
    notification = panel.childNodes[0];
    is(
      notification.id,
      "addon-progress-notification",
      "Should have seen the progress notification"
    );

    // Wait for it to fail
    await new Promise(resolve => {
      Services.obs.addObserver(function observer() {
        Services.obs.removeObserver(observer, "addon-install-failed");
        resolve();
      }, "addon-install-failed");
    });

    // Allow the browser code to add the failure notification and then wait
    // for the progress notification to dismiss itself
    await waitForSingleNotification();
    is(
      PopupNotifications.panel.childNodes.length,
      1,
      "Should be only one notification"
    );
    notification = panel.childNodes[0];
    is(
      notification.id,
      "addon-install-failed-notification",
      "Should have seen the install fail"
    );

    await removeTabAndWaitForNotificationClose();
    await SpecialPowers.popPrefEnv();
  },

  // Verifies that incognito checkbox is checked if add-on was already
  // installed before, with private access. Regression test for bug 1581852.
  async function test_incognito_checkbox() {
    // Grant permission up front.
    await installAddonWithPrivateBrowsingAccess(
      TESTROOT + "amosigned.xpi",
      "amosigned-xpi@tests.mozilla.org"
    );
    // ^ the above add-on will be overwritten by the install below, and removed
    // at the end of this task.

    let progressPromise = waitForProgressNotification();
    let dialogPromise = waitForInstallDialog();

    gBrowser.selectedTab = BrowserTestUtils.addTab(gBrowser, "about:blank");
    await BrowserTestUtils.browserLoaded(gBrowser.selectedBrowser);
    gURLBar.value = TESTROOT + "amosigned.xpi";
    gURLBar.focus();
    EventUtils.synthesizeKey("KEY_Enter");

    await progressPromise;
    let installDialog = await dialogPromise;

    testInstallDialogIncognitoCheckbox(installDialog, {
      incognitoHidden: false,
      incognitoChecked: true,
      toggleIncognito: true,
    });

    let notificationPromise = acceptAppMenuNotificationWhenShown(
      "addon-installed",
      "amosigned-xpi@tests.mozilla.org"
    );
    let readyPromise = AddonTestUtils.promiseWebExtensionStartup(
      "amosigned-xpi@tests.mozilla.org"
    );
    installDialog.button.click();
    await notificationPromise;

    let installs = await AddonManager.getAllInstalls();
    is(installs.length, 0, "Should be no pending installs");

    let addon = await AddonManager.getAddonByID(
      "amosigned-xpi@tests.mozilla.org"
    );

    await readyPromise;

    // This addon should no longer have private browsing permission.
    let policy = WebExtensionPolicy.getByID(addon.id);
    ok(!policy.privateBrowsingAllowed, "private browsing permission removed");

    await addon.uninstall();

    await removeTabAndWaitForNotificationClose();
  },

  async function test_incognito_checkbox_new_window() {
    // Grant permission up front.
    await installAddonWithPrivateBrowsingAccess(
      TESTROOT + "amosigned.xpi",
      "amosigned-xpi@tests.mozilla.org"
    );
    // ^ the above add-on will be overwritten by the install below, and removed
    // at the end of this task.

    let win = await BrowserTestUtils.openNewBrowserWindow();
    await SimpleTest.promiseFocus(win);

    let panelEventPromise = new Promise(resolve => {
      win.PopupNotifications.panel.addEventListener(
        "PanelUpdated",
        function eventListener(e) {
          if (e.detail.includes("addon-webext-permissions")) {
            win.PopupNotifications.panel.removeEventListener(
              "PanelUpdated",
              eventListener
            );
            // NOTE: the panel may intermittently still be in the "showing" state, and
            // so we explicitly await for the state to become "open" before proceeding
            // with asserting the visibility of the elements we expected to be in the
            // panel.
            if (win.PopupNotifications.panel.state === "showing") {
              TestUtils.waitForCondition(
                () => win.PopupNotifications.panel.state === "open",
                "Wait for addon-webext-permissions panel state to become open"
              ).then(resolve);
            } else {
              is(
                win.PopupNotifications.panel.state,
                "open",
                "Expect addon-webext-permissions panel state to be open"
              );
              resolve();
            }
          }
        }
      );
    });

    win.gBrowser.selectedTab = BrowserTestUtils.addTab(
      win.gBrowser,
      "about:blank"
    );
    await BrowserTestUtils.browserLoaded(win.gBrowser.selectedBrowser);
    win.gURLBar.value = TESTROOT + "amosigned.xpi";
    win.gURLBar.focus();
    EventUtils.synthesizeKey("KEY_Enter", {}, win);

    await panelEventPromise;
    await waitForTick();

    let panel = win.PopupNotifications.panel;
    let installDialog = panel.childNodes[0];

    testInstallDialogIncognitoCheckbox(installDialog, {
      incognitoHidden: false,
      incognitoChecked: true,
      toggleIncognito: true,
    });

    let notificationPromise = acceptAppMenuNotificationWhenShown(
      "addon-installed",
      "amosigned-xpi@tests.mozilla.org",
      { global: win }
    );
    let readyPromise = AddonTestUtils.promiseWebExtensionStartup(
      "amosigned-xpi@tests.mozilla.org"
    );
    acceptInstallDialog(installDialog);
    await notificationPromise;

    let installs = await AddonManager.getAllInstalls();
    is(installs.length, 0, "Should be no pending installs");

    let addon = await AddonManager.getAddonByID(
      "amosigned-xpi@tests.mozilla.org"
    );

    await readyPromise;

    // This addon should no longer have private browsing permission.
    let policy = WebExtensionPolicy.getByID(addon.id);
    ok(!policy.privateBrowsingAllowed, "private browsing permission removed");

    await addon.uninstall();

    await BrowserTestUtils.closeWindow(win);
  },

  async function test_mv3_installOrigins_disallowed_with_unified_extensions() {
    await SpecialPowers.pushPrefEnv({
      set: [
        // Disable signature check because we load an unsigned MV3 extension.
        ["xpinstall.signatures.required", false],
        ["extensions.install_origins.enabled", true],
      ],
    });

    let win = await BrowserTestUtils.openNewBrowserWindow();
    await SimpleTest.promiseFocus(win);

    let notificationPromise = waitForNotification(
      "addon-install-failed",
      1,
      "unified-extensions-button",
      win
    );
    let triggers = encodeURIComponent(
      JSON.stringify({
        // This XPI does not have any `install_origins` in its manifest.
        XPI: "unsigned_mv3.xpi",
      })
    );
    await BrowserTestUtils.openNewForegroundTab(
      win.gBrowser,
      TESTROOT + "installtrigger.html?" + triggers
    );
    await notificationPromise;

    await BrowserTestUtils.closeWindow(win);
    await SpecialPowers.popPrefEnv();
  },

  async function test_mv3_installOrigins_allowed_with_unified_extensions() {
    await SpecialPowers.pushPrefEnv({
      set: [
        // Disable signature check because we load an unsigned MV3 extension.
        ["xpinstall.signatures.required", false],
        // When this pref is disabled, install should be possible.
        ["extensions.install_origins.enabled", false],
      ],
    });

    let win = await BrowserTestUtils.openNewBrowserWindow();
    await SimpleTest.promiseFocus(win);

    let notificationPromise = waitForNotification(
      "addon-install-blocked",
      1,
      "unified-extensions-button",
      win
    );
    let triggers = encodeURIComponent(
      JSON.stringify({
        // This XPI does not have any `install_origins` in its manifest.
        XPI: "unsigned_mv3.xpi",
      })
    );
    await BrowserTestUtils.openNewForegroundTab(
      win.gBrowser,
      TESTROOT + "installtrigger.html?" + triggers
    );
    let panel = await notificationPromise;

    let closePromise = waitForNotificationClose(win);
    // hide the panel (this simulates the user dismissing it)
    panel.hidePopup();
    await closePromise;

    await BrowserTestUtils.closeWindow(win);
    await SpecialPowers.popPrefEnv();
  },
];

var gTestStart = null;

var XPInstallObserver = {
  observe(aSubject, aTopic) {
    var installInfo = aSubject.wrappedJSObject;
    info(
      "Observed " + aTopic + " for " + installInfo.installs.length + " installs"
    );
    installInfo.installs.forEach(function (aInstall) {
      info(
        "Install of " +
          aInstall.sourceURI.spec +
          " was in state " +
          aInstall.state
      );
    });
  },
};

add_setup(async function () {
  requestLongerTimeout(4);

  await SpecialPowers.pushPrefEnv({
    set: [
      ["extensions.logging.enabled", true],
      ["extensions.strictCompatibility", true],
      ["extensions.install.requireSecureOrigin", false],
      ["security.dialog_enable_delay", 0],
      // These tests currently depends on InstallTrigger.install.
      ["extensions.InstallTrigger.enabled", true],
      ["extensions.InstallTriggerImpl.enabled", true],
      // Relax the user input requirements while running this test.
      ["xpinstall.userActivation.required", false],
      // Bug 721336 - Use sync XHR system requests
      ["network.xhr.block_sync_system_requests", false],
    ],
  });

  Services.obs.addObserver(XPInstallObserver, "addon-install-started");
  Services.obs.addObserver(XPInstallObserver, "addon-install-blocked");
  Services.obs.addObserver(XPInstallObserver, "addon-install-failed");

  registerCleanupFunction(async function () {
    // Make sure no more test parts run in case we were timed out
    TESTS = [];

    let aInstalls = await AddonManager.getAllInstalls();
    aInstalls.forEach(function (aInstall) {
      aInstall.cancel();
    });

    Services.obs.removeObserver(XPInstallObserver, "addon-install-started");
    Services.obs.removeObserver(XPInstallObserver, "addon-install-blocked");
    Services.obs.removeObserver(XPInstallObserver, "addon-install-failed");
  });
});

// Run all test cases with the private browsing checkbox available in the first
// install dialog, before the addon has been already installed.
add_task(async function testBasic() {
  for (let i = 0; i < TESTS.length; ++i) {
    if (gTestStart) {
      info("Test part took " + (Date.now() - gTestStart) + "ms");
    }

    ok(!PopupNotifications.isPanelOpen, "Notification should be closed");

    let installs = await AddonManager.getAllInstalls();

    is(installs.length, 0, "Should be no active installs");
    info("===== Running test case: " + TESTS[i].name);
    gTestStart = Date.now();
    await TESTS[i]();
  }
});
