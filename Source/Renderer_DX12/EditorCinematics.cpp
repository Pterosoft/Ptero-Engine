// ---------------------------------------------------------------------------
// The level editor's side of Ptero Sequencer: what the Sequencer window is
// allowed to see of the editor (entities, selection, the view), cinematic
// cameras as level objects (placement, Properties, piloting), and locking the
// viewport to the sequence's cut camera.
//
// The Sequencer itself lives in Source/Editor/Sequencer and never includes
// Editor.h; EditorSequencerHost below is the whole of its view of the editor.
// ---------------------------------------------------------------------------

#include "pch.h"
#include "Editor.h"

#include "CinematicWorldHost.h"
#include "DX12SceneRenderer.h"
#include "EntityIds.h"
#include "EntityProperties.h"
#include "FrameCapture.h"
#include "VideoImport.h"

#include "System/Cinematics/CinematicSequence.h"
#include "System/DataFiles.h"
#include "System/PteroLog.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>

namespace
{
    constexpr float kRadiansPerDegree = 0.017453292519943295f;
    constexpr float kDegreesPerRadian = 57.29577951308232f;
}

class EditorSequencerHost final : public SequencerEditorHost
{
public:
    explicit EditorSequencerHost(Editor& editor) : mEditor(editor) {}

    std::vector<Entity>* Entities() override { return &mEditor.mEntities; }

    EngineEnvironment Environment() override
    {
        return EngineEnvironment{ mEditor.mTimeOfDaySettings, mEditor.mVolumetricFogSettings };
    }

    void EnsureEntityIds() override
    {
        // A binding names its entity by id, so an entity bound before the level was ever
        // saved must have one - and the level must be saved for the id to stick.
        if (::EnsureEntityIds(mEditor.mEntities))
            mEditor.mSceneDirty = true;
    }

    std::vector<std::uint64_t> SelectedEntityIds() override
    {
        EnsureEntityIds();
        std::vector<std::uint64_t> ids;
        for (const int index : mEditor.GetSelectedEntityIndices())
            ids.push_back(mEditor.mEntities[index].Id);
        return ids;
    }

    void SelectEntity(std::uint64_t entityId) override
    {
        for (std::size_t i = 0; i < mEditor.mEntities.size(); ++i)
        {
            if (mEditor.mEntities[i].Id == entityId)
            {
                mEditor.SelectEntity(static_cast<int>(i), false);
                return;
            }
        }
    }

    std::uint64_t CreateCameraAtView(const std::string& name) override { return mEditor.CreateCameraAtView(name); }
    void PilotCamera(std::uint64_t entityId) override { mEditor.PilotCamera(entityId); }
    bool IsPlaySessionActive() override { return mEditor.IsPlaySessionActive(); }
    std::string DataDirectory() override { return DataFiles::FindDataDirectory().string(); }

    bool StartCinematicRender(const CinematicRenderSettings& settings, std::string& error) override
    {
        return mEditor.StartCinematicRender(settings, error);
    }
    void CancelCinematicRender() override { mEditor.CancelCinematicRender(); }
    CinematicRenderStatus GetCinematicRenderStatus() override { return mEditor.GetCinematicRenderStatus(); }

private:
    Editor& mEditor;
};

// ---------------------------------------------------------------------------
// Render Sequence.
//
// Offline rendering through the real-time renderer, one frame per editor frame. The
// image read back at a frame is the one the GPU finished last, which was recorded the
// frame before and posed the frame before that - so the job keeps the last two poses in
// flight and captures each one two frames after setting it. Warm-up frames at the first
// pose let temporal effects settle; two trailing frames flush the pipeline. While the job
// runs the scene clock advances a fixed 1/fps per frame (SetFixedFrameDelta), so the
// result does not depend on how long the GPU took over each frame.
// ---------------------------------------------------------------------------

struct CinematicRenderJob
{
    struct Slot
    {
        double Time = 0.0;
        // Index of the output frame this pose becomes, or -1 for warm-up and flush.
        int Capture = -1;
    };

    enum class Stage
    {
        WaitForResolution,
        Rendering,
        Encoding
    };

    CinematicRenderSettings Settings;
    Stage CurrentStage = Stage::WaitForResolution;
    std::vector<Slot> Slots;
    std::size_t NextSlot = 0;
    std::deque<int> InFlight;
    int FramesTotal = 0;
    int FramesDone = 0;
    int WaitedFrames = 0;
    std::filesystem::path FramesDirectory;
    std::filesystem::path OutputPath;
    bool CancelRequested = false;
    std::string Message;

    bool HadFixedResolution = false;
    int PreviousWidth = 0;
    int PreviousHeight = 0;

    // MP4: ffmpeg runs on a thread once every frame is on disk.
    std::thread Encoder;
    std::atomic<bool> EncodeDone{ false };
    std::atomic<bool> EncodeSucceeded{ false };
    std::atomic<HANDLE> EncodeProcess{ nullptr };
    std::string EncodeError;
    // Frames handed to ffmpeg: all of them, or as many as were done when a render was
    // cancelled - a cancelled MP4 render still produces the video up to that point.
    int FramesToEncode = 0;

