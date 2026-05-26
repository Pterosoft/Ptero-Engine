#pragma once

#include <string>

// SplashScreen: borderless topmost window displayed during editor startup.
// Call Show() once to create the window on a background thread.
// Call UpdateStatus() to update the status text shown on the splash.
// Call Close() to destroy the splash window once the editor is ready.
namespace SplashScreen
{
    // Launches the splash window asynchronously. Returns immediately.
    void Show(HINSTANCE hInstance);

    // Updates the status message rendered below the splash image.
    // Thread-safe: may be called from any thread.
    void UpdateStatus(const wchar_t* message);

    // Destroys the splash window and waits for its thread to exit.
    void Close();
}
