/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

const ConsoleAPIStorage = Cc["@mozilla.org/consoleAPI-storage;1"].getService(
  Ci.nsIConsoleAPIStorage
);

const { WebExtensionPolicy } = Cu.getGlobalForObject(
  ChromeUtils.importESModule("resource://gre/modules/AppConstants.sys.mjs")
);

const FAKE_ADDON_ID = "test-webext-addon@mozilla.org";
const EXPECTED_CONSOLE_ID = `addon/${FAKE_ADDON_ID}`;
const EXPECTED_CONSOLE_MESSAGE_CONTENT = "fake-webext-addon-test-log-message";

const ConsoleObserver = {
  init() {
    ConsoleAPIStorage.addLogEventListener(
      this.observe,
      Cc["@mozilla.org/systemprincipal;1"].createInstance(Ci.nsIPrincipal)
    );
  },

  uninit() {
    ConsoleAPIStorage.removeLogEventListener(this.observe);
  },

  observe(aSubject) {
    let consoleAPIMessage = aSubject.wrappedJSObject;

    is(
      consoleAPIMessage.arguments[0],
      EXPECTED_CONSOLE_MESSAGE_CONTENT,
      "the consoleAPIMessage contains the expected message"
    );

    is(
      consoleAPIMessage.addonId,
      FAKE_ADDON_ID,
      "the consoleAPImessage originAttributes contains the expected addonId"
    );

    let cachedMessages = ConsoleAPIStorage.getEvents().filter(msg => {
      return msg.addonId == FAKE_ADDON_ID;
    });

    is(
      cachedMessages.length,
      1,
      "found the expected cached console messages from the addon"
    );
    is(
      cachedMessages[0] && cachedMessages[0].addonId,
      FAKE_ADDON_ID,
      "the cached message originAttributes contains the expected addonId"
    );

    finish();
  },
};

function test() {
  ConsoleObserver.init();

  waitForExplicitFinish();

  let uuidGenerator = Services.uuid;
  let uuid = uuidGenerator.generateUUID().number;
  uuid = uuid.slice(1, -1); // Strip { and } off the UUID.

  const url = `moz-extension://${uuid}/`;
  let policy = new WebExtensionPolicy({
    id: FAKE_ADDON_ID,
    mozExtensionHostname: uuid,
    baseURL: "file:///",
    allowedOrigins: new MatchPatternSet([]),
    localizeCallback() {},
  });
  policy.active = true;

  let baseURI = Services.io.newURI(url);
  let principal = Services.scriptSecurityManager.createContentPrincipal(
    baseURI,
    {}
  );

  let chromeWebNav = Services.appShell.createWindowlessBrowser(true);
  let docShell = chromeWebNav.docShell;
  docShell.createAboutBlankDocumentViewer(principal, principal);

  info("fake webextension docShell created");

  registerCleanupFunction(function () {
    policy.active = false;
    if (chromeWebNav) {
      chromeWebNav.close();
      chromeWebNav = null;
    }
    ConsoleObserver.uninit();
  });

  let window = docShell.docViewer.DOMDocument.defaultView;
  window.console.log(EXPECTED_CONSOLE_MESSAGE_CONTENT);
  chromeWebNav.close();
  chromeWebNav = null;

  info("fake webextension page logged a console api message");
}