    // Compressing a 1080p PNG takes longer than rendering the frame, so it happens on
    // worker threads; the render thread only reads the image back and queues it. The
    // queue is bounded so a slow disk holds the render back instead of filling memory.
    struct PendingImage
    {
        std::filesystem::path Path;
        std::vector<std::uint8_t> Pixels;
        std::uint32_t Width = 0;
        std::uint32_t Height = 0;
        FrameCapture::ImageFormat Format = FrameCapture::ImageFormat::Png;
        int Quality = 95;
    };
    static constexpr std::size_t kMaxQueuedImages = 8;
    std::mutex WriteMutex;
    std::condition_variable WriteReady;
    std::condition_variable WriteSpace;
    std::deque<PendingImage> WriteQueue;
    std::vector<std::thread> Writers;
    bool StopWriters = false;
    int WritesInProgress = 0;
    int FramesWritten = 0;
    std::string WriteError;

    void StartWriters()
    {
        const unsigned cores = (std::max)(2u, std::thread::hardware_concurrency());
        const unsigned count = std::clamp(cores / 2, 2u, 6u);
        for (unsigned i = 0; i < count; ++i)
            Writers.emplace_back([this] { WriterLoop(); });
    }

    void WriterLoop()
    {
        for (;;)
        {
            PendingImage image;
            {
                std::unique_lock<std::mutex> lock(WriteMutex);
                WriteReady.wait(lock, [this] { return StopWriters || !WriteQueue.empty(); });
                if (WriteQueue.empty())
                    return; // stopping, and everything queued is written
                image = std::move(WriteQueue.front());
                WriteQueue.pop_front();
                ++WritesInProgress;
            }
            WriteSpace.notify_one();
            const bool written = FrameCapture::WriteImage(PathText(image.Path), image.Pixels, image.Width, image.Height,
                                                          image.Format, image.Quality);
            std::lock_guard<std::mutex> lock(WriteMutex);
            --WritesInProgress;
            if (written)
                ++FramesWritten;
            else if (WriteError.empty())
                WriteError = "Cannot write '" + PathText(image.Path) + "'.";
        }
    }

    void Enqueue(PendingImage image)
    {
        std::unique_lock<std::mutex> lock(WriteMutex);
        WriteSpace.wait(lock, [this] { return WriteQueue.size() < kMaxQueuedImages; });
        WriteQueue.push_back(std::move(image));
        lock.unlock();
        WriteReady.notify_one();
    }

    // Writes everything still queued, then ends the workers.
    void FinishWriting()
    {
        {
            std::lock_guard<std::mutex> lock(WriteMutex);
            StopWriters = true;
        }
        WriteReady.notify_all();
        for (std::thread& writer : Writers)
        {
            if (writer.joinable())
                writer.join();
        }
        Writers.clear();
    }

    int Written()
    {
        std::lock_guard<std::mutex> lock(WriteMutex);
        return FramesWritten;
    }

    std::string Error()
    {
        std::lock_guard<std::mutex> lock(WriteMutex);
        return WriteError;
    }

    static std::string PathText(const std::filesystem::path& path)
    {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    ~CinematicRenderJob()
    {
        FinishWriting();
        if (HANDLE process = EncodeProcess.load())
            TerminateProcess(process, 1);
        if (Encoder.joinable())
            Encoder.join();
    }
};

namespace
{
    std::filesystem::path Utf8Path(const std::string& text)
    {
        return std::filesystem::path(std::u8string(text.begin(), text.end()));
    }

    std::string PathText(const std::filesystem::path& path)
    {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    // ffmpeg turns the frames into H.264. Odd sizes are padded by a pixel: 4:2:0 needs
    // even ones. Its errors go to a log beside the frames, read back on failure.
    void RunEncoder(CinematicRenderJob* job, std::wstring ffmpeg)
    {
        const CinematicRenderSettings& s = job->Settings;
        const int crf = std::clamp(static_cast<int>(std::lround(51.0 - s.Quality * 0.33)), 12, 40);
        const std::filesystem::path pattern = job->FramesDirectory / L"frame_%06d.png";
        const std::filesystem::path log = job->FramesDirectory / L"ffmpeg.log";
        std::wstring command = L"\"" + ffmpeg + L"\" -y -hide_banner -loglevel error -framerate " +
                               std::to_wstring(s.RateNumerator) + L"/" + std::to_wstring(s.RateDenominator) +
                               L" -i \"" + pattern.wstring() + L"\" -frames:v " + std::to_wstring(job->FramesToEncode) +
                               L" -vf \"pad=ceil(iw/2)*2:ceil(ih/2)*2\" -c:v libx264 -preset slow -crf " +
                               std::to_wstring(crf) + L" -pix_fmt yuv420p -movflags +faststart \"" +
                               job->OutputPath.wstring() + L"\"";

        SECURITY_ATTRIBUTES inherit{ sizeof(inherit), nullptr, TRUE };
        HANDLE logFile = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdError = logFile;
        startup.hStdOutput = logFile;
        startup.hStdInput = nullptr;
        PROCESS_INFORMATION process{};
        const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                            job->FramesDirectory.c_str(), &startup, &process);
        if (logFile != INVALID_HANDLE_VALUE)
            CloseHandle(logFile);
        if (!started)
        {
            job->EncodeError = "ffmpeg could not be started.";
            job->EncodeDone = true;
            return;
        }

        job->EncodeProcess = process.hProcess;
        WaitForSingleObject(process.hProcess, INFINITE);
        DWORD exitCode = 1;
        GetExitCodeProcess(process.hProcess, &exitCode);
        job->EncodeProcess = nullptr;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);

