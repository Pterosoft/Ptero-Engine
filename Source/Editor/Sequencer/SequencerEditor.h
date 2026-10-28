#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Entity;
struct EngineEnvironment;

// What Render Sequence produces. Frames are counted at `Rate`, inclusive at both ends.
struct CinematicRenderSettings
{
    enum class Format
    {
        Png,
        Jpeg,
        // H.264 through the ffmpeg that ships in Binaries/ffmpeg. Video only for now:
        // sequences have no audio tracks yet.
        Mp4
    };

    std::string OutputDirectory;
    // Files are <BaseName>_<frame>.png / .jpg, or <BaseName>.mp4.
    std::string BaseName;
    int Width = 1920;
    int Height = 1080;
    int RateNumerator = 30;
    int RateDenominator = 1;
    std::int64_t StartFrame = 0;
    std::int64_t EndFrame = 150;
    Format OutputFormat = Format::Png;
    // 1-100: JPEG quality, or MP4 visual quality (mapped to the encoder's CRF).
    int Quality = 90;
    // Frames rendered at the first frame's time and thrown away before capturing, so
    // temporal effects (TAA, exposure adaptation, GI accumulation) have settled.
    int WarmupFrames = 24;
};

struct CinematicRenderStatus
{
    bool Active = false;
    bool Finished = false;
    bool Failed = false;
    bool Encoding = false;
    int FramesDone = 0;
    int FramesTotal = 0;
    std::string Message;
    // The folder of images, or the .mp4.
    std::string OutputPath;
};

// What Ptero Sequencer needs from the level editor. The Editor implements it; the
// Sequencer window never includes Editor.h, so the two stay decoupled the way the Node
// Graph window is.
class SequencerEditorHost
{
public:
    virtual ~SequencerEditorHost() = default;

    // The level being edited. Previewing a sequence writes animated values straight into
    // these entities, exactly as a play session does.
    virtual std::vector<Entity>* Entities() = 0;
    virtual EngineEnvironment Environment() = 0;

    // Gives every entity a persistent id (EnsureEntityIds) so it can be bound.
    virtual void EnsureEntityIds() = 0;
    virtual std::vector<std::uint64_t> SelectedEntityIds() = 0;
    virtual void SelectEntity(std::uint64_t entityId) = 0;

    // Adds a camera entity where the viewport is looking from; returns its id (0 if the
    // level cannot take one). The editor records it for undo like any other new entity.
    virtual std::uint64_t CreateCameraAtView(const std::string& name) = 0;

    // Hands the viewport's fly controls to a camera entity: moving the view moves the
    // camera (and, with Auto Key on, keys it).
    virtual void PilotCamera(std::uint64_t entityId) = 0;

    // While a play session runs the level belongs to the game; the Sequencer neither
    // previews nor keys during one.
    virtual bool IsPlaySessionActive() = 0;

    // Data/ as an absolute path, for file dialogs.
    virtual std::string DataDirectory() = 0;

    // Render Sequence. The editor owns the job because it owns the frame loop: each frame
    // it poses the level through SequencerEditor::SetCinematicRenderTime and captures the
    // finished image. False with `error` filled when the job cannot start.
    virtual bool StartCinematicRender(const CinematicRenderSettings& settings, std::string& error) = 0;
    virtual void CancelCinematicRender() = 0;
    virtual CinematicRenderStatus GetCinematicRenderStatus() = 0;
};

// The editor's handle on the Sequencer window. Every function is safe to call before the
// window exists - and before Qt is initialised - and from the packaged game, where a
// headless stand-in compiles in its place.
namespace SequencerEditor
{
    void SetHost(SequencerEditorHost* host);

    void Show();
    bool IsVisible();
    // Destroys the window, restoring the level first. Called before Qt shuts down.
    void Shutdown();

    // Opens a .sequence (absolute path), showing the window. Asks about unsaved edits.
    bool OpenSequence(const std::string& path);

    // Once per editor frame: advances preview playback and applies pending evaluation.
    void Tick(float deltaSeconds);

    // Called by Editor::MarkSceneChanged after an edit in the level: Auto Key turns
    // changes to animated objects into keys at the playhead.
    void OnSceneChanged();

    // The level was replaced (load, new). The recorded pre-animation state belonged to the
    // old one and is dropped rather than restored onto unrelated entities.
    void OnLevelReplaced();

    // The level's own undo or redo rewrote the entities. Animated properties are put back
    // on their curves at the next tick - the level's history cannot undo sequence keys,
    // and leaving the two disagreeing would let Auto Key record the rewound values on the
    // next unrelated edit.
    void OnLevelRestored();

    // Puts the level back to how it was authored, and later re-applies the preview: around
    // a level save (so animated values are never saved into the level) and around a play
    // session. Nested calls are counted.
    void SuspendPreview();
    void ResumePreview();

    // "Lock Viewport to Cinematic Camera": while on, and a sequence with camera cuts is
    // open, the viewport looks through the cut at the playhead. Toggled from the
    // Sequencer's toolbar and the viewport menu alike.
    void SetCameraLock(bool locked);
    bool IsCameraLocked();

    // The camera entity the viewport should look through under the lock: the cut at the
    // playhead. 0 when unlocked or there is none. `shakeTime` is the sequence time.
    std::uint64_t ActiveCutCamera(double& shakeTime);

    // Asks about unsaved sequence edits before the editor closes. False means cancel.
    bool ConfirmClose();

    // Width over height of the frame the sequence renders at (Render Sequence's
    // resolution, 16:9 until one is chosen). The viewport frames the camera view to it.
    double PreviewAspect();

    // Used by the editor's render job. Begin pauses the preview and locks the view to the
    // camera cuts (false when no sequence is open); SetCinematicRenderTime poses the level
    // at a time; End returns the Sequencer to normal.
    bool BeginCinematicRender();
    void SetCinematicRenderTime(double seconds);
    void EndCinematicRender();
}
