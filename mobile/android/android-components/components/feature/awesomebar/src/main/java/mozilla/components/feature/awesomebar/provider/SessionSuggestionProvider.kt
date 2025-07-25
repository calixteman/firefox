/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package mozilla.components.feature.awesomebar.provider

import android.graphics.drawable.Drawable
import android.net.Uri
import androidx.annotation.VisibleForTesting
import androidx.core.net.toUri
import mozilla.components.browser.icons.BrowserIcons
import mozilla.components.browser.icons.IconRequest
import mozilla.components.browser.state.state.BrowserState
import mozilla.components.browser.state.state.TabSessionState
import mozilla.components.browser.state.store.BrowserStore
import mozilla.components.concept.awesomebar.AwesomeBar
import mozilla.components.feature.awesomebar.facts.emitOpenTabSuggestionClickedFact
import mozilla.components.feature.tabs.TabsUseCases
import java.util.UUID

/**
 * A [AwesomeBar.SuggestionProvider] implementation that provides suggestions based on the sessions in the
 * [SessionManager] (Open tabs).
 */
class SessionSuggestionProvider(
    private val store: BrowserStore,
    private val selectTabUseCase: TabsUseCases.SelectTabUseCase,
    private val icons: BrowserIcons? = null,
    private val indicatorIcon: Drawable? = null,
    private val excludeSelectedSession: Boolean = false,
    private val suggestionsHeader: String? = null,
    private val switchToTabDescription: String,
    @get:VisibleForTesting val resultsUriFilter: ((Uri) -> Boolean)? = null,
) : AwesomeBar.SuggestionProvider {
    override val id: String = UUID.randomUUID().toString()

    override fun groupTitle(): String? {
        return suggestionsHeader
    }

    @Suppress("ComplexCondition")
    override suspend fun onInputChanged(text: String): List<AwesomeBar.Suggestion> {
        val searchText = text.trim()
        if (searchText.isEmpty()) {
            return emptyList()
        }

        val state = store.state
        val distinctTabs = state.tabs.asSequence().distinctBy { it.content.url }

        val suggestions = mutableListOf<AwesomeBar.Suggestion>()
        val searchWords = searchText.split(" ")

        distinctTabs.filter { item ->
            !item.content.private &&
                searchWords.all { item.contains(it) } &&
                resultsUriFilter?.invoke(item.content.url.toUri()) != false &&
                shouldIncludeSelectedTab(state, item)
        }.forEach { item ->
            val iconRequest = icons?.loadIcon(IconRequest(url = item.content.url, waitOnNetworkLoad = false))
            suggestions.add(
                AwesomeBar.Suggestion(
                    provider = this,
                    id = item.id,
                    title = item.content.title.ifBlank { item.content.url },
                    description = switchToTabDescription,
                    flags = setOf(AwesomeBar.Suggestion.Flag.OPEN_TAB),
                    icon = iconRequest?.await()?.bitmap,
                    indicatorIcon = indicatorIcon,
                    onSuggestionClicked = {
                        selectTabUseCase(item.id)
                        emitOpenTabSuggestionClickedFact()
                    },
                ),
            )
        }

        return suggestions
    }

    private fun TabSessionState.contains(text: String) =
        (content.url.contains(text, ignoreCase = true) || content.title.contains(text, ignoreCase = true))

    private fun shouldIncludeSelectedTab(state: BrowserState, tab: TabSessionState): Boolean {
        return if (excludeSelectedSession) {
            tab.id != state.selectedTabId
        } else {
            true
        }
    }
}
