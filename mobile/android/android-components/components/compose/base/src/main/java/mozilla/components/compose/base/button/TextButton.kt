/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

package mozilla.components.compose.base.button

import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Column
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.tooling.preview.PreviewLightDark
import mozilla.components.compose.base.theme.AcornTheme
import java.util.Locale

/**
 * Text-only button.
 *
 * @param text The button text to be displayed.
 * @param onClick Invoked when the user clicks on the button.
 * @param modifier [Modifier] Used to shape and position the underlying [androidx.compose.material3.TextButton].
 * @param enabled Controls the enabled state of the button. When `false`, this button will not
 * be clickable.
 * @param textColor [Color] to apply to the button text.
 * @param upperCaseText If the button text should be in uppercase letters.
 */
@Composable
fun TextButton(
    text: String,
    onClick: () -> Unit,
    modifier: Modifier = Modifier,
    enabled: Boolean = true,
    textColor: Color = AcornTheme.colors.textAccent,
    upperCaseText: Boolean = true,
) {
    androidx.compose.material3.TextButton(
        onClick = onClick,
        modifier = modifier,
        enabled = enabled,
    ) {
        Text(
            text = if (upperCaseText) {
                text.uppercase(Locale.getDefault())
            } else {
                text
            },
            color = if (enabled) textColor else AcornTheme.colors.textDisabled,
            style = AcornTheme.typography.button,
            maxLines = 1,
        )
    }
}

@Composable
@PreviewLightDark
private fun TextButtonPreview() {
    AcornTheme {
        Column(Modifier.background(AcornTheme.colors.layer1)) {
            TextButton(
                text = "label",
                onClick = {},
            )

            TextButton(
                text = "disabled",
                onClick = {},
                enabled = false,
            )
        }
    }
}
