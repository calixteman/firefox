/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.crashes

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import mozilla.components.lib.crash.store.CrashReportCache
import mozilla.components.lib.crash.store.CrashReportOption
import mozilla.components.lib.crash.store.TimeInMillis
import org.mozilla.fenix.utils.Settings

/**
 * A disk cache for handling data related to Crash Reports.
 *
 * @param settings Convenience delegate for Shared Preferences.
 */
class SettingsCrashReportCache(private val settings: Settings) : CrashReportCache {
    override suspend fun getCutoffDate(): TimeInMillis? =
        settings.crashReportCutoffDate.takeIf { it != 0L }

    override suspend fun setCutoffDate(timeInMillis: TimeInMillis?) {
        settings.crashReportCutoffDate = timeInMillis ?: 0
    }

    override suspend fun getDeferredUntil(): TimeInMillis? =
        settings.crashReportDeferredUntil.takeIf { it != 0L }

    override suspend fun setDeferredUntil(timeInMillis: TimeInMillis?) {
        settings.crashReportDeferredUntil = timeInMillis ?: 0
    }

    override suspend fun setCrashPullNeverShowAgain(neverShowAgain: Boolean) {
        settings.crashPullNeverShowAgain = neverShowAgain
    }

    override suspend fun setCrashPullDeferUntil(timeInMillis: TimeInMillis) {
        settings.crashPullDontShowBefore = timeInMillis
    }

    override suspend fun getCrashPullDeferUntil(): TimeInMillis? =
        if (settings.crashPullNeverShowAgain) {
            // defer this forever
            Long.MAX_VALUE
        } else {
            settings.crashPullDontShowBefore
        }

    override suspend fun getReportOption(): CrashReportOption = try {
        CrashReportOption.fromLabel(settings.crashReportChoice)
    } catch (e: IllegalArgumentException) {
        CrashReportOption.Never
    }

    override suspend fun setReportOption(option: CrashReportOption) = withContext(Dispatchers.IO) {
        settings.crashReportChoice = option.toString()
    }
}

/**
 * Extension function to convert the crash report choice from settings to a [CrashReportOption].
 */
fun Settings.crashReportOption(): CrashReportOption =
    CrashReportOption.fromLabel(crashReportChoice)
