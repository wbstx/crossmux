#include <gtest/gtest.h>

#include "DirectPixelWriter.h"
#include "GfxRenderer.h"

namespace {

bool mappedBit(const GfxRenderer::RenderMode mode, const GfxRenderer::TwoBitPixel pixel) {
  return pixel.draw && !GfxRenderer::framebufferState(mode, pixel.state);
}

DirectPixelWriter identityWriter(uint8_t* framebuffer, const GfxRenderer::RenderMode mode, const bool absolute) {
  DirectPixelWriter writer{};
  writer.fb = framebuffer;
  writer.mode = mode;
  writer.absolute = absolute;
  writer.displayWidthBytes = 1;
  writer.originY = 0;
  writer.clipRows = 1;
  writer.phyXBase = 0;
  writer.phyYBase = 0;
  writer.phyXStepX = 1;
  writer.phyYStepX = 0;
  writer.phyXStepY = 0;
  writer.phyYStepY = 1;
  writer.beginRow(0);
  return writer;
}

bool directBit(const GfxRenderer::RenderMode mode, const uint8_t value) {
  uint8_t framebuffer = 0;
  identityWriter(&framebuffer, mode, false).writePixel(0, value);
  return (framebuffer & 0x80) != 0;
}

}  // namespace

TEST(GfxTwoBitMapping, A4DirectGlyphAndBitmapProduceTheSameFramebufferBit) {
  for (const auto mode : {GfxRenderer::GRAYSCALE_MSB, GfxRenderer::GRAYSCALE_LSB}) {
    for (uint8_t value = 0; value < 4; ++value) {
      const bool direct = directBit(mode, value);
      const bool glyph = mappedBit(mode, GfxRenderer::mapTwoBitGlyphCoverage(mode, 3 - value));
      const bool bitmap = mappedBit(mode, GfxRenderer::mapTwoBitPixel(mode, value));
      EXPECT_EQ(direct, glyph) << "mode=" << mode << " value=" << static_cast<int>(value);
      EXPECT_EQ(direct, bitmap) << "mode=" << mode << " value=" << static_cast<int>(value);
    }
  }
}

TEST(GfxTwoBitMapping, AbsolutePlanesStoreEveryLevelOnAWhiteClear) {
  // A4 grayscale inverts framebuffer bits (set = black). Overlay only sets
  // bits, so a 0xFF clear stays white. Absolute mode stores all four levels.
  for (const auto mode : {GfxRenderer::GRAYSCALE_MSB, GfxRenderer::GRAYSCALE_LSB}) {
    uint8_t overlay = 0xFF;
    auto overlayWriter = identityWriter(&overlay, mode, false);
    uint8_t absolute = 0xFF;
    auto absoluteWriter = identityWriter(&absolute, mode, true);
    for (uint8_t value = 0; value < 4; ++value) {
      overlayWriter.writePixel(value, value);
      absoluteWriter.writePixel(value, value);
    }
    EXPECT_EQ(overlay, 0xFF) << "mode=" << mode;
    const uint8_t expected = mode == GfxRenderer::GRAYSCALE_MSB ? 0xCF : 0xAF;
    EXPECT_EQ(absolute, expected) << "mode=" << mode;
  }
}

TEST(GfxTwoBitMapping, BwKeepsLogicalPixelState) {
  for (uint8_t value = 0; value < 4; ++value) {
    const auto pixel = GfxRenderer::mapTwoBitPixel(GfxRenderer::BW, value);
    EXPECT_EQ(pixel.draw, value < 3);
    EXPECT_TRUE(GfxRenderer::framebufferState(GfxRenderer::BW, pixel.state));
  }
}
