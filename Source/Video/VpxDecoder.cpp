#include "VpxDecoder.h"

#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"

#include <algorithm>
#include <array>
#include <thread>

namespace
{
    // Fixed-point YUV -> RGB lookup tables, 16 fractional bits. One set per matrix and
    // range, built on first use; the conversion then costs a few table reads per pixel.
    struct YuvTables
    {
        std::array<int, 256> Y{};
        std::array<int, 256> RV{};
        std::array<int, 256> GU{};
        std::array<int, 256> GV{};
        std::array<int, 256> BU{};
    };

    YuvTables BuildTables(bool bt709, bool fullRange)
    {
        // Kr/Kb define the matrix; everything else follows from them.
        const double kr = bt709 ? 0.2126 : 0.299;
        const double kb = bt709 ? 0.0722 : 0.114;
        const double kg = 1.0 - kr - kb;
        const double yScale = fullRange ? 1.0 : 255.0 / 219.0;
        const double cScale = fullRange ? 1.0 : 255.0 / 224.0;
        const double yOffset = fullRange ? 0.0 : 16.0;

        const double rv = 2.0 * (1.0 - kr) * cScale;
        const double bu = 2.0 * (1.0 - kb) * cScale;
        const double gu = -2.0 * (1.0 - kb) * kb / kg * cScale;
        const double gv = -2.0 * (1.0 - kr) * kr / kg * cScale;

        YuvTables tables;
        constexpr double one = 65536.0;
        for (int i = 0; i < 256; ++i)
        {
            const double c = static_cast<double>(i) - 128.0;
            // Half an LSB folded into Y rounds every channel instead of truncating it.
            tables.Y[i] = static_cast<int>((static_cast<double>(i) - yOffset) * yScale * one + one * 0.5);
            tables.RV[i] = static_cast<int>(c * rv * one);
            tables.GU[i] = static_cast<int>(c * gu * one);
            tables.GV[i] = static_cast<int>(c * gv * one);
            tables.BU[i] = static_cast<int>(c * bu * one);
        }

        return tables;
    }

    const YuvTables& GetTables(bool bt709, bool fullRange)
    {
        static const YuvTables tables[4] = {
            BuildTables(false, false), BuildTables(false, true), BuildTables(true, false), BuildTables(true, true)
        };
        return tables[(bt709 ? 2 : 0) + (fullRange ? 1 : 0)];
    }

    inline std::uint8_t Clamp8(int value)
    {
        value >>= 16;
        return static_cast<std::uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
    }
}

VpxDecoder::VpxDecoder() = default;

VpxDecoder::~VpxDecoder()
{
    Close();
}

bool VpxDecoder::Open(WebmDemuxer::Codec codec, std::string& error)
{
    Close();

    vpx_codec_iface_t* iface = nullptr;
    switch (codec)
    {
    case WebmDemuxer::Codec::VP8: iface = vpx_codec_vp8_dx(); break;
    case WebmDemuxer::Codec::VP9: iface = vpx_codec_vp9_dx(); break;
    default: break;
    }

    if (iface == nullptr)
    {
        error = "Unsupported video codec.";
        return false;
    }

    vpx_codec_dec_cfg_t config{};
    // VP9 splits a frame into tiles it can decode in parallel; the worker threads are
    // what keeps a 1080p/4K stream real time in a debug build of the engine.
    config.threads = (std::min)((std::max)(std::thread::hardware_concurrency(), 1u), 8u);

    mContext = new vpx_codec_ctx_t{};
    if (vpx_codec_dec_init(mContext, iface, &config, 0) != VPX_CODEC_OK)
    {
        error = std::string("libvpx failed to initialise: ") + vpx_codec_error(mContext);
        delete mContext;
        mContext = nullptr;
        return false;
    }

    if (codec == WebmDemuxer::Codec::VP9)
    {
        // Row-based multithreading also helps streams that were encoded with one tile column.
        vpx_codec_control(mContext, VP9D_SET_ROW_MT, 1);
    }

    return true;
}

void VpxDecoder::Close()
{
    if (mContext != nullptr)
    {
        vpx_codec_destroy(mContext);
        delete mContext;
        mContext = nullptr;
    }
}

bool VpxDecoder::Decode(const std::vector<std::uint8_t>& packet, std::vector<std::uint8_t>& bgra, int& width,
                        int& height, std::string& error)
{
    if (mContext == nullptr || packet.empty())
    {
        return false;
    }

    if (vpx_codec_decode(mContext, packet.data(), static_cast<unsigned int>(packet.size()), nullptr, 0) !=
        VPX_CODEC_OK)
    {
        const char* detail = vpx_codec_error_detail(mContext);
        error = std::string("Decode failed: ") + vpx_codec_error(mContext) + (detail ? std::string(" (") + detail + ")" : "");
        return false;
    }

    // A superframe may carry several pictures; only the last one is shown.
    vpx_codec_iter_t iterator = nullptr;
    const vpx_image_t* shown = nullptr;
    while (const vpx_image_t* image = vpx_codec_get_frame(mContext, &iterator))
    {
        shown = image;
    }

    if (shown == nullptr)
    {
        return false;
    }

    if ((shown->fmt & VPX_IMG_FMT_HIGHBITDEPTH) != 0)
    {
        error = "High bit-depth video is not supported. Re-import it to convert it to 8-bit.";
        return false;
    }

    width = static_cast<int>(shown->d_w);
    height = static_cast<int>(shown->d_h);
    bgra.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 4);
    ConvertToBgra(*shown, bgra.data(), width * 4);
    return true;
}

void VpxDecoder::ConvertToBgra(const vpx_image& image, std::uint8_t* destination, int pitch)
{
    // Streams that do not say which matrix they use are, by convention, BT.709 when HD.
    bool bt709 = image.cs == VPX_CS_BT_709;
    if (image.cs == VPX_CS_UNKNOWN)
    {
        bt709 = image.d_h >= 720;
    }

    const YuvTables& t = GetTables(bt709, image.range == VPX_CR_FULL_RANGE);
    const int width = static_cast<int>(image.d_w);
    const int height = static_cast<int>(image.d_h);
    const unsigned xShift = image.x_chroma_shift;
    const unsigned yShift = image.y_chroma_shift;

    for (int y = 0; y < height; ++y)
    {
        const std::uint8_t* yRow = image.planes[VPX_PLANE_Y] + static_cast<ptrdiff_t>(y) * image.stride[VPX_PLANE_Y];
        const std::uint8_t* uRow =
            image.planes[VPX_PLANE_U] + static_cast<ptrdiff_t>(y >> yShift) * image.stride[VPX_PLANE_U];
        const std::uint8_t* vRow =
            image.planes[VPX_PLANE_V] + static_cast<ptrdiff_t>(y >> yShift) * image.stride[VPX_PLANE_V];
        std::uint8_t* out = destination + static_cast<ptrdiff_t>(y) * pitch;

        for (int x = 0; x < width; ++x)
        {
            const int u = uRow[x >> xShift];
            const int v = vRow[x >> xShift];
            const int luma = t.Y[yRow[x]];
            out[0] = Clamp8(luma + t.BU[u]);
            out[1] = Clamp8(luma + t.GU[u] + t.GV[v]);
            out[2] = Clamp8(luma + t.RV[v]);
            out[3] = 255;
            out += 4;
        }
    }
}