        if (exitCode != 0)
        {
            std::string text;
            if (DataFiles::ReadText(log, text) && !text.empty())
                job->EncodeError = "ffmpeg failed: " + text.substr(0, 400);
            else
                job->EncodeError = "ffmpeg failed (exit code " + std::to_string(exitCode) + ").";
        }
        job->EncodeSucceeded = exitCode == 0;
        job->EncodeDone = true;
    }
}

bool Editor::StartCinematicRender(const CinematicRenderSettings& settings, std::string& error)
{
    if (mRenderJob)
    {
        error = "A render is already running.";
        return false;
    }
    if (IsPlaySessionActive())
    {
        error = "Stop the play session first.";
        return false;
    }
    if (mSceneRenderer == nullptr)
    {
        error = "The renderer is not ready.";
        return false;
    }
    if (settings.Width < 16 || settings.Height < 16 || settings.Width > 8192 || settings.Height > 8192)
    {
        error = "The resolution must be between 16 and 8192 pixels on each side.";
        return false;
    }
    if (settings.RateNumerator <= 0 || settings.RateDenominator <= 0 || settings.EndFrame < settings.StartFrame)
    {
        error = "The frame range is empty.";
        return false;
    }
    if (settings.OutputDirectory.empty())
    {
        error = "Choose an output folder.";
        return false;
    }

    auto job = std::make_unique<CinematicRenderJob>();
    job->Settings = settings;
    const std::filesystem::path directory = Utf8Path(settings.OutputDirectory);
    const bool video = settings.OutputFormat == CinematicRenderSettings::Format::Mp4;
    std::wstring ffmpeg;
    if (video)
    {
        ffmpeg = VideoImport::FindFfmpeg();
        if (ffmpeg.empty())
        {
            error = "MP4 output needs ffmpeg.exe, which was not found (expected in Binaries\\ffmpeg\\bin).";
            return false;
        }
        job->FramesDirectory = directory / (Utf8Path(settings.BaseName).wstring() + L"_frames");
        job->OutputPath = directory / (Utf8Path(settings.BaseName).wstring() + L".mp4");
    }
    else
    {
        job->FramesDirectory = directory;
        job->OutputPath = directory;
    }

    std::error_code fileError;
    std::filesystem::create_directories(job->FramesDirectory, fileError);
    if (fileError || !std::filesystem::is_directory(job->FramesDirectory))
    {
        error = "Cannot create the output folder '" + PathText(job->FramesDirectory) + "'.";
        return false;
    }

    if (!SequencerEditor::BeginCinematicRender())
    {
        error = "Open the sequence in the Sequencer (Windows > Cinematic Editor) to render it.";
        return false;
    }

    // The pose of every frame, plus warm-up at the first and two flush frames at the last.
    const FrameRate rate{ settings.RateNumerator, settings.RateDenominator };
    job->FramesTotal = static_cast<int>(settings.EndFrame - settings.StartFrame + 1);
    const double first = rate.FrameToSeconds(static_cast<double>(settings.StartFrame));
    const double last = rate.FrameToSeconds(static_cast<double>(settings.EndFrame));
    for (int i = 0; i < settings.WarmupFrames; ++i)
        job->Slots.push_back({ first, -1 });
    for (int i = 0; i < job->FramesTotal; ++i)
        job->Slots.push_back({ rate.FrameToSeconds(static_cast<double>(settings.StartFrame + i)), i });
    job->Slots.push_back({ last, -1 });
    job->Slots.push_back({ last, -1 });

    // The viewport's scene target becomes the render resolution for the duration.
    StopPilotingCamera();
    job->HadFixedResolution = mViewportResolutionFixed;
    job->PreviousWidth = mViewportResolutionWidth;
    job->PreviousHeight = mViewportResolutionHeight;
    RequestViewportResolution(settings.Width, settings.Height);
    mViewportResolutionFixed = true;
    mSceneRenderer->SetFixedFrameDelta(static_cast<float>(rate.FrameDuration()));
    SequencerEditor::SetCinematicRenderTime(first);

    job->Message = "Preparing " + std::to_string(settings.Width) + " x " + std::to_string(settings.Height) + "...";
    mLastRenderStatus = CinematicRenderStatus{};
    job->StartWriters();
    mRenderJob = std::move(job);
    PTERO_LOG_INFO("Sequencer", "Render Sequence: %d frames at %dx%d to '%s'.", mRenderJob->FramesTotal, settings.Width,
                   settings.Height, settings.OutputDirectory.c_str());
    return true;
}

