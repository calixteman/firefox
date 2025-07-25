/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

Services.prefs.setBoolPref("security.allow_eval_with_system_principal", true);
registerCleanupFunction(() => {
  Services.prefs.clearUserPref("security.allow_eval_with_system_principal");
});

add_task(
  threadFrontTest(async ({ threadFront, debuggee }) => {
    const packet = await executeOnNextTickAndWaitForPause(
      () => evalCode(debuggee),
      threadFront
    );

    const args = packet.frame.arguments;

    Assert.equal(args[0].class, "Object");

    const objectFront = threadFront.pauseGrip(args[0]);
    const response = await objectFront.getPrototype();
    Assert.notEqual(response.prototype, undefined);

    await threadFront.resume();
  })
);

function evalCode(debuggee) {
  debuggee.eval(
    // These arguments are tested.
    // eslint-disable-next-line no-unused-vars
    function stopMe(arg1) {
      debugger;
    }.toString()
  );
  debuggee.eval(
    function Constr() {
      this.a = 1;
    }.toString()
  );
  debuggee.eval(
    "Constr.prototype = { b: true, c: 'foo' }; var o = new Constr(); stopMe(o)"
  );
}
