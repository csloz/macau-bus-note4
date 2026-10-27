// Button driver for the three NOTE4 keys (UP GPIO39, DOWN GPIO18, OK GPIO0).
#pragma once

enum BusKey {
    kKeyUp = 0,
    kKeyDown = 1,
    kKeyOk = 2,
};

enum BusKeyEventType {
    kPressClick = 0,  // released below the long-press threshold
    kPressLong = 1,   // held past the long-press threshold
};

struct BusKeyEvent {
    BusKey key;
    BusKeyEventType type;
};

// Starts the polling task. Events are delivered on the given callback from the
// button task context. UP/DOWN auto-repeat while held (clicks every ~130 ms
// after a 500 ms delay); OK reports click vs long-press.
void BusButtonsStart(void (*on_event)(BusKeyEvent event));
