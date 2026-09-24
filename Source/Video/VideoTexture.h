#pragma once

#include "VideoAPI.h"
#include "VideoPlayer.h"

#include <d3d12.h>

#include <cstdint>
#include <memory>

// Streams a VideoPlayer's pictures into a D3D12 texture and draws it over a render target.
//
// The engine keeps three frames in flight, which shapes all of this:
//  - frame uploads go through a ring of UPLOAD buffers, so a frame the GPU is still
//    copying from is never overwritten;
//  - a texture replaced because the video size changed is released a few frames later,
//    not immediately, since earlier command lists still reference it;
//  - the SRV slots are handed in once and rotated on replacement rather than rewritten,
//    because the shared heap has no free list and an in-flight frame may still be
//    reading the old slot.
class VIDEO_API VideoTexture
{
public:
    static constexpr int kDescriptorCount = 4;

    enum class Fit
    {
        // Whole picture visible, black bars where the aspect ratios differ.
        Letterbox,
        // Fills the target, cropping whatever overhangs.
        Fill,
        Stretch
    };

    VideoTexture();
    ~VideoTexture();

    VideoTexture(const VideoTexture&) = delete;
    VideoTexture& operator=(const VideoTexture&) = delete;

    // `cpuHandles`/`gpuHandles` are kDescriptorCount slots in the shader-visible heap
    // that will be bound when Draw is recorded. `renderTargetFormat` is the format Draw
    // renders into.
    bool Initialize(ID3D12Device* device,
                    const D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandles,
                    const D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandles,
                    DXGI_FORMAT renderTargetFormat);
    // The caller must make sure the GPU has finished with every frame that used this.
    void Shutdown();
    bool IsInitialized() const;

    // Call once per rendered frame, before Upload/Draw. Ages retired resources.
    void BeginFrame();

    // Records the copy of `frame` into the texture when its serial is new. Returns false
    // only on a device error.
    bool Upload(ID3D12GraphicsCommandList* commandList, const VideoPlayer::Frame& frame);

    // True once a picture has been uploaded.
    bool HasPicture() const;
    // Forgets the current picture, so HasPicture() is false until the next Upload.
    void Clear();

    // Draws the picture over the whole of the currently bound render target, which is
    // `targetWidth` x `targetHeight`. The caller binds the render target and the SRV heap.
    void Draw(ID3D12GraphicsCommandList* commandList, unsigned targetWidth, unsigned targetHeight, Fit fit) const;

    std::uint64_t GetGpuDescriptor() const;
    int GetWidth() const;
    int GetHeight() const;

private:
    struct Impl;
#pragma warning(push)
#pragma warning(disable : 4251)
    std::unique_ptr<Impl> mImpl;
#pragma warning(pop)
};
