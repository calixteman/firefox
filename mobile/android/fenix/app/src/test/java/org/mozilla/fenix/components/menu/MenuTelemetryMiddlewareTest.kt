/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.components.menu

import mozilla.components.browser.state.state.ReaderState
import mozilla.components.browser.state.state.createTab
import mozilla.components.feature.addons.Addon
import mozilla.components.service.fxa.manager.AccountState
import mozilla.components.support.test.ext.joinBlocking
import mozilla.components.support.test.robolectric.testContext
import mozilla.telemetry.glean.internal.CounterMetric
import mozilla.telemetry.glean.private.EventMetricType
import mozilla.telemetry.glean.private.NoExtras
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Rule
import org.junit.Test
import org.junit.runner.RunWith
import org.mozilla.fenix.GleanMetrics.AppMenu
import org.mozilla.fenix.GleanMetrics.Events
import org.mozilla.fenix.GleanMetrics.HomeMenu
import org.mozilla.fenix.GleanMetrics.Menu
import org.mozilla.fenix.GleanMetrics.ReaderMode
import org.mozilla.fenix.GleanMetrics.Translations
import org.mozilla.fenix.components.menu.middleware.MenuTelemetryMiddleware
import org.mozilla.fenix.components.menu.store.BrowserMenuState
import org.mozilla.fenix.components.menu.store.MenuAction
import org.mozilla.fenix.components.menu.store.MenuState
import org.mozilla.fenix.components.menu.store.MenuStore
import org.mozilla.fenix.helpers.FenixGleanTestRule
import org.robolectric.RobolectricTestRunner

@RunWith(RobolectricTestRunner::class)
class MenuTelemetryMiddlewareTest {
    @get:Rule
    val gleanTestRule = FenixGleanTestRule(testContext)

