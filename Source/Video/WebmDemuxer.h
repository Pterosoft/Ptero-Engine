#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace mkvparser
{
    class Segment;
    class Track;
    class BlockEntry;
}

// Reads one track of a WebM file, one compressed frame at a time: either the first VP8/VP9
// video track or the first Opus audio track.
//
// Audio is read through a second instance with its own file handle rather than a second
// cursor on this one, so the audio thread never touches the video thread's reader.
class WebmDemuxer
{
public:
    enum class Kind
    {
        Video,
        Audio
    };

    enum class Codec
    {
        Unknown,
        VP8,
        VP9,
        Opus
    };

    struct Packet
    {
        std::vector<std::uint8_t> Data;
        double Time = 0.0;
        bool IsKey = false;
    };

    WebmDemuxer();
    ~WebmDemuxer();

    // Fails when the file holds no track of that kind in a codec the engine can decode.
    bool Open(const std::wstring& path, std::string& error, Kind kind = Kind::Video);
    void Close();

    // False at the end of the stream (and on a read error, which ends the stream too).
    bool ReadPacket(Packet& packet);

    // Positions the reader on the last key frame at or before `seconds`, so decoding
    // from there reaches the requested time with a valid reference chain.
    bool SeekToKeyFrame(double seconds);

    Codec GetCodec() const { return mCodec; }
    int GetWidth() const { return mWidth; }
    int GetHeight() const { return mHeight; }
    double GetDuration() const { return mDuration; }
    double GetFrameRate() const { return mFrameRate; }

    // Audio only. The codec private data is the OpusHead the decoder is configured from,
    // and the codec delay is how much of the decoded signal precedes time zero.
    int GetChannels() const { return mChannels; }
    int GetSampleRate() const { return mSampleRate; }
    double GetCodecDelay() const { return mCodecDelay; }
    const std::vector<std::uint8_t>& GetCodecPrivate() const { return mCodecPrivate; }

private:
    class FileReader;

    bool Advance();

    std::unique_ptr<FileReader> mReader;
    std::unique_ptr<mkvparser::Segment> mSegment;
    const mkvparser::Track* mTrack = nullptr;
    const mkvparser::BlockEntry* mEntry = nullptr;
    // Several frames may share one block (lacing).
    int mFrameInBlock = 0;
    Codec mCodec = Codec::Unknown;
    int mWidth = 0;
    int mHeight = 0;
    double mDuration = 0.0;
    double mFrameRate = 0.0;
    int mChannels = 0;
    int mSampleRate = 0;
    double mCodecDelay = 0.0;
    std::vector<std::uint8_t> mCodecPrivate;
};
