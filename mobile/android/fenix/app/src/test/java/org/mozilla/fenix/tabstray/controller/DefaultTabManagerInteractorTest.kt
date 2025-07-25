/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.tabstray.controller

import io.mockk.every
import io.mockk.mockk
import io.mockk.verify
import io.mockk.verifySequence
import mozilla.components.browser.state.state.ContentState
import mozilla.components.browser.state.state.TabSessionState
import org.junit.Test

class DefaultTabManagerInteractorTest {

    private val controller: TabManagerController = mockk(relaxed = true)
    private val interactor = DefaultTabManagerInteractor(controller)

    @Test
    fun `WHEN user selects a new browser tab THEN the Interactor delegates to the controller`() {
        val tab: TabSessionState = mockk()
        interactor.onTabSelected(tab, null)

        verifySequence { controller.handleTabSelected(tab, null) }
    }

    @Test
    fun `WHEN user deletes one browser tab page THEN the Interactor delegates to the controller`() {
        val tab: TabSessionState = mockk()
        val id = "testTabId"
        every { tab.id } returns id
        interactor.onTabClosed(tab)

        verifySequence { controller.handleTabDeletion(id) }
    }

    @Test
    fun `WHEN user confirms downloads cancellation THEN the Interactor delegates to the controller`() {
        interactor.onDeletePrivateTabWarningAccepted("testTabId")

        verifySequence { controller.handleDeleteTabWarningAccepted("testTabId") }
    }

    @Test
    fun `WHEN user clicks to delete the selected tabs THEN the Interactor delegates to the controller`() {
        interactor.onDeleteSelectedTabsClicked()

        verify { controller.handleDeleteSelectedTabsClicked() }
    }

    @Test
    fun `WHEN user clicks to force the selected tabs as inactive THEN the Interactor delegates to the controller`() {
        interactor.onForceSelectedTabsAsInactiveClicked()

        verify { controller.handleForceSelectedTabsAsInactiveClicked() }
    }

    @Test
    fun `WHEN user clicks to bookmark the selected tabs THEN the Interactor delegates to the controller`() {
        interactor.onBookmarkSelectedTabsClicked()

        verify { controller.handleBookmarkSelectedTabsClicked() }
    }

    @Test
    fun `WHEN user clicks to save the selected tabs to a collection THEN the Interactor delegates to the controller`() {
        interactor.onAddSelectedTabsToCollectionClicked()

        verify { controller.handleAddSelectedTabsToCollectionClicked() }
    }

    @Test
    fun `WHEN user clicks to share the selected tabs THEN the Interactor delegates to the controller`() {
        interactor.onShareSelectedTabs()

        verify { controller.handleShareSelectedTabsClicked() }
    }

    @Test
    fun `WHEN the inactive tabs header is clicked THEN update the expansion state of the inactive tabs card`() {
        interactor.onInactiveTabsHeaderClicked(true)

        verify { controller.handleInactiveTabsHeaderClicked(true) }
    }

    @Test
    fun `WHEN the inactive tabs auto close dialog's close button is clicked THEN dismiss the dialog`() {
        interactor.onAutoCloseDialogCloseButtonClicked()

        verify { controller.handleInactiveTabsAutoCloseDialogDismiss() }
    }

    @Test
    fun `WHEN the enable inactive tabs auto close button is clicked THEN turn on the auto close feature`() {
        interactor.onEnableAutoCloseClicked()

        verify { controller.handleEnableInactiveTabsAutoCloseClicked() }
    }

    @Test
    fun `WHEN an inactive tab is clicked THEN open the tab`() {
        val tab = TabSessionState(
            id = "tabId",
            content = ContentState(
                url = "www.mozilla.com",
            ),
        )

        interactor.onInactiveTabClicked(tab)

        verify { controller.handleInactiveTabClicked(tab) }
    }

    @Test
    fun `WHEN an inactive tab is clicked to be closed THEN close the tab`() {
        val tab = TabSessionState(
            id = "tabId",
            content = ContentState(
                url = "www.mozilla.com",
            ),
        )

        interactor.onInactiveTabClosed(tab)

        verify { controller.handleCloseInactiveTabClicked(tab) }
    }

    @Test
    fun `WHEN the close all inactive tabs button is clicked THEN delete all inactive tabs`() {
        interactor.onDeleteAllInactiveTabsClicked()

        verify { controller.handleDeleteAllInactiveTabsClicked() }
    }

    @Test
    fun `GIVEN the user is viewing normal tabs WHEN the user clicks on the FAB THEN the Interactor delegates to the controller`() {
        interactor.onNormalTabsFabClicked()

        verifySequence { controller.handleNormalTabsFabClick() }
    }

    @Test
    fun `GIVEN the user is viewing private tabs WHEN the user clicks on the FAB THEN the Interactor delegates to the controller`() {
        interactor.onPrivateTabsFabClicked()

        verifySequence { controller.handlePrivateTabsFabClick() }
    }

    @Test
    fun `GIVEN the user is viewing synced tabs WHEN the user clicks on the FAB THEN the Interactor delegates to the controller`() {
        interactor.onSyncedTabsFabClicked()

        verifySequence { controller.handleSyncedTabsFabClick() }
    }
}
