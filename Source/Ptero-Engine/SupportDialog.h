#pragma once

// SupportDialog: the "support Ptero-Engine on Patreon" window shown once the editor
// is up, styled after the support section of pteroengine.pterosoftstudio.com.
// Only the public build (PTERO_PUBLIC_RELEASE, the Public|x64 configuration) shows it;
// the internal Editor.exe never calls in here.
namespace SupportDialog
{
    // Opens the dialog over the editor window and returns immediately. The owner stays
    // disabled until the dialog is dismissed; Continue only unlocks after a short delay,
    // or straight away once the Patreon page has been opened.
    void Show(HINSTANCE hInstance, HWND owner);
}
