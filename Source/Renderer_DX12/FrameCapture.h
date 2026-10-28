#pragma once

#include <d3d12.h>

#include <cstdint>
#include <string>
#include <vector>

// Reading the final viewport image back from the GPU and writing it to an image file.
// Used by the viewport screenshot and by Ptero Sequencer's Render Sequence.
//
// Readback copies on the context's queue and waits for it, so it sees the last frame the
// queue finished - while a frame is being recorded, that is the previous one. Callers that
// capture every frame (the cinematic render) account for that latency.
namespace FrameCapture
{
    enum class ImageFormat
    {
        Png,
        Jpeg
    };

    // `texture` must be in PIXEL_SHADER_RESOURCE state; it is returned to it. Supports the
    // half-float and 8-bit RGBA/BGRA formats the post chain produces. Pixels come back as
    // tightly packed 8-bit BGRA, the half-float values clamped to [0, 1].
    bool ReadbackBgra(ID3D12Resource* texture, std::vector<std::uint8_t>& pixels, std::uint32_t& width,
                      std::uint32_t& height);

    // `quality` (1-100) only affects JPEG.
    bool WriteImage(const std::string& path, const std::vector<std::uint8_t>& bgraPixels, std::uint32_t width,
                    std::uint32_t height, ImageFormat format, int quality = 95);

    bool SaveTexture(ID3D12Resource* texture, const std::string& path, ImageFormat format, int quality = 95);
}
