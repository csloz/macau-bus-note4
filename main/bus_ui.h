// On-device settings UI: menu, Wi-Fi picker + password editor, stop code
// editor, and route picker, driven by the three NOTE4 keys.
#pragma once

#include "bus_config.h"

// Starts the UI task. The UI paints to the shared display; while a page other
// than the card is open, BusUiActive() is true and the main loop must not
// paint. When the user exits back to the card, `on_exit` fires (from the UI
// task) so the app can repaint immediately.
void BusUiStart(BusConfig* config, void (*on_exit)());

// Programmatically open the settings menu (console `bus-ui` command); does
// nothing if the UI is already active.
void BusUiOpenMenu();

// Inject a synthetic key event (console `bus-key` command) for testing:
// key = 0 up, 1 down, 2 ok; long = press-and-hold instead of click.
void BusUiInjectKey(int key, bool long_press);

bool BusUiActive();