void Editor::CancelCinematicRender()
{
    if (mRenderJob)
        mRenderJob->CancelRequested = true;
}

CinematicRenderStatus Editor::GetCinematicRenderStatus() const
{
    if (!mRenderJob)
        return mLastRenderStatus;
    CinematicRenderStatus status;
    status.Active = true;
    status.Encoding = mRenderJob->CurrentStage == CinematicRenderJob::Stage::Encoding;
    status.FramesDone = mRenderJob->FramesDone;
    status.FramesTotal = mRenderJob->FramesTotal;
    status.Message = mRenderJob->Message;
    status.OutputPath = PathText(mRenderJob->OutputPath);
    return status;
}

void Editor::FinishCinematicRender(bool succeeded, const std::string& message)
{
    if (!mRenderJob)
        return;

    // The viewport goes back to the size it had; following the panel again if it was.
    mViewportResolutionFixed = mRenderJob->HadFixedResolution;
    if (mRenderJob->HadFixedResolution)
        RequestViewportResolution(mRenderJob->PreviousWidth, mRenderJob->PreviousHeight);
    if (mSceneRenderer != nullptr)
        mSceneRenderer->SetFixedFrameDelta(0.0f);
    SequencerEditor::EndCinematicRender();

    mLastRenderStatus = CinematicRenderStatus{};
    mLastRenderStatus.Finished = true;
    mLastRenderStatus.Failed = !succeeded;
    mLastRenderStatus.FramesDone = mRenderJob->FramesDone;
    mLastRenderStatus.FramesTotal = mRenderJob->FramesTotal;
    mLastRenderStatus.Message = message;
    mLastRenderStatus.OutputPath = succeeded ? PathText(mRenderJob->OutputPath) : std::string();
    if (succeeded)
        PTERO_LOG_INFO("Sequencer", "%s", message.c_str());
    else
        PTERO_LOG_WARNING("Sequencer", "%s", message.c_str());
    mRenderJob.reset();
}

