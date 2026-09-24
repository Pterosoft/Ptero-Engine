#pragma once

#include "VideoAPI.h"

#include <string>

// The editor's video preview: a stand-alone window with transport controls.
//
// It runs on a thread of its own with its own message loop, so it keeps playing smoothly
// whatever the editor's frame rate is, and it never touches Qt or the renderer.
//
// Controls: Space play/pause, Left/Right skip 5 s, Home rewind, L loop, drag the timeline
// to scrub.
namespace VideoPlayerWindow
{
    // Shows the window and starts playing `path`, replacing whatever it was playing.
    VIDEO_API bool Open(const std::wstring& path);
    // Closes the window and waits for its thread. Call before the process tears down.
    VIDEO_API void Shutdown();
}
