#pragma once

#include <windows.h>

// The packaged game's splash screen: the image the editor's Build Game dialog embedded in
// this exe (RT_RCDATA 300, a PNG), shown centred on the primary monitor from launch until
// the game's first frame is on screen. A borderless, topmost, per-pixel-alpha window on a
// thread of its own, so it stays responsive while the main thread starts the renderer.
// An exe built without a splash image simply shows nothing.
namespace GameSplash
{
    // Returns immediately; false when this exe has no splash image (or it will not decode).
    bool Show(HINSTANCE instance);

    // Takes the splash down and waits for its thread. Safe to call when none is showing.
    void Close();
}
