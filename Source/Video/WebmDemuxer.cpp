#include "WebmDemuxer.h"

#include "mkvparser/mkvparser.h"

#include "System/DataFiles.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

// mkvparser's own MkvReader opens files through fopen, which cannot reach a path outside
// the ANSI code page. This one is the same thing over _wfopen - or, for a packaged game,
// over the video's bytes decrypted out of Videos.ppak into memory (see DataFiles.h).
class WebmDemuxer::FileReader final : public mkvparser::IMkvReader
{
public:
    ~FileReader() override
    {
        if (mFile != nullptr)
        {
            std::fclose(mFile);
        }
    }

    bool Open(const std::wstring& path)
    {
        if (!DataFiles::PackagedRelativePath(path).empty())
        {
            mInMemory = true;
            if (!DataFiles::ReadBytes(path, mMemory))
            {
                return false;
            }
            mLength = static_cast<long long>(mMemory.size());
            return true;
        }

        if (_wfopen_s(&mFile, path.c_str(), L"rb") != 0 || mFile == nullptr)
        {
            mFile = nullptr;
            return false;
        }

        _fseeki64(mFile, 0, SEEK_END);
        mLength = _ftelli64(mFile);
        _fseeki64(mFile, 0, SEEK_SET);
        return mLength >= 0;
    }

    int Read(long long position, long length, unsigned char* buffer) override
    {
        if ((mFile == nullptr && !mInMemory) || position < 0 || length < 0)
        {
            return -1;
        }

        if (length == 0)
        {
            return 0;
        }

        if (position >= mLength)
        {
            return -1;
        }

        if (mInMemory)
        {
            if (position + length > mLength)
            {
                return -1;
            }
            std::memcpy(buffer, mMemory.data() + position, static_cast<size_t>(length));
            return 0;
        }

        if (_fseeki64(mFile, position, SEEK_SET) != 0)
        {
            return -1;
        }

        return std::fread(buffer, 1, static_cast<size_t>(length), mFile) == static_cast<size_t>(length) ? 0 : -1;
    }

    int Length(long long* total, long long* available) override
    {
        if (mFile == nullptr && !mInMemory)
        {
            return -1;
        }

        if (total != nullptr)
        {
            *total = mLength;
        }

        if (available != nullptr)
        {
            *available = mLength;
        }

        return 0;
    }

private:
    FILE* mFile = nullptr;
    bool mInMemory = false;
    std::vector<std::uint8_t> mMemory;
    long long mLength = 0;
};

WebmDemuxer::WebmDemuxer() = default;

WebmDemuxer::~WebmDemuxer()
{
    Close();
}

bool WebmDemuxer::Open(const std::wstring& path, std::string& error, Kind kind)
{
    Close();

    mReader = std::make_unique<FileReader>();
    if (!mReader->Open(path))
    {
        error = "The file could not be opened.";
        Close();
        return false;
    }

    long long position = 0;
    mkvparser::EBMLHeader header;
    if (header.Parse(mReader.get(), position) < 0)
    {
        error = "Not a WebM/Matroska file.";
        Close();
        return false;
    }

    mkvparser::Segment* segment = nullptr;
    if (mkvparser::Segment::CreateInstance(mReader.get(), position, segment) != 0 || segment == nullptr)
    {
        error = "The WebM segment could not be read.";
        Close();
        return false;
    }

    mSegment.reset(segment);
    if (mSegment->Load() < 0)
    {
        error = "The WebM segment is damaged.";
        Close();
        return false;
    }

    const long wantedType = kind == Kind::Video ? mkvparser::Track::kVideo : mkvparser::Track::kAudio;
    const mkvparser::Tracks* tracks = mSegment->GetTracks();
    for (unsigned long i = 0; tracks != nullptr && i < tracks->GetTracksCount(); ++i)
    {
        const mkvparser::Track* track = tracks->GetTrackByIndex(i);
        if (track == nullptr || track->GetType() != wantedType || track->GetCodecId() == nullptr)
        {
            continue;
        }

        const char* codecId = track->GetCodecId();
        if (kind == Kind::Video && std::strcmp(codecId, "V_VP9") == 0)
        {
            mCodec = Codec::VP9;
        }
        else if (kind == Kind::Video && std::strcmp(codecId, "V_VP8") == 0)
        {
            mCodec = Codec::VP8;
        }
        else if (kind == Kind::Audio && std::strcmp(codecId, "A_OPUS") == 0)
        {
            mCodec = Codec::Opus;
        }
        else
        {
            continue;
        }

        mTrack = track;
        break;
    }

    if (mTrack == nullptr)
    {
        error = kind == Kind::Video ? "The file has no VP8 or VP9 video track. Re-import it to convert it."
                                    : "The file has no Opus audio track.";
        Close();
        return false;
    }

    if (kind == Kind::Video)
    {
        const auto* videoTrack = static_cast<const mkvparser::VideoTrack*>(mTrack);
        mWidth = static_cast<int>(videoTrack->GetWidth());
        mHeight = static_cast<int>(videoTrack->GetHeight());
        mFrameRate = videoTrack->GetFrameRate();
    }
    else
    {
        const auto* audioTrack = static_cast<const mkvparser::AudioTrack*>(mTrack);
        mChannels = static_cast<int>(audioTrack->GetChannels());
        mSampleRate = static_cast<int>(audioTrack->GetSamplingRate());
        // Opus always decodes at 48 kHz whatever the original rate was.
        if (mSampleRate <= 0)
        {
            mSampleRate = 48000;
        }

        mCodecDelay = static_cast<double>(mTrack->GetCodecDelay()) / 1e9;
        std::size_t codecPrivateSize = 0;
        if (const unsigned char* codecPrivate = mTrack->GetCodecPrivate(codecPrivateSize))
        {
            mCodecPrivate.assign(codecPrivate, codecPrivate + codecPrivateSize);
        }
    }

    const long long durationNs = mSegment->GetDuration();
    mDuration = durationNs > 0 ? static_cast<double>(durationNs) / 1e9 : 0.0;

    // Muxers that stream (ffmpeg writing to a pipe, recorders) leave the duration out.
    // The last block of the last cluster is as good an answer.
    if (mDuration <= 0.0)
    {
        if (const mkvparser::Cluster* last = mSegment->GetLast(); last != nullptr && !last->EOS())
        {
            mDuration = (std::max)(static_cast<double>(last->GetLastTime()) / 1e9, 0.0);
        }
    }

    if (mTrack->GetFirst(mEntry) != 0)
    {
        mEntry = nullptr;
    }

    mFrameInBlock = 0;

    // Most muxers, ffmpeg's included, leave the frame rate out, so it is measured from
    // the first few seconds of timestamps. WebM stores those in whole milliseconds (30 fps
    // alternates 33 and 34 ms gaps), hence averaging over many frames and snapping to the
    // standard rates.
    if (kind == Kind::Video && mFrameRate <= 0.0)
    {
        Packet packet;
        double first = -1.0;
        double last = 0.0;
        int count = 0;
        for (; count < 120 && ReadPacket(packet); ++count)
        {
            if (first < 0.0)
            {
                first = packet.Time;
            }

            last = packet.Time;
        }

        if (count > 1 && last > first)
        {
            const double measured = static_cast<double>(count - 1) / (last - first);
            double nearest = 0.0;
            for (const double standard : { 23.976, 24.0, 25.0, 29.97, 30.0, 48.0, 50.0, 59.94, 60.0, 90.0, 100.0,
                                           119.88, 120.0, 144.0 })
            {
                if (std::abs(measured - standard) < std::abs(measured - nearest))
                {
                    nearest = standard;
                }
            }

            mFrameRate = std::abs(measured - nearest) < nearest * 0.002 ? nearest
                                                                          : std::round(measured * 1000.0) / 1000.0;
        }

        if (mTrack->GetFirst(mEntry) != 0)
        {
            mEntry = nullptr;
        }

        mFrameInBlock = 0;
    }

    return true;
}

