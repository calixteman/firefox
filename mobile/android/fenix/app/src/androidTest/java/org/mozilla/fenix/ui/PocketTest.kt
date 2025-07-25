package org.mozilla.fenix.ui

import androidx.compose.ui.test.junit4.AndroidComposeTestRule
import org.junit.Before
import org.junit.Rule
import org.junit.Test
import org.mozilla.fenix.helpers.AppAndSystemHelper.isNetworkConnected
import org.mozilla.fenix.helpers.AppAndSystemHelper.runWithCondition
import org.mozilla.fenix.helpers.Constants
import org.mozilla.fenix.helpers.Constants.RETRY_COUNT
import org.mozilla.fenix.helpers.HomeActivityTestRule
import org.mozilla.fenix.helpers.RetryTestRule
import org.mozilla.fenix.helpers.TestHelper.waitForAppWindowToBeUpdated
import org.mozilla.fenix.helpers.TestSetup
import org.mozilla.fenix.helpers.perf.DetectMemoryLeaksRule
import org.mozilla.fenix.ui.robots.homeScreen

/**
 *  Tests for verifying the presence of the Pocket section and its elements
 */

class PocketTest : TestSetup() {
    @get:Rule(order = 0)
    val activityTestRule =
        AndroidComposeTestRule(
            HomeActivityTestRule(
                isRecentTabsFeatureEnabled = false,
                isRecentlyVisitedFeatureEnabled = false,
            ),
        ) { it.activity }

    @get:Rule(order = 1)
    val memoryLeaksRule = DetectMemoryLeaksRule()

    @Rule(order = 2)
    @JvmField
    val retryTestRule = RetryTestRule(3)

    @Before
    override fun setUp() {
        super.setUp()
        // Workaround to make sure the Pocket articles are populated before starting the tests.
        for (i in 1..RETRY_COUNT) {
            try {
                homeScreen {
                }.openThreeDotMenu {
                }.openSettings {
                }.goBack {
                    verifyThoughtProvokingStories(true)
                }

                break
            } catch (e: AssertionError) {
                if (i == RETRY_COUNT) {
                    throw e
                } else {
                    waitForAppWindowToBeUpdated()
                }
            }
        }
    }

    // TestRail link: https://mozilla.testrail.io/index.php?/cases/view/2252509
    @Test
    fun verifyPocketSectionTest() {
        runWithCondition(isNetworkConnected()) {
            homeScreen {
                verifyThoughtProvokingStories(true)
                verifyPocketRecommendedStoriesItems(activityTestRule)
                // Sponsored Pocket stories are only advertised for a limited time.
                // See also known issue https://bugzilla.mozilla.org/show_bug.cgi?id=1828629
                // verifyPocketSponsoredStoriesItems(2, 8)
            }.openThreeDotMenu {
            }.openCustomizeHome {
                clickPocketButton()
            }.goBackToHomeScreen {
                verifyThoughtProvokingStories(false)
            }
        }
    }

    // TestRail link: https://mozilla.testrail.io/index.php?/cases/view/2252513
    @Test
    fun openPocketStoryItemTest() {
        runWithCondition(isNetworkConnected()) {
            homeScreen {
                verifyThoughtProvokingStories(true)
            }.clickPocketStoryItem(1) {
                verifyUrl(Constants.STORIES_UTM_PARAM)
            }
        }
    }
}
