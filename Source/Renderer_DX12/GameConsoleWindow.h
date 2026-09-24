#pragma once

#include <windows.h>

// The packaged game's console: a native drop-down over the top of the game window with
// the session log and a command line wired to the cvar registry - the same stream and
// the same commands as the editor's Console panel, which is Qt and so does not exist in
// a shipped game (see QtUiHeadless.cpp).
//
// Owned by the render thread, like everything else in the renderer's frame loop; its
// window messages arrive through the host's own message pump.
namespace GameConsoleWindow
{
    // Call once per frame. Shows or hides the console to match `visible` and keeps it
    // docked to the host's client area. Returns the visibility the console wants next
    // frame - false once the player has pressed Escape in it.
    bool Update(HWND host, bool visible);

    // True while the console's command line has the keyboard, so the tilde toggle and
    // game input can tell typing from playing.
    bool HasFocus();

    void Shutdown();
}
