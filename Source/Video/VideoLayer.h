#pragma once

#include "VideoAPI.h"
#include "VideoTexture.h"

#include <d3d12.h>

#include <memory>
#include <string>

// A full-screen video shown over the game: cut-scenes, intros, video backgrounds.
//
// This is what the Node Graph's video nodes drive. The renderer owns one, updates it
// once per game frame and calls Record() where the layer belongs in the frame - over the
// scene, under the game UI - and everything else lives here.
class VIDEO_API VideoLayer
{
public:
    VideoLayer();
    ~VideoLayer();

    VideoLayer(const VideoLayer&) = delete;
    VideoLayer& operator=(const VideoLayer&) = delete;

    // See VideoTexture::Initialize.
    bool InitializeGpu(ID3D12Device* device,
                       const D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandles,
                       const D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandles,
                       DXGI_FORMAT renderTargetFormat);
    void ShutdownGpu();
    bool IsGpuInitialized() const;

    // `fileName` is relative to the Data folder, ".webm" optional. When it is not found
    // there, the first file of that name anywhere under Data is used, so a graph can say
    // "Intro" without knowing which folder the video was imported into. `fit` is
    // "Letterbox", "Fill" or "Stretch".
    bool Play(const std::string& fileName, bool loop, const std::string& fit);
    void Pause();
    void Resume();
    // Ends playback and hides the layer.
    void Stop();
    void Seek(double seconds);
    void SetLooping(bool loop);
    // Linear gain 0..1, remembered across videos.
    void SetVolume(float volume);
    float GetVolume() const;
    // False when the video has no soundtrack the engine can decode.
    bool HasAudio() const;

    bool IsVisible() const;
    bool IsPlaying() const;
    double GetTime() const;
    double GetDuration() const;
    // True once after a non-looping video reached its end. The layer hides itself then.
    bool ConsumeFinished();
    const std::string& GetLastError() const;

    // Advances playback by the game's frame time, so pausing the game pauses the video.
    void Update(double elapsedSeconds);

    // Uploads the current picture and draws it over the bound render target. Call once
    // per rendered frame whether or not the layer is visible; it ages GPU resources.
    void Record(ID3D12GraphicsCommandList* commandList, unsigned targetWidth, unsigned targetHeight);

private:
    struct Impl;
#pragma warning(push)
#pragma warning(disable : 4251)
    std::unique_ptr<Impl> mImpl;
#pragma warning(pop)
};
