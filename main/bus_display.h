// Shared e-paper display access for the card renderer and the button UI.
#pragma once

#include "hearth_canvas.h"
#include "zectrix_epd.h"

// Bind the shared canvas to the panel driver handle.
void BusDisplayInit(zectrix_epd_handle_t epd);

// The single shared 400x300 1bpp canvas. Render a full frame, then call
// BusDisplayPaint (or BusDisplayPaintRows for a fast partial band update).
HearthCanvas& BusDisplayCanvas();

// Full-screen refresh. Powers the panel on first if needed. Blocks until the
// panel reports busy-release (~2-3 s).
void BusDisplayPaint();

// Partial refresh of the row band [y0, y1). The canvas row stride matches the
// panel's packed row width, so the band is refreshed straight from the canvas.
// A successful full refresh must have happened since boot before this works.
void BusDisplayPaintRows(int y0, int y1);
