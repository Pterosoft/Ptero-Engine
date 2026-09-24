#pragma once

#include "WebmDemuxer.h"

#include <cstdint>
#include <string>
#include <vector>

struct vpx_codec_ctx;
struct vpx_image;

// libvpx VP8/VP9 decoder that hands pictures back as BGRA.
class VpxDecoder
{
public:
    VpxDecoder();
    ~VpxDecoder();

    VpxDecoder(const VpxDecoder&) = delete;
    VpxDecoder& operator=(const VpxDecoder&) = delete;

    bool Open(WebmDemuxer::Codec codec, std::string& error);
    void Close();

    // Decodes one compressed frame. Returns true when it produced a visible picture,
    // which is then converted into `bgra` (resized as needed). Hidden reference frames
    // decode fine but produce no picture.
    bool Decode(const std::vector<std::uint8_t>& packet, std::vector<std::uint8_t>& bgra, int& width, int& height,
                std::string& error);

private:
    static void ConvertToBgra(const vpx_image& image, std::uint8_t* destination, int pitch);

    vpx_codec_ctx* mContext = nullptr;
};
