// The SequencerEditor facade for the packaged game's renderer, which has no Qt and so no
// Sequencer window. Every call is a no-op; the game plays sequences through the
// CinematicSystem, never through the editor. Compiled in place of SequencerWindow.cpp,
// SequencerTimeline.cpp and SequencerDocument.cpp when Renderer_DX12 is built with
// PteroGameRuntime=true.

#include "SequencerEditor.h"

namespace SequencerEditor
{
void SetHost(SequencerEditorHost*) {}
void Show() {}
bool IsVisible() { return false; }
void Shutdown() {}
bool OpenSequence(const std::string&) { return false; }
void Tick(float) {}
void OnSceneChanged() {}
void OnLevelReplaced() {}
void OnLevelRestored() {}
void SuspendPreview() {}
void ResumePreview() {}
void SetCameraLock(bool) {}
bool IsCameraLocked() { return false; }
std::uint64_t ActiveCutCamera(double&) { return 0; }
bool ConfirmClose() { return true; }
double PreviewAspect() { return 16.0 / 9.0; }
bool BeginCinematicRender() { return false; }
void SetCinematicRenderTime(double) {}
void EndCinematicRender() {}
} // namespace SequencerEditor