void Editor::UpdateCinematicRender(ID3D12Resource* finalImage)
{
    if (!mRenderJob)
        return;
    CinematicRenderJob& job = *mRenderJob;
    const CinematicRenderSettings& settings = job.Settings;

    const auto startEncoding = [&](int frames) {
        job.FramesToEncode = frames;
        job.CurrentStage = CinematicRenderJob::Stage::Encoding;
        job.Message = "Encoding MP4 (" + std::to_string(frames) + " frames)...";
        job.Encoder = std::thread(RunEncoder, &job, VideoImport::FindFfmpeg());
    };

    if (job.CurrentStage == CinematicRenderJob::Stage::Encoding)
    {
        // Cancelling now stops ffmpeg; the frames stay on disk.
        const bool stopEncoding = job.CancelRequested && job.FramesToEncode == job.FramesTotal;
        if (stopEncoding)
        {
            if (HANDLE process = job.EncodeProcess.load())
                TerminateProcess(process, 1);
        }
        if (!job.EncodeDone)
            return;
        job.Encoder.join();
        const bool partial = job.FramesToEncode < job.FramesTotal;
        if (stopEncoding)
        {
            FinishCinematicRender(false, "Encoding cancelled. The frames were kept in " + PathText(job.FramesDirectory) + ".");
        }
        else if (job.EncodeSucceeded)
        {
            // The frames were only the encoder's input.
            std::error_code ignored;
            std::filesystem::remove_all(job.FramesDirectory, ignored);
            FinishCinematicRender(true, (partial ? "Cancelled; the " + std::to_string(job.FramesToEncode) + " frames "
                                                   "rendered so far were encoded to "
                                                 : "Rendered " + std::to_string(job.FramesTotal) + " frames to ") +
                                            PathText(job.OutputPath) + ".");
        }
        else
        {
            FinishCinematicRender(false, job.EncodeError + " The frames were kept in " + PathText(job.FramesDirectory) + ".");
        }
        return;
    }

    if (const std::string error = job.Error(); !error.empty())
    {
        job.FinishWriting();
        FinishCinematicRender(false, error);
        return;
    }

    if (job.CancelRequested || IsPlaySessionActive())
    {
        // Whatever was captured is finished writing; an MP4 render still becomes a video
        // of the frames done so far rather than a folder of images.
        job.FinishWriting();
        const int written = job.Written();
        if (settings.OutputFormat == CinematicRenderSettings::Format::Mp4 && written > 0 && job.Error().empty())
        {
            startEncoding(written);
            return;
        }
        FinishCinematicRender(false, std::string(job.CancelRequested ? "Render cancelled" : "Render stopped by a play session") +
                                         " after " + std::to_string(written) + " of " + std::to_string(job.FramesTotal) +
                                         " frames.");
        return;
    }

    if (job.CurrentStage == CinematicRenderJob::Stage::WaitForResolution)
    {
        // A few frames at the new size before anything counts, so every target has been
        // rebuilt; give up if the renderer never gets there.
        ++job.WaitedFrames;
        if (mSceneRenderer->GetSceneWidth() == static_cast<UINT>(settings.Width) &&
            mSceneRenderer->GetSceneHeight() == static_cast<UINT>(settings.Height))
        {
            if (job.WaitedFrames >= 3)
            {
                job.CurrentStage = CinematicRenderJob::Stage::Rendering;
                job.Message = "Rendering...";
            }
        }
        else if (job.WaitedFrames > 300)
        {
            FinishCinematicRender(false, "The renderer did not switch to " + std::to_string(settings.Width) + " x " +
                                             std::to_string(settings.Height) + ".");
        }
        return;
    }

    // Capture the pose set two frames ago. The read-back happens here, on the render
    // thread that owns the GPU queue; compressing and writing it goes to the workers.
    if (job.InFlight.size() == 2)
    {
        const int capture = job.InFlight.front();
        job.InFlight.pop_front();
        if (capture >= 0)
        {
            CinematicRenderJob::PendingImage image;
            if (!FrameCapture::ReadbackBgra(finalImage, image.Pixels, image.Width, image.Height))
            {
                job.FinishWriting();
                FinishCinematicRender(false, "Reading the rendered image back failed.");
                return;
            }
            if (image.Width != static_cast<std::uint32_t>(settings.Width) ||
                image.Height != static_cast<std::uint32_t>(settings.Height))
            {
                job.FinishWriting();
                FinishCinematicRender(false, "The render resolution changed during the render.");
                return;
            }

            char name[64];
            if (settings.OutputFormat == CinematicRenderSettings::Format::Mp4)
            {
                std::snprintf(name, sizeof(name), "frame_%06d.png", capture);
                image.Path = job.FramesDirectory / name;
            }
            else
            {
                const bool jpeg = settings.OutputFormat == CinematicRenderSettings::Format::Jpeg;
                image.Format = jpeg ? FrameCapture::ImageFormat::Jpeg : FrameCapture::ImageFormat::Png;
                std::snprintf(name, sizeof(name), "_%06lld.%s", static_cast<long long>(settings.StartFrame + capture),
                              jpeg ? "jpg" : "png");
                image.Path = job.FramesDirectory / (Utf8Path(settings.BaseName).wstring() + Utf8Path(name).wstring());
            }
            image.Quality = settings.Quality;
            job.Enqueue(std::move(image));
            ++job.FramesDone;
            job.Message = "Frame " + std::to_string(job.FramesDone) + " of " + std::to_string(job.FramesTotal) +
                          (settings.OutputFormat == CinematicRenderSettings::Format::Mp4 ? "  (encoded to MP4 at the end)" : "");
        }
    }

    // Pose the next frame.
    if (job.NextSlot < job.Slots.size())
    {
        const CinematicRenderJob::Slot& slot = job.Slots[job.NextSlot++];
        SequencerEditor::SetCinematicRenderTime(slot.Time);
        job.InFlight.push_back(slot.Capture);
    }

    if (job.FramesDone < job.FramesTotal)
        return;

    // Every frame is captured: wait for the last ones to reach the disk.
    job.FinishWriting();
    if (const std::string error = job.Error(); !error.empty())
    {
        FinishCinematicRender(false, error);
        return;
    }
    if (settings.OutputFormat == CinematicRenderSettings::Format::Mp4)
    {
        startEncoding(job.FramesTotal);
        return;
    }
    FinishCinematicRender(true, "Rendered " + std::to_string(job.FramesTotal) + " frames to " + PathText(job.OutputPath) + ".");
}

Editor::Editor() = default;
Editor::~Editor() = default;

void Editor::InitializeCinematics()
{
    if (!mSequencerHost)
    {
        RegisterEngineProperties();
        mSequencerHost = std::make_unique<EditorSequencerHost>(*this);
        SequencerEditor::SetHost(mSequencerHost.get());
    }

    // The renderer may be (re)created after the editor; the hook is installed once it
    // exists.
    if (!mCinematicCameraHookInstalled && mSceneRenderer != nullptr)
    {
        mSceneRenderer->SetEditorCameraHook([this](EditorCamera& camera) { UpdateCinematicViewportCamera(camera); });
        mCinematicCameraHookInstalled = true;
    }
}

void Editor::ShutdownCinematics()
{
    // A render still running is abandoned; its job stops ffmpeg if it was encoding.
    if (mRenderJob)
        FinishCinematicRender(false, "Render abandoned: the editor closed.");
    SequencerEditor::Shutdown();
    SequencerEditor::SetHost(nullptr);
    if (mSceneRenderer != nullptr)
        mSceneRenderer->SetEditorCameraHook({});
    mCinematicCameraHookInstalled = false;
    mSequencerHost.reset();
}

Entity* Editor::FindEntityById(std::uint64_t entityId)
{
    if (entityId == 0)
        return nullptr;
    for (Entity& entity : mEntities)
    {
        if (entity.Id == entityId)
            return &entity;
    }
    return nullptr;
}

