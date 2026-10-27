// Minimal u8g2 font decoder — renders glyphs from a bdfconv-generated font
// blob (u8g2 RLE bitmap format) into the 1bpp HearthCanvas.
//
// Only what bus_ui needs: lookup + draw + measure for UTF-8 strings.
// Format reference: u8g2 csrc/u8g2_font.c (decode_len / get_unsigned_bits).
#pragma once

#include <cstdint>

class HearthCanvas;

namespace u8g2font {

// Returns the byte offset of the glyph data for `enc`, or -1 if absent.
int GlyphPos(const uint8_t* font, uint16_t enc);

// Draw one glyph with its BASELINE at (x, y_base). Returns advance (dx) px.
// Unknown glyphs draw as a 12px hollow box and advance 12.
int DrawGlyph(HearthCanvas& c, const uint8_t* font, int x, int y_base,
              uint16_t enc, bool inverted);

// Draw UTF-8 text with the BASELINE of the first line at (x, y_base).
// Stops before exceeding max_px. Returns the end x.
int DrawText(HearthCanvas& c, const uint8_t* font, int x, int y_base,
             const char* utf8, int max_px, bool inverted);

// Pixel width of a UTF-8 string in this font.
int TextWidth(const uint8_t* font, const char* utf8);

}  // namespace u8g2font
