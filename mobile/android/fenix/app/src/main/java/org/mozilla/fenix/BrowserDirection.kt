/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix

import androidx.annotation.IdRes

/**
 * Used with [HomeActivity.openToBrowser] to indicate which fragment
 * the browser is being opened from.
 *
 * @property fragmentId ID of the fragment opening the browser in the navigation graph.
 * An ID of `0` indicates a global action with no corresponding opening fragment.
 */
enum class BrowserDirection(@param:IdRes val fragmentId: Int) {
    FromGlobal(0),
    FromHome(R.id.homeFragment),
    FromWallpaper(R.id.wallpaperSettingsFragment),
    FromSearchDialog(R.id.searchDialogFragment),
    FromSettings(R.id.settingsFragment),
    FromBookmarks(R.id.bookmarkFragment),
    FromHistory(R.id.historyFragment),
    FromHistoryMetadataGroup(R.id.historyMetadataGroupFragment),
    FromTrackingProtectionExceptions(R.id.trackingProtectionExceptionsFragment),
    FromAbout(R.id.aboutFragment),
    FromTrackingProtection(R.id.trackingProtectionFragment),
    FromHttpsOnlyMode(R.id.httpsOnlyFragment),
    FromDnsOverHttps(R.id.dohSettingsFragment),
    FromTrackingProtectionDialog(R.id.trackingProtectionPanelDialogFragment),
    FromSavedLoginsFragment(R.id.savedLoginsFragment),
    FromAddNewDeviceFragment(R.id.addNewDeviceFragment),
    FromSearchEngineFragment(R.id.searchEngineFragment),
    FromSaveSearchEngineFragment(R.id.saveSearchEngineFragment),
    FromAddonDetailsFragment(R.id.addonDetailsFragment),
    FromStudiesFragment(R.id.studiesFragment),
    FromAddonPermissionsDetailsFragment(R.id.addonPermissionsDetailFragment),
    FromLoginDetailFragment(R.id.loginDetailFragment),
    FromTabsTray(R.id.tabsTrayFragment),
    FromTabManager(R.id.tabManagementFragment),
    FromRecentlyClosed(R.id.recentlyClosedFragment),
    FromAddonsManagementFragment(R.id.addonsManagementFragment),
    FromTranslationsDialogFragment(R.id.translationsDialogFragment),
    FromDownloadLanguagesPreferenceFragment(R.id.downloadLanguagesPreferenceFragment),
    FromMenuDialogFragment(R.id.menuDialogFragment),
    FromWebCompatReporterFragment(R.id.webCompatReporterFragment),
    FromGleanDebugToolsFragment(R.id.gleanDebugToolsFragment),
}
