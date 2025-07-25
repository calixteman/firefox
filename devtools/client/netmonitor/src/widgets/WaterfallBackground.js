/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

"use strict";

const {
  getCssVariableColor,
} = require("resource://devtools/client/shared/theme.js");
const {
  REQUESTS_WATERFALL,
} = require("resource://devtools/client/netmonitor/src/constants.js");

const HTML_NS = "http://www.w3.org/1999/xhtml";
const STATE_KEYS = [
  "firstRequestStartedMs",
  "scale",
  "timingMarkers",
  "waterfallWidth",
];

/**
 * Creates the background displayed on each waterfall view in this container.
 */
class WaterfallBackground {
  constructor() {
    this.canvas = document.createElementNS(HTML_NS, "canvas");
    this.ctx = this.canvas.getContext("2d");
    this.prevState = {};
  }

  /**
   * Changes the element being used as the CSS background for a background
   * with a given background element ID.
   *
   * The funtion wrap the Firefox only API. Waterfall Will not draw the
   * vertical line when running on non-firefox browser.
   * Could be fixed by Bug 1308695
   */
  setImageElement(imageElementId, imageElement) {
    if (document.mozSetImageElement) {
      document.mozSetImageElement(imageElementId, imageElement);
    }
  }

  draw(state) {
    // Do a shallow compare of the previous and the new state
    const shouldUpdate = STATE_KEYS.some(
      key => this.prevState[key] !== state[key]
    );
    if (!shouldUpdate) {
      return;
    }

    this.prevState = state;

    if (state.waterfallWidth === null || state.scale === null) {
      this.setImageElement("waterfall-background", null);
      return;
    }

    // Nuke the context.
    const canvasWidth = (this.canvas.width = Math.max(
      state.waterfallWidth - REQUESTS_WATERFALL.LABEL_WIDTH,
      1
    ));
    // Awww yeah, 1px, repeats on Y axis.
    const canvasHeight = (this.canvas.height = 1);

    // Start over.
    const imageData = this.ctx.createImageData(canvasWidth, canvasHeight);
    const pixelArray = imageData.data;

    const buf = new ArrayBuffer(pixelArray.length);
    const view8bit = new Uint8ClampedArray(buf);
    const view32bit = new Uint32Array(buf);

    // Build new millisecond tick lines...
    let timingStep = REQUESTS_WATERFALL.BACKGROUND_TICKS_MULTIPLE;
    let optimalTickIntervalFound = false;
    let scaledStep;

    while (!optimalTickIntervalFound) {
      // Ignore any divisions that would end up being too close to each other.
      scaledStep = state.scale * timingStep;
      if (scaledStep < REQUESTS_WATERFALL.BACKGROUND_TICKS_SPACING_MIN) {
        timingStep <<= 1;
        continue;
      }
      optimalTickIntervalFound = true;
    }

    const isRTL = document.dir === "rtl";
    const [r, g, b] = REQUESTS_WATERFALL.BACKGROUND_TICKS_COLOR_RGB;
    let alphaComponent = REQUESTS_WATERFALL.BACKGROUND_TICKS_OPACITY_MIN;

    function drawPixelAt(offset, color) {
      const position = (isRTL ? canvasWidth - offset : offset - 1) | 0;
      const [rc, gc, bc, ac] = color;
      view32bit[position] = (ac << 24) | (bc << 16) | (gc << 8) | rc;
    }

    // Insert one pixel for each division on each scale.
    for (let i = 1; i <= REQUESTS_WATERFALL.BACKGROUND_TICKS_SCALES; i++) {
      const increment = scaledStep * Math.pow(2, i);
      for (let x = 0; x < canvasWidth; x += increment) {
        drawPixelAt(x, [r, g, b, alphaComponent]);
      }
      alphaComponent += REQUESTS_WATERFALL.BACKGROUND_TICKS_OPACITY_ADD;
    }

    function drawTimestamp(timestamp, color) {
      if (timestamp === -1) {
        return;
      }

      const delta = Math.floor(
        (timestamp - state.firstRequestStartedMs) * state.scale
      );
      drawPixelAt(delta, color);
    }

    const { DOMCONTENTLOADED_TICKS_COLOR, LOAD_TICKS_COLOR } =
      REQUESTS_WATERFALL;
    drawTimestamp(
      state.timingMarkers.firstDocumentDOMContentLoadedTimestamp,
      this.getThemeColorAsRgba(DOMCONTENTLOADED_TICKS_COLOR)
    );

    drawTimestamp(
      state.timingMarkers.firstDocumentLoadTimestamp,
      this.getThemeColorAsRgba(LOAD_TICKS_COLOR)
    );

    // Flush the image data and cache the waterfall background.
    pixelArray.set(view8bit);
    try {
      this.ctx.putImageData(imageData, 0, 0);
    } catch (e) {
      console.error("WaterfallBackground crash error", e);
    }

    this.setImageElement("waterfall-background", this.canvas);
  }

  /**
   * Retrieve a color defined for the provided theme as a rgba array.
   *
   * @param {String} colorVariableName
   *        The name of the variable defining the color
   * @return {Array} RGBA array for the color.
   */
  getThemeColorAsRgba(colorVariableName) {
    const colorStr = getCssVariableColor(
      colorVariableName,
      document.ownerGlobal
    );
    const { r, g, b, a } =
      InspectorUtils.colorToRGBA(colorStr) ||
      // In theory we shouldn't get null as a result, but we got reports that it was in
      // some cases (Bug 1924882, Bug 1973307).
      // Until we actually get to the cause of this, let's use a default color that works
      // for both light and dark themes.
      InspectorUtils.colorToRGBA("#888");
    return [r, g, b, a * 255];
  }

  destroy() {
    this.setImageElement("waterfall-background", null);
  }
}

module.exports = WaterfallBackground;
