#include "u8g2_font.h"

#include <cstring>

#include "hearth_canvas.h"

namespace u8g2font {
namespace {

// Sequential-bit reader over the RLE glyph stream (u8g2 get_unsigned_bits).
struct BitReader {
    const uint8_t* p;
    int bit_pos;

    uint16_t UBits(int cnt) {
        uint16_t val = static_cast<uint16_t>(p[0] >> bit_pos);
        int bp = bit_pos + cnt;
        if (bp >= 8) {
            ++p;
            val |= static_cast<uint16_t>(static_cast<uint16_t>(p[0])
                                         << (8 - bit_pos));
            bp -= 8;
        }
        bit_pos = bp;
        return val & static_cast<uint16_t>((1u << cnt) - 1u);
    }

    int16_t SBits(int cnt) {
        return static_cast<int16_t>(UBits(cnt) - (1u << (cnt - 1)));
    }
};

// RLE row walker: draws fg/bg runs, wrapping at the glyph width.
struct GlyphDraw {
    HearthCanvas& c;
    int w;
    int h;
    int x0;
    int y_top;
    bool inverted;
    int lx = 0;
    int ly = 0;

    void Run(int cnt, bool fg) {
        for (;;) {
            const int rem = w - lx;
            const int cur = (cnt < rem) ? cnt : rem;
            if (fg && cur > 0) {
                for (int i = 0; i < cur; i++) {
                    const int px = x0 + lx + i;
                    const int py = y_top + ly;
                    if (px >= 0 && px < HearthCanvas::kWidth && py >= 0 &&
                        py < HearthCanvas::kHeight) {
                        c.Pixel(px, py, !inverted);
                    }
                }
            }
            if (cnt < rem) break;
            cnt -= rem;
            lx = 0;
            ly++;
        }
        lx += cnt;
    }
};

struct GlyphMetrics {
    int w;
    int h;
    int x_off;
    int y_off;  // signed: pixels below baseline at the glyph bottom
    int dx;
    BitReader br;
};

// Reads W/H/X/Y/DX from the glyph header; BitReader is left at the RLE data.
GlyphMetrics GlyphHeader(const uint8_t* font, int pos) {
    GlyphMetrics m;
    m.br = BitReader{font + pos, 0};
    m.w = m.br.UBits(font[4]);
    m.h = m.br.UBits(font[5]);
    m.x_off = m.br.SBits(font[6]);
    m.y_off = m.br.SBits(font[7]);
    m.dx = m.br.SBits(font[8]);
    return m;
}

}  // namespace

int GlyphPos(const uint8_t* font, uint16_t enc) {
    if (enc <= 255) {
        // Linear scan of the single-byte section (jump byte 0 terminates).
        int pos = 23;
        while (pos + 1 < 65536) {
            const uint8_t jump = font[pos + 1];
            if (jump == 0) return -1;
            if (font[pos] == (enc & 0xFF)) return pos + 2;
            pos += jump;
        }
        return -1;
    }
    // Unicode section: block table at start_pos_unicode (bytes 21..22, BE).
    int font_pos = 23 + ((static_cast<int>(font[21]) << 8) | font[22]);
    int table = font_pos;
    for (;;) {
        if (table + 3 >= 65536) break;
        font_pos += (static_cast<int>(font[table]) << 8) | font[table + 1];
        const uint16_t bound =
            static_cast<uint16_t>((static_cast<int>(font[table + 2]) << 8) |
                                  font[table + 3]);
        table += 4;
        if (!(bound < enc)) break;
    }
    // Linear scan of glyph entries inside the block (bounded by data end).
    while (font_pos + 2 < 65536) {
        const uint16_t e = static_cast<uint16_t>(
            (static_cast<int>(font[font_pos]) << 8) | font[font_pos + 1]);
        if (e == 0) return -1;
        if (e == enc) return font_pos + 3;
        font_pos += font[font_pos + 2];
    }
    return -1;
}

int DrawGlyph(HearthCanvas& c, const uint8_t* font, int x, int y_base,
              uint16_t enc, bool inverted) {
    const int pos = GlyphPos(font, enc);
    if (pos < 0) {
        // Placeholder: hollow 12x11 box on the baseline.
        c.Rect(x, y_base - 10, 12, 11, !inverted);
        return 12;
    }
    GlyphMetrics m = GlyphHeader(font, pos);
    if (m.w == 0) return m.dx;
    GlyphDraw d{c, m.w, m.h, x + m.x_off, y_base - (m.h + m.y_off), inverted};
    for (;;) {
        const int a = m.br.UBits(font[2]);
        const int b = m.br.UBits(font[3]);
        do {
            d.Run(a, false);
            d.Run(b, true);
        } while (m.br.UBits(1) != 0);
        if (d.ly >= m.h) break;
    }
    return m.dx;
}

int DrawText(HearthCanvas& c, const uint8_t* font, int x, int y_base,
             const char* utf8, int max_px, bool inverted) {
    int i = 0;
    while (utf8[i] != '\0' && x < max_px) {
        const uint8_t b = static_cast<uint8_t>(utf8[i]);
        int len = 1;
        uint16_t cp = 0xFFFD;
        if (b < 0x80) {
            cp = b;
        } else if ((b & 0xE0) == 0xC0 && (utf8[i + 1] & 0xC0) == 0x80) {
            len = 2;
            cp = ((b & 0x1F) << 6) | (utf8[i + 1] & 0x3F);
        } else if ((b & 0xF0) == 0xE0 && (utf8[i + 1] & 0xC0) == 0x80 &&
                   (utf8[i + 2] & 0xC0) == 0x80) {
            len = 3;
            cp = ((b & 0x0F) << 12) | ((utf8[i + 1] & 0x3F) << 6) |
                 (utf8[i + 2] & 0x3F);
        }
        x += DrawGlyph(c, font, x, y_base, cp, inverted);
        i += len;
    }
    return x;
}

int TextWidth(const uint8_t* font, const char* utf8) {
    int w = 0;
    int i = 0;
    while (utf8[i] != '\0') {
        const uint8_t b = static_cast<uint8_t>(utf8[i]);
        int len = 1;
        uint16_t cp = 0xFFFD;
        if (b < 0x80) {
            cp = b;
        } else if ((b & 0xE0) == 0xC0) {
            len = 2;
            cp = ((b & 0x1F) << 6) | (utf8[i + 1] & 0x3F);
        } else if ((b & 0xF0) == 0xE0) {
            len = 3;
            cp = ((b & 0x0F) << 12) | ((utf8[i + 1] & 0x3F) << 6) |
                 (utf8[i + 2] & 0x3F);
        }
        const int pos = GlyphPos(font, cp);
        if (pos < 0) {
            w += 12;
        } else {
            w += GlyphHeader(font, pos).dx;
        }
        i += len;
    }
    return w;
}

}  // namespace u8g2font
