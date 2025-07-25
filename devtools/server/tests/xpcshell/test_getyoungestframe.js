/* eslint-disable strict */
function run_test() {
  Services.prefs.setBoolPref("security.allow_eval_with_system_principal", true);
  registerCleanupFunction(() => {
    Services.prefs.clearUserPref("security.allow_eval_with_system_principal");
  });
  const { addDebuggerToGlobal } = ChromeUtils.importESModule(
    "resource://gre/modules/jsdebugger.sys.mjs"
  );
  addDebuggerToGlobal(globalThis);
  const xpcInspector = Cc["@mozilla.org/jsinspector;1"].getService(
    Ci.nsIJSInspector
  );
  const g = createTestGlobal("test1");

  const dbg = makeDebugger({
    shouldAddNewGlobalAsDebuggee() {
      return true;
    },
  });
  dbg.uncaughtExceptionHook = testExceptionHook;

  dbg.addDebuggee(g);
  dbg.onDebuggerStatement = function (frame) {
    Assert.strictEqual(frame, dbg.getNewestFrame());
    // Execute from the nested event loop, dbg.getNewestFrame() won't
    // be working anymore.

    executeSoon(function () {
      try {
        Assert.strictEqual(frame, dbg.getNewestFrame());
      } finally {
        xpcInspector.exitNestedEventLoop("test");
      }
    });
    xpcInspector.enterNestedEventLoop("test");
  };

  g.eval("function debuggerStatement() { debugger; }; debuggerStatement();");

  dbg.disable();
}