void WebmDemuxer::Close()
{
    mEntry = nullptr;
    mTrack = nullptr;
    mSegment.reset();
    mReader.reset();
    mCodec = Codec::Unknown;
    mWidth = 0;
    mHeight = 0;
    mDuration = 0.0;
    mFrameRate = 0.0;
    mChannels = 0;
    mSampleRate = 0;
    mCodecDelay = 0.0;
    mCodecPrivate.clear();
    mFrameInBlock = 0;
}

bool WebmDemuxer::Advance()
{
    mFrameInBlock = 0;
    const mkvparser::BlockEntry* next = nullptr;
    if (mTrack->GetNext(mEntry, next) != 0)
    {
        next = nullptr;
    }

    mEntry = next;
    return mEntry != nullptr && !mEntry->EOS();
}

bool WebmDemuxer::ReadPacket(Packet& packet)
{
    if (mTrack == nullptr)
    {
        return false;
    }

    while (mEntry != nullptr && !mEntry->EOS())
    {
        const mkvparser::Block* block = mEntry->GetBlock();
        if (block == nullptr || mFrameInBlock >= block->GetFrameCount())
        {
            if (!Advance())
            {
                return false;
            }

            continue;
        }

        const mkvparser::Block::Frame& frame = block->GetFrame(mFrameInBlock);
        if (frame.len <= 0 || frame.len > (std::numeric_limits<int>::max)())
        {
            ++mFrameInBlock;
            continue;
        }

        packet.Data.resize(static_cast<size_t>(frame.len));
        if (frame.Read(mReader.get(), packet.Data.data()) != 0)
        {
            mEntry = nullptr;
            return false;
        }

        // Laced frames carry one timestamp for the whole block; spread them by the frame
        // rate when it is known so the playhead does not show them all at once. (Audio
        // packets are timed from the block as well; the decoder counts samples from there.)
        double time = static_cast<double>(block->GetTime(mEntry->GetCluster())) / 1e9;
        if (mFrameInBlock > 0 && mFrameRate > 0.0)
        {
            time += static_cast<double>(mFrameInBlock) / mFrameRate;
        }

        packet.Time = time;
        packet.IsKey = block->IsKey() && mFrameInBlock == 0;
        ++mFrameInBlock;
        return true;
    }

    return false;
}

bool WebmDemuxer::SeekToKeyFrame(double seconds)
{
    if (mTrack == nullptr)
    {
        return false;
    }

    const long long timeNs = static_cast<long long>((std::max)(seconds, 0.0) * 1e9);
    const mkvparser::BlockEntry* entry = nullptr;
    if (mTrack->Seek(timeNs, entry) != 0 || entry == nullptr || entry->EOS())
    {
        // Seek fails past the last cue; restarting is always valid.
        if (mTrack->GetFirst(entry) != 0)
        {
            return false;
        }
    }

    mEntry = entry;
    mFrameInBlock = 0;
    return true;
}
