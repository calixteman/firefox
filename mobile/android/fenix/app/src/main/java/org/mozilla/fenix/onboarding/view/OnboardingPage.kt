/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package org.mozilla.fenix.onboarding.view

import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.BoxWithConstraintsScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.testTag
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import mozilla.components.compose.base.annotation.FlexibleWindowLightDarkPreview
import mozilla.components.compose.base.button.PrimaryButton
import mozilla.components.compose.base.button.SecondaryButton
import org.mozilla.fenix.R
import org.mozilla.fenix.compose.LinkText
import org.mozilla.fenix.theme.FirefoxTheme

/**
 * The ratio of the image height to the parent height. This was determined from the designs in figma
 * taking the ratio of the image height to the mockup height.
 */
private const val IMAGE_HEIGHT_RATIO_DEFAULT = 0.4f

/**
 * The ratio of the image height to the parent height for medium sized devices.
 */
private const val IMAGE_HEIGHT_RATIO_MEDIUM = 0.36f

/**
 * The ratio of the image height to the parent height for small devices like Nexus 4, Nexus 1.
 */
private const val IMAGE_HEIGHT_RATIO_SMALL = 0.28f

/**
 * The small device height definition for onboarding in dp.
 */
val ONBOARDING_SMALL_DEVICE = 550.dp

/**
 * The medium device height definition for onboarding in dp.
 */
val ONBOARDING_MEDIUM_DEVICE = 650.dp

/**
 * A composable for displaying onboarding page content.
 *
 * @param pageState [OnboardingPageState] The page content that's displayed.
 * @param modifier The modifier to be applied to the Composable.
 * @param onDismiss Invoked when the user clicks the close button. This defaults to null. When null,
 * it doesn't show the close button.
 */
@Composable
@Suppress("LongMethod")
fun OnboardingPage(
    pageState: OnboardingPageState,
    modifier: Modifier = Modifier,
    onDismiss: (() -> Unit)? = null,
) {
    BoxWithConstraints(
        modifier = Modifier
            .background(FirefoxTheme.colors.layer1)
            .padding(bottom = if (pageState.secondaryButton == null) 32.dp else 24.dp)
            .then(modifier),
    ) {
        val boxWithConstraintsScope = this
        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState()),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.SpaceBetween,
        ) {
            if (onDismiss != null) {
                IconButton(
                    onClick = onDismiss,
                    modifier = Modifier.align(Alignment.End),
                ) {
                    Icon(
                        painter = painterResource(id = R.drawable.mozac_ic_cross_24),
                        contentDescription = stringResource(R.string.onboarding_home_content_description_close_button),
                        tint = FirefoxTheme.colors.iconPrimary,
                    )
                }
            } else {
                Spacer(Modifier)
            }

            Column(
                modifier = Modifier.padding(horizontal = 16.dp, vertical = 32.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
            ) {
                Image(
                    painter = painterResource(id = pageState.imageRes),
                    contentDescription = null,
                    modifier = Modifier.height(imageHeight(boxWithConstraintsScope)),
                )

                Spacer(modifier = Modifier.height(32.dp))

                Text(
                    text = pageState.title,
                    color = FirefoxTheme.colors.textPrimary,
                    textAlign = TextAlign.Center,
                    style = FirefoxTheme.typography.headline5,
                )

                Spacer(modifier = Modifier.height(16.dp))

                Text(
                    text = pageState.description,
                    color = FirefoxTheme.colors.textSecondary,
                    textAlign = TextAlign.Center,
                    style = FirefoxTheme.typography.body2,
                )

                Spacer(modifier = Modifier.height(16.dp))

                pageState.privacyCaption?.let { privacyCaption ->
                    LinkText(
                        text = privacyCaption.text,
                        linkTextStates = listOf(privacyCaption.linkTextState),
                    )
                }
            }

            Column(
                horizontalAlignment = Alignment.CenterHorizontally,
                modifier = Modifier.padding(horizontal = 16.dp),
            ) {
                PrimaryButton(
                    modifier = Modifier
                        .width(width = FirefoxTheme.layout.size.maxWidth.small)
                        .semantics {
                            testTag = pageState.title + "onboarding_card.positive_button"
                        },
                    text = pageState.primaryButton.text,
                    onClick = pageState.primaryButton.onClick,
                )

                if (pageState.secondaryButton != null) {
                    Spacer(modifier = Modifier.height(8.dp))
                    SecondaryButton(
                        modifier = Modifier
                            .width(width = FirefoxTheme.layout.size.maxWidth.small)
                            .semantics {
                                testTag = pageState.title + "onboarding_card.negative_button"
                            },
                        text = pageState.secondaryButton.text,
                        onClick = pageState.secondaryButton.onClick,
                    )
                }
            }

            LaunchedEffect(pageState) {
                pageState.onRecordImpressionEvent()
            }
        }
    }
}

/**
 * Calculates the image height to be set. The ratio is selected based on parent height.
 */
fun imageHeight(boxWithConstraintsScope: BoxWithConstraintsScope): Dp {
    val imageHeightRatio: Float = when {
        boxWithConstraintsScope.maxHeight <= ONBOARDING_SMALL_DEVICE -> IMAGE_HEIGHT_RATIO_SMALL
        boxWithConstraintsScope.maxHeight <= ONBOARDING_MEDIUM_DEVICE -> IMAGE_HEIGHT_RATIO_MEDIUM
        else -> IMAGE_HEIGHT_RATIO_DEFAULT
    }
    return boxWithConstraintsScope.maxHeight.times(imageHeightRatio)
}

@FlexibleWindowLightDarkPreview
@Composable
private fun OnboardingPagePreview() {
    FirefoxTheme {
        OnboardingPage(
            pageState = OnboardingPageState(
                imageRes = R.drawable.ic_notification_permission,
                title = stringResource(
                    id = R.string.onboarding_home_welcome_title_2,
                    formatArgs = arrayOf(stringResource(R.string.app_name)),
                ),
                description = stringResource(
                    id = R.string.onboarding_home_welcome_description,
                    formatArgs = arrayOf(stringResource(R.string.app_name)),
                ),
                primaryButton = Action(
                    text = stringResource(
                        id = R.string.onboarding_home_get_started_button,
                    ),
                    onClick = {},
                ),
                secondaryButton = Action(
                    text = stringResource(id = R.string.onboarding_home_skip_button),
                    onClick = {},
                ),
                onRecordImpressionEvent = {},
            ),
            onDismiss = {},
        )
    }
}