    @Test
    fun `WHEN adding a bookmark THEN record the bookmark browser menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.AddBookmark).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "add_bookmark")
    }

    @Test
    fun `WHEN navigating to edit a bookmark THEN record the edit bookmark browser menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.EditBookmark).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "edit_bookmark")
    }

    @Test
    fun `WHEN navigating to bookmarks THEN record the bookmarks browser menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.Bookmarks).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "bookmarks")
    }

    @Test
    fun `WHEN adding a shortcut THEN record the add to top sites browser menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.AddShortcut).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "add_to_top_sites")
    }

    @Test
    fun `WHEN open in regular tab THEN record open in regular tab menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.OpenInRegularTab).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "open_in_regular_tab")
    }

    @Test
    fun `WHEN removing a shortcut from top sites THEN record the remove from top sites browser menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.RemoveShortcut).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "remove_from_top_sites")
    }

    fun `WHEN navigating to add site to home screen THEN record the add_to_homescreen browser menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.AddToHomeScreen).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "add_to_homescreen")
    }

    @Test
    fun `WHEN navigating to downloads THEN record the downloads browser menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.Downloads).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "downloads")
    }

    @Test
    fun `WHEN navigating to history THEN record the history browser menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.History).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "history")
    }

    @Test
    fun `WHEN navigating to manage extensions THEN record the manage extensions browser menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.ManageExtensions).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "addons_manager")
    }

    @Test
    fun `WHEN navigating to sync account THEN record the sync account browser menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(
            MenuAction.Navigate.MozillaAccount(
                accountState = AccountState.NotAuthenticated,
                accesspoint = MenuAccessPoint.Browser,
            ),
        ).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "sync_account")
        assertTelemetryRecorded(AppMenu.signIntoSync)
    }

    @Test
    fun `WHEN opening a site in app THEN record the open in app menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.OpenInApp).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "open_in_app")
    }

    @Test
    fun `WHEN navigating to passwords THEN record the passwords browser menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.Passwords).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "passwords")
    }

    @Test
    fun `WHEN navigating to the release notes page from home page menu THEN record the whats new interaction telemetry`() {
        val store = createStore()
        assertNull(Events.whatsNewTapped.testGetValue())

        store.dispatch(MenuAction.Navigate.ReleaseNotes).joinBlocking()

        assertNotNull(Events.whatsNewTapped.testGetValue())
        val snapshot = Events.whatsNewTapped.testGetValue()!!

        assertEquals(1, snapshot.size)
        assertEquals("MENU", snapshot.single().extra?.getValue("source"))
    }

    @Test
    fun `WHEN navigating to the save to collection sheet THEN record the share browser menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.SaveToCollection(hasCollection = true)).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "save_to_collection")
    }

    @Test
    fun `WHEN navigating back without view history THEN record back telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.Back(viewHistory = false)).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "back")
    }

    @Test
    fun `WHEN navigating back with view history THEN record back long press telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.Back(viewHistory = true)).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "back_long_press")
    }

    @Test
    fun `WHEN navigating back from external menu without view history THEN record custom back telemetry`() {
        val store = createStore(accessPoint = MenuAccessPoint.External)
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.Back(viewHistory = false)).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "custom_back")
    }

    @Test
    fun `WHEN navigating back from external menu with view history THEN record custom back long press telemetry`() {
        val store = createStore(accessPoint = MenuAccessPoint.External)
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.Back(viewHistory = true)).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "custom_back_long_press")
    }

    @Test
    fun `WHEN navigating forward without view history THEN record forward telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.Forward(viewHistory = false)).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "forward")
    }

    @Test
    fun `WHEN navigating forward with view history THEN record forward long press telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.Forward(viewHistory = true)).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "forward_long_press")
    }

    @Test
    fun `WHEN navigating forward from external menu without view history THEN record custom forward telemetry`() {
        val store = createStore(accessPoint = MenuAccessPoint.External)
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.Forward(viewHistory = false)).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "custom_forward")
    }

    @Test
    fun `WHEN navigating forward from external menu with view history THEN record custom forward long press telemetry`() {
        val store = createStore(accessPoint = MenuAccessPoint.External)
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.Forward(viewHistory = true)).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "custom_forward_long_press")
    }

    @Test
    fun `WHEN reloading page THEN record reload telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.Reload(bypassCache = false)).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "reload")
    }

    @Test
    fun `WHEN stopping page load THEN record stop telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.Stop).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "stop")
    }

    @Test
    fun `WHEN navigating to the share sheet THEN record the share browser menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.Share).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "share")
    }

    @Test
    fun `WHEN navigating to the share sheet in custom tab THEN record the share custom tab telemetry`() {
        val store = createStore(accessPoint = MenuAccessPoint.External)
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.Share).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "custom_share")
    }

    @Test
    fun `GIVEN the menu accesspoint is from the browser WHEN navigating to the settings THEN record the settings browser menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())
        assertNull(HomeMenu.settingsItemClicked.testGetValue())

        store.dispatch(MenuAction.Navigate.Settings).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "settings")
        assertNull(HomeMenu.settingsItemClicked.testGetValue())
    }

    @Test
    fun `GIVEN the menu accesspoint is from the home screen WHEN navigating to the settings THEN record the home menu interaction telemetry`() {
        val store = createStore(accessPoint = MenuAccessPoint.Home)
        assertNull(Events.browserMenuAction.testGetValue())
        assertNull(HomeMenu.settingsItemClicked.testGetValue())

        store.dispatch(MenuAction.Navigate.Settings).joinBlocking()

        assertTelemetryRecorded(HomeMenu.settingsItemClicked)
        assertNull(Events.browserMenuAction.testGetValue())
    }

    @Test
    fun `WHEN navigating to translations dialog THEN record the translate browser menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())
        assertNull(Translations.action.testGetValue())

        store.dispatch(MenuAction.Navigate.Translate).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "translate")

        val snapshot = Translations.action.testGetValue()!!
        assertEquals(1, snapshot.size)
        assertEquals("main_flow_browser", snapshot.single().extra?.getValue("item"))
    }

    @Test
    fun `WHEN deleting browsing data and quitting THEN record the quit browser menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.DeleteBrowsingDataAndQuit).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "quit")
    }

    @Test
    fun `WHEN find in page feature is started THEN record the find in page browser menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.FindInPage).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "find_in_page")
    }

    @Test
    fun `WHEN find in page feature is started in custom tab THEN record the find in page custom tab telemetry`() {
        val store = createStore(accessPoint = MenuAccessPoint.External)
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.FindInPage).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "custom_find_in_page")
    }

    @Test
    fun `WHEN menu banner action is dispatched THEN record the menu banner telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.MenuBanner).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "menu_banner")
    }

    @Test
    fun `WHEN dismiss menu banner action is dispatched THEN record the menu banner telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.DismissMenuBanner).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "dismiss_menu_banner")
    }

    @Test
    fun `WHEN customize reader view action is dispatched THEN record the reader mode appearance telemetry`() {
        val store = createStore()
        assertNull(ReaderMode.appearance.testGetValue())

        store.dispatch(MenuAction.CustomizeReaderView).joinBlocking()

        assertTelemetryRecorded(ReaderMode.appearance)
    }

    @Test
    fun `GIVEN reader view is not active WHEN toggle reader view action is dispatched THEN record the reader mode opened telemetry`() {
        val url = "https://www.mozilla.org"
        val title = "Mozilla"
        val readerState = ReaderState(
            readerable = true,
            active = false,
        )
        val browserMenuState = BrowserMenuState(
            selectedTab = createTab(
                url = url,
                title = title,
                readerState = readerState,
            ),
        )
        val store = createStore(
            menuState = MenuState(
                browserMenuState = browserMenuState,
            ),
        )

        assertNull(ReaderMode.opened.testGetValue())

        store.dispatch(MenuAction.ToggleReaderView).joinBlocking()

        assertTelemetryRecorded(ReaderMode.opened)
    }

    @Test
    fun `GIVEN reader view is active WHEN toggle reader view action is dispatched THEN record the reader mode closed telemetry`() {
        val url = "https://www.mozilla.org"
        val title = "Mozilla"
        val readerState = ReaderState(
            readerable = true,
            active = true,
        )
        val browserMenuState = BrowserMenuState(
            selectedTab = createTab(
                url = url,
                title = title,
                readerState = readerState,
            ),
        )
        val store = createStore(
            menuState = MenuState(
                browserMenuState = browserMenuState,
            ),
        )

        assertNull(ReaderMode.closed.testGetValue())

        store.dispatch(MenuAction.ToggleReaderView).joinBlocking()

        assertTelemetryRecorded(ReaderMode.closed)
    }

    @Test
    fun `WHEN requesting desktop site THEN record the desktop view ON telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.RequestDesktopSite).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "desktop_view_on")
    }

    @Test
    fun `WHEN requesting desktop site in custom tab THEN record the desktop view ON custom tab telemetry`() {
        val store = createStore(accessPoint = MenuAccessPoint.External)
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.RequestDesktopSite).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "custom_desktop_view_on")
    }

    @Test
    fun `WHEN requesting mobile site THEN record the desktop view OFF telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.RequestMobileSite).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "desktop_view_off")
    }

    @Test
    fun `WHEN requesting mobile site in custom tab THEN record the desktop view OFF custom tab telemetry`() {
        val store = createStore(accessPoint = MenuAccessPoint.External)
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.RequestMobileSite).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "custom_desktop_view_off")
    }

    fun `When opening a site in browser THEN record the open in Fenix telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.OpenInFirefox).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "open_in_fenix")
    }

    @Test
    fun `WHEN navigating to the discover more extensions page THEN record the discover more extensions browser menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.DiscoverMoreExtensions).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "discover_more_extensions")
    }

    @Test
    fun `WHEN navigating to the sumo page for installing add-ons THEN record the extensions learn more browser menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.ExtensionsLearnMore).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "extensions_learn_more")
    }

    @Test
    fun `WHEN navigating to an add-on's details THEN record the addon details browser menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.AddonDetails(Addon(""))).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "addon_details")
    }

    @Test
    fun `WHEN installing an add-on THEN record the install addon browser menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.InstallAddon(Addon(""))).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "install_addon")
    }

    @Test
    fun `WHEN navigating to an installed add-on's details THEN record the installed addon details browser menu telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.InstalledAddonDetails(Addon(""))).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "installed_addon_details")
    }

    @Test
    fun `WHEN CFR is shown THEN record the CFR is shown menu telemetry`() {
        val store = createStore()
        assertNull(Menu.showCfr.testGetValue())

        store.dispatch(MenuAction.OnCFRShown).joinBlocking()

        assertTelemetryRecorded(Menu.showCfr)
    }

    @Test
    fun `WHEN CFR is dismissed THEN record the CFR is dismissed menu telemetry`() {
        val store = createStore()
        assertNull(Menu.dismissCfr.testGetValue())

        store.dispatch(MenuAction.OnCFRDismiss).joinBlocking()

        assertTelemetryRecorded(Menu.dismissCfr)
    }

    @Test
    fun `WHEN navigating to web compat reporter THEN record the web compat reporter telemetry`() {
        val store = createStore()
        assertNull(Events.browserMenuAction.testGetValue())

        store.dispatch(MenuAction.Navigate.WebCompatReporter).joinBlocking()

        assertTelemetryRecorded(Events.browserMenuAction, item = "report_broken_site")
    }

    private fun assertTelemetryRecorded(
        event: EventMetricType<Events.BrowserMenuActionExtra>,
        item: String,
    ) {
        assertNotNull(event.testGetValue())

        val snapshot = event.testGetValue()!!
        assertEquals(1, snapshot.size)
        assertEquals(item, snapshot.single().extra?.getValue("item"))
    }

    private fun assertTelemetryRecorded(event: EventMetricType<NoExtras>) {
        assertNotNull(event.testGetValue())
        assertEquals(1, event.testGetValue()!!.size)
    }

    private fun assertTelemetryRecorded(event: CounterMetric) {
        assertNotNull(event.testGetValue())
        assertEquals(1, event.testGetValue()!!)
    }

    private fun createStore(
        menuState: MenuState = MenuState(),
        accessPoint: MenuAccessPoint = MenuAccessPoint.Browser,
    ) = MenuStore(
        initialState = menuState,
        middleware = listOf(
            MenuTelemetryMiddleware(
                accessPoint = accessPoint,
            ),
        ),
    )
}