std::uint64_t Editor::CreateCameraAtView(const std::string& name)
{
    if (IsPlaySessionActive())
        return 0;

    Entity entity;
    entity.Name = name;
    CameraComponent& lens = entity.Camera.emplace();
    if (mSceneRenderer != nullptr)
    {
        const EditorCamera& view = mSceneRenderer->GetCamera();
        const DirectX::XMFLOAT3 rotation = view.GetRotation();
        entity.Transform.Position = view.GetPosition();
        entity.Transform.Rotation = CinematicWorldHost::RotationForView(rotation.x, rotation.y);
        // The new camera frames exactly what the viewport frames.
        lens.SetVerticalFovDegrees(view.GetFovYRadians() * kDegreesPerRadian);
    }

    mEntities.push_back(std::move(entity));
    ::EnsureEntityIds(mEntities);
    mSelectedEntityIndex = static_cast<int>(mEntities.size()) - 1;
    mSelectedEntityIndices = { mSelectedEntityIndex };
    MarkSceneChanged();
    return mEntities.back().Id;
}

void Editor::PilotCamera(std::uint64_t entityId)
{
    if (entityId == mPilotedCameraId)
        return;

    const bool wasPiloting = mPilotedCameraId != 0;
    const Entity* entity = FindEntityById(entityId);
    if (entityId != 0 && (entity == nullptr || !entity->Camera))
        return;

    mPilotedCameraId = entityId;
    if (entityId != 0)
    {
        // The view's own field of view, to give back when piloting ends. Switching from
        // one camera to another keeps the one saved before the first.
        if (!wasPiloting)
        {
            mFovBeforePilot = mCinematicLockActive ? mViewBeforeLockFov
                            : mSceneRenderer != nullptr ? mSceneRenderer->GetCamera().GetFovYRadians()
                                                        : DirectX::XM_PIDIV4;
        }
        mPilotSnapPending = true;
        mPilotRestorePending = false;
    }
    else
    {
        mPilotRestorePending = wasPiloting;
    }
}

namespace
{
    // The render frame inside a viewport of the given size: the largest rectangle of the
    // render's aspect ratio that fits, centred. The camera's vertical field of view is
    // the render's; FitFovToRenderFrame widens the view's when the viewport is narrower
    // than the render, so the frame's height still spans exactly that field of view.
    void RenderFrameInViewport(double renderAspect, float width, float height, float& frameWidth, float& frameHeight)
    {
        frameWidth = width;
        frameHeight = height;
        if (width <= 1.0f || height <= 1.0f || renderAspect <= 0.0)
            return;
        if (width / height > renderAspect)
            frameWidth = static_cast<float>(height * renderAspect);
        else
            frameHeight = static_cast<float>(width / renderAspect);
    }
}

float Editor::FitFovToRenderFrame(float verticalFovRadians) const
{
    if (mSceneRenderer == nullptr)
        return verticalFovRadians;
    const double viewWidth = static_cast<double>(mSceneRenderer->GetSceneWidth());
    const double viewHeight = static_cast<double>(mSceneRenderer->GetSceneHeight());
    if (viewWidth <= 1.0 || viewHeight <= 1.0)
        return verticalFovRadians;
    const double viewAspect = viewWidth / viewHeight;
    const double renderAspect = SequencerEditor::PreviewAspect();
    if (viewAspect >= renderAspect)
        return verticalFovRadians;
    // The frame is letterboxed: its height is viewHeight * viewAspect / renderAspect, and
    // the camera's field of view must span exactly that.
    const double halfTangent = std::tan(verticalFovRadians * 0.5) * renderAspect / viewAspect;
    return static_cast<float>(2.0 * std::atan(halfTangent));
}

void Editor::DrawCinematicFrameGuide(const UiVec2& viewportOrigin, const UiVec2& viewportSize) const
{
    if (!mCinematicLockActive && mPilotedCameraId == 0)
        return;

    float frameWidth = 0.0f;
    float frameHeight = 0.0f;
    RenderFrameInViewport(SequencerEditor::PreviewAspect(), viewportSize.x, viewportSize.y, frameWidth, frameHeight);
    const float left = viewportOrigin.x + (viewportSize.x - frameWidth) * 0.5f;
    const float top = viewportOrigin.y + (viewportSize.y - frameHeight) * 0.5f;
    const float right = left + frameWidth;
    const float bottom = top + frameHeight;
    const float viewRight = viewportOrigin.x + viewportSize.x;
    const float viewBottom = viewportOrigin.y + viewportSize.y;

    // Outside the frame is what the render will not contain: mattes over it, and a thin
    // line on the frame edge.
    UiDrawList* drawList = QtUi::GetWindowDrawList();
    const UiU32 matte = UI_COL32(0, 0, 0, 200);
    if (top > viewportOrigin.y + 0.5f)
    {
        drawList->AddRectFilled(UiVec2(viewportOrigin.x, viewportOrigin.y), UiVec2(viewRight, top), matte);
        drawList->AddRectFilled(UiVec2(viewportOrigin.x, bottom), UiVec2(viewRight, viewBottom), matte);
    }
    if (left > viewportOrigin.x + 0.5f)
    {
        drawList->AddRectFilled(UiVec2(viewportOrigin.x, viewportOrigin.y), UiVec2(left, viewBottom), matte);
        drawList->AddRectFilled(UiVec2(right, viewportOrigin.y), UiVec2(viewRight, viewBottom), matte);
    }
    drawList->AddRect(UiVec2(left, top), UiVec2(right, bottom),
                      mPilotedCameraId != 0 ? UI_COL32(255, 200, 90, 220) : UI_COL32(220, 220, 220, 160), 0.0f, 0, 1.0f);
}

