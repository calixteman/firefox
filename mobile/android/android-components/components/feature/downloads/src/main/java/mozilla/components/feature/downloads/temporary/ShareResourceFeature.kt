/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package mozilla.components.feature.downloads.temporary

import android.content.Context
import androidx.annotation.VisibleForTesting
import kotlinx.coroutines.CoroutineDispatcher
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.distinctUntilChangedBy
import kotlinx.coroutines.flow.mapNotNull
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeout
import mozilla.components.browser.state.action.BrowserAction
import mozilla.components.browser.state.action.ShareResourceAction
import mozilla.components.browser.state.selector.findTabOrCustomTabOrSelectedTab
import mozilla.components.browser.state.state.content.ShareResourceState
import mozilla.components.browser.state.store.BrowserStore
import mozilla.components.concept.fetch.Client
import mozilla.components.lib.state.ext.flowScoped
import mozilla.components.support.base.feature.LifecycleAwareFeature
import mozilla.components.support.ktx.android.content.shareLocalPdf
import mozilla.components.support.ktx.android.content.shareMedia

/**
 * At most time to allow for the file to be downloaded and action to be performed.
 */
private const val OPERATION_TIMEOUT_MS: Long = 1000L

/**
 * [LifecycleAwareFeature] implementation for sharing online and local resources.
 *
 * This will intercept only [ShareResourceAction] [BrowserAction]s.
 *
 * This [ShareResourceFeature] can handle two different cases:
 * 1) In the case of an online resource, it will transparently
 *  - download internet resources while respecting the private mode related to cookies handling
 *  - temporarily cache the downloaded resources
 *  - automatically open the platform app chooser to share the cached files with other installed Android apps
 * with a 1 second timeout to ensure a smooth UX.
 *
 * To finish the process in this small timeframe the feature is recommended to be used only for images,
 * PDFs, or other small files.
 *
 * 2) In the case of a local resource (currently, specifically PDFs):
 *  - automatically open the platform app chooser to share the local file with other installed Android apps.
 *
 *  @property context Android context used for various platform interactions
 *  @property store a reference to the application's [BrowserStore]
 *  @property tabId ID of the tab session, or null if the selected session should be used.
 *  @param httpClient Client used for downloading internet resources
 *  @param ioDispatcher Coroutine dispatcher used for IO operations like the download operation
 *  and cleanup of old cached files. Defaults to IO.
 */
class ShareResourceFeature(
    private val context: Context,
    private val store: BrowserStore,
    private val tabId: String?,
    httpClient: Client,
    ioDispatcher: CoroutineDispatcher = Dispatchers.IO,
) : TemporaryDownloadFeature(
    context = context,
    httpClient = httpClient,
    ioDispatcher = ioDispatcher,
) {

    override fun start() {
        scope = store.flowScoped { flow ->
            flow.mapNotNull { state -> state.findTabOrCustomTabOrSelectedTab(tabId) }
                .distinctUntilChangedBy { it.content.share }
                .collect { state ->
                    state.content.share?.let { shareState ->
                        logger.debug("Starting the sharing process")
                        startSharing(shareState)

                        // This is a fire and forget action, not something that we want lingering the tab state.
                        store.dispatch(ShareResourceAction.ConsumeShareAction(state.id))
                    }
                }
        }
    }

    @VisibleForTesting
    internal fun startSharing(internetResource: ShareResourceState) {
        val coroutineExceptionHandler = coroutineExceptionHandler("Share")

        scope?.launch(coroutineExceptionHandler) {
            when (internetResource) {
                is ShareResourceState.InternetResource -> {
                    withTimeout(OPERATION_TIMEOUT_MS) {
                        val download = download(internetResource)
                        shareInternetResource(
                            contentType = internetResource.contentType,
                            filePath = download.canonicalPath,
                        )
                    }
                }
                is ShareResourceState.LocalResource ->
                    shareLocalPdf(internetResource.url, internetResource.contentType)
            }
        }
    }

    @VisibleForTesting
    internal fun shareInternetResource(
        filePath: String,
        contentType: String?,
        subject: String? = null,
        message: String? = null,
    ) = context.shareMedia(filePath, contentType, subject, message)

    @VisibleForTesting
    internal fun shareLocalPdf(
        filePath: String,
        contentType: String?,
    ) = context.shareLocalPdf(filePath, contentType)
}