void Editor::UpdateCinematicViewportCamera(EditorCamera& camera)
{
    if (IsPlaySessionActive())
        return;

    if (mPilotRestorePending)
    {
        camera.SetFovYRadians(mFovBeforePilot);
        mPilotRestorePending = false;
    }

    // ---- Piloting: the view drives the camera entity ----------------------------------
    if (mPilotedCameraId != 0)
    {
        Entity* entity = FindEntityById(mPilotedCameraId);
        if (entity == nullptr || !entity->Camera)
        {
            // Deleted or stripped of its camera while being flown.
            mPilotedCameraId = 0;
            camera.SetFovYRadians(mFovBeforePilot);
            return;
        }

        TransformComponent& transform = entity->Transform;
        const auto differs = [](const DirectX::XMFLOAT3& a, const DirectX::XMFLOAT3& b) {
            return std::fabs(a.x - b.x) > 1e-5f || std::fabs(a.y - b.y) > 1e-5f || std::fabs(a.z - b.z) > 1e-5f;
        };
        const bool entityMovedElsewhere = differs(transform.Position, mPilotLastTransform.Position) ||
                                          differs(transform.Rotation, mPilotLastTransform.Rotation);

        if (mPilotSnapPending || entityMovedElsewhere)
        {
            // Taking control, or something other than the pilot moved the camera - the
            // sequence being scrubbed or played, an undo, the Properties panel. The view
            // follows the camera; nothing is written back, so nothing is keyed.
            const CinematicCameraView view = CinematicWorldHost::ComputeCameraView(*entity, 0.0);
            camera.SetPosition(view.Position);
            camera.SetRotation(view.Pitch, view.Yaw);
            mPilotSnapPending = false;
        }
        else
        {
            const DirectX::XMFLOAT3 position = camera.GetPosition();
            const DirectX::XMFLOAT3 rotation = camera.GetRotation();
            if (differs(position, mPilotLastViewPosition) || differs(rotation, mPilotLastViewRotation))
            {
                // The user flew the view: the camera goes with it. The heading is applied
                // as a change to the camera's own Rotation.z rather than recomputed from
                // the view's yaw, which is wrapped to +/-180 degrees and would make an
                // animated camera at 270 degrees jump - and, under Auto Key, be keyed so.
                const float yawChange = rotation.y - mPilotLastViewRotation.y;
                transform.Position = position;
                transform.Rotation.x = rotation.x;
                transform.Rotation.z -= yawChange;
                // An ordinary level edit: undoable, and keyed by Auto Key when the camera
                // is animated by the open sequence.
                MarkSceneChanged();
            }
        }
        mPilotLastTransform = transform;
        mPilotLastViewPosition = camera.GetPosition();
        mPilotLastViewRotation = camera.GetRotation();

        camera.SetFovYRadians(FitFovToRenderFrame(entity->Camera->VerticalFovDegrees() * kRadiansPerDegree));
        // Piloting takes over from the lock without handing the old view back.
        mCinematicLockActive = false;
        return;
    }

    // ---- Lock: the cut camera drives the view -----------------------------------------
    double shakeTime = 0.0;
    const Entity* cutCamera = FindEntityById(SequencerEditor::ActiveCutCamera(shakeTime));
    if (cutCamera != nullptr)
    {
        if (!mCinematicLockActive)
        {
            mViewBeforeLockPosition = camera.GetPosition();
            mViewBeforeLockRotation = camera.GetRotation();
            mViewBeforeLockFov = camera.GetFovYRadians();
            mCinematicLockActive = true;
        }
        const CinematicCameraView view = CinematicWorldHost::ComputeCameraView(*cutCamera, shakeTime);
        camera.SetPosition(view.Position);
        camera.SetRotation(view.Pitch, view.Yaw);
        camera.SetFovYRadians(FitFovToRenderFrame(view.VerticalFovRadians));
    }
    else if (mCinematicLockActive)
    {
        // Back to the free view the lock interrupted.
        camera.SetPosition(mViewBeforeLockPosition);
        camera.SetRotation(mViewBeforeLockRotation.x, mViewBeforeLockRotation.y);
        camera.SetFovYRadians(mViewBeforeLockFov);
        mCinematicLockActive = false;
    }
}

void Editor::DrawCinematicViewportMenuItems()
{
    bool locked = SequencerEditor::IsCameraLocked();
    if (QtUi::MenuItem("Lock to Cinematic Camera", nullptr, &locked))
        SequencerEditor::SetCameraLock(locked);

    Entity* selected = GetSelectedEntity();
    if (mPilotedCameraId != 0)
    {
        if (QtUi::MenuItem("Stop Piloting Camera"))
            StopPilotingCamera();
    }
    else if (QtUi::MenuItem("Pilot Selected Camera", nullptr, false, selected != nullptr && selected->Camera.has_value()))
    {
        ::EnsureEntityIds(mEntities);
        selected = GetSelectedEntity();
        if (selected != nullptr)
            PilotCamera(selected->Id);
    }
}

void Editor::DrawCameraProperties(Entity& entity)
{
    if (!QtUi::CollapsingHeader("Camera", QtUiTreeNodeFlags_DefaultOpen))
        return;

    CameraComponent& lens = *entity.Camera;

    // Unbounded drags clamped by hand: a bounded drag spans its range in a fixed number
    // of steps, which over 1-2000 mm would make every pixel jump several millimetres.
    float fov = lens.VerticalFovDegrees();
    if (QtUi::DragFloat("Field of View##cam", &fov, 0.1f, 0.0f, 0.0f, "%.2f deg"))
    {
        lens.SetVerticalFovDegrees(std::clamp(fov, 1.0f, 170.0f));
        MarkSceneChanged();
    }
    if (QtUi::DragFloat("Focal Length##cam", &lens.FocalLengthMm, 0.1f, 0.0f, 0.0f, "%.1f mm"))
    {
        lens.FocalLengthMm = std::clamp(lens.FocalLengthMm, 1.0f, 2000.0f);
        MarkSceneChanged();
    }

    struct SensorPreset
    {
        const char* Name;
        float Width;
        float Height;
    };
    static const SensorPreset kSensors[] = {
        { "Full Frame (36 x 24)", 36.0f, 24.0f },
        { "Super 35 (24.89 x 18.66)", 24.89f, 18.66f },
        { "APS-C (23.6 x 15.6)", 23.6f, 15.6f },
        { "Micro Four Thirds (17.3 x 13)", 17.3f, 13.0f },
        { "16 mm (12.52 x 7.41)", 12.52f, 7.41f },
    };
    const char* sensorNames[std::size(kSensors) + 1];
    int sensorIndex = static_cast<int>(std::size(kSensors));
    for (int i = 0; i < static_cast<int>(std::size(kSensors)); ++i)
    {
        sensorNames[i] = kSensors[i].Name;
        if (std::fabs(lens.SensorWidthMm - kSensors[i].Width) < 0.01f &&
            std::fabs(lens.SensorHeightMm - kSensors[i].Height) < 0.01f)
            sensorIndex = i;
    }
    sensorNames[std::size(kSensors)] = "Custom";
    if (QtUi::Combo("Sensor##cam", &sensorIndex, sensorNames, static_cast<int>(std::size(sensorNames))) &&
        sensorIndex < static_cast<int>(std::size(kSensors)))
    {
        // Keep the framing: the field of view stays, the focal length follows the sensor.
        const float keepFov = lens.VerticalFovDegrees();
        lens.SensorWidthMm = kSensors[sensorIndex].Width;
        lens.SensorHeightMm = kSensors[sensorIndex].Height;
        lens.SetVerticalFovDegrees(keepFov);
        MarkSceneChanged();
    }
    if (QtUi::DragFloat("Sensor Width##cam", &lens.SensorWidthMm, 0.05f, 0.0f, 0.0f, "%.2f mm"))
    {
        lens.SensorWidthMm = std::clamp(lens.SensorWidthMm, 1.0f, 100.0f);
        MarkSceneChanged();
    }
    if (QtUi::DragFloat("Sensor Height##cam", &lens.SensorHeightMm, 0.05f, 0.0f, 0.0f, "%.2f mm"))
    {
        lens.SensorHeightMm = std::clamp(lens.SensorHeightMm, 1.0f, 100.0f);
        MarkSceneChanged();
    }
    if (QtUi::DragFloat("Near Clip##cam", &lens.NearClip, 0.001f, 0.0f, 0.0f, "%.3f m"))
    {
        lens.NearClip = std::clamp(lens.NearClip, 0.001f, 100.0f);
        MarkSceneChanged();
    }
    if (QtUi::DragFloat("Shake Amplitude##cam", &lens.ShakeAmplitudeDegrees, 0.01f, 0.0f, 0.0f, "%.2f deg"))
    {
        lens.ShakeAmplitudeDegrees = std::clamp(lens.ShakeAmplitudeDegrees, 0.0f, 45.0f);
        MarkSceneChanged();
    }
    if (QtUi::DragFloat("Shake Frequency##cam", &lens.ShakeFrequency, 0.01f, 0.0f, 0.0f, "%.2f Hz"))
    {
        lens.ShakeFrequency = std::clamp(lens.ShakeFrequency, 0.0f, 50.0f);
        MarkSceneChanged();
    }
    QtUi::SetItemTooltip("Shake shows while a sequence cuts to this camera, driven by sequence time.");

    const bool piloting = mPilotedCameraId != 0 && mPilotedCameraId == entity.Id;
    if (QtUi::Button(piloting ? "Stop Piloting##cam" : "Pilot Camera##cam"))
    {
        if (piloting)
        {
            StopPilotingCamera();
        }
        else
        {
            ::EnsureEntityIds(mEntities);
            PilotCamera(entity.Id);
        }
    }
    QtUi::SetItemTooltip("Fly the viewport to move this camera. The viewport takes the camera's lens.");
}
