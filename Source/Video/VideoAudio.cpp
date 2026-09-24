#include "VideoAudio.h"

#include "WebmDemuxer.h"

#include "fmod.hpp"
#include "opus.h"
#include "opus_multistream.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    // Opus always decodes at 48 kHz, and everything is mixed down to stereo: a game's
    // cut-scene does not need its own surround path, and FMOD upmixes to the output layout.
    constexpr int kSampleRate = 48000;
    constexpr int kChannels = 2;
    // Longest Opus packet is 120 ms.
    constexpr int kMaxFramesPerPacket = kSampleRate / 1000 * 120;
    // Decoded audio held ahead of the playhead.
    constexpr size_t kRingFrames = kSampleRate;  // one second
    // How far the sound may drift from the video before it is repositioned rather than
    // nudged. Below this, a rate nudge pulls it back inaudibly.
    constexpr double kResyncThreshold = 0.12;
    constexpr double kMaxNudge = 0.01;  // 1% of playback rate
    // How long a starved read waits for the decoder before it gives up and feeds silence.
    constexpr int kUnderrunWaitMs = 20;

    // ---- shared FMOD system ----------------------------------------------------------
    //
    // One core system for every video in the process, created with the first soundtrack.
    // It is separate from the game's FMOD Studio system on purpose: video audio has no
    // events, banks or 3D positioning, and a second core system is explicitly allowed.

    std::mutex gSystemMutex;
    FMOD::System* gSystem = nullptr;
    int gSystemUsers = 0;
    std::thread gUpdateThread;
    std::atomic<bool> gUpdateQuit{ false };

    FMOD::System* AcquireSystem()
    {
        std::lock_guard<std::mutex> lock(gSystemMutex);
        if (gSystem == nullptr)
        {
            if (FMOD::System_Create(&gSystem) != FMOD_OK)
            {
                gSystem = nullptr;
                return nullptr;
            }

            if (gSystem->init(16, FMOD_INIT_NORMAL, nullptr) != FMOD_OK)
            {
                gSystem->release();
                gSystem = nullptr;
                return nullptr;
            }

            gUpdateQuit.store(false);
            gUpdateThread = std::thread([] {
                while (!gUpdateQuit.load())
                {
                    {
                        std::lock_guard<std::mutex> lock(gSystemMutex);
                        if (gSystem != nullptr)
                        {
                            gSystem->update();
                        }
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
            });
        }

        ++gSystemUsers;
        return gSystem;
    }

    void ReleaseSystem()
    {
        std::thread updateThread;
        {
            std::lock_guard<std::mutex> lock(gSystemMutex);
            if (--gSystemUsers > 0 || gSystem == nullptr)
            {
                return;
            }

            gUpdateQuit.store(true);
            updateThread = std::move(gUpdateThread);
        }

        if (updateThread.joinable())
        {
            updateThread.join();
        }

        std::lock_guard<std::mutex> lock(gSystemMutex);
        if (gSystem != nullptr && gSystemUsers == 0)
        {
            gSystem->release();
            gSystem = nullptr;
        }
    }
}

struct VideoAudio::Impl
{
    // ---- decoding ---------------------------------------------------------------------
    WebmDemuxer Demuxer;
    OpusMSDecoder* Decoder = nullptr;
    int SourceChannels = 0;
    double CodecDelay = 0.0;
    double Duration = 0.0;

    std::thread Worker;
    std::mutex Mutex;
    std::condition_variable WorkerWake;
    std::condition_variable DataReady;
    bool Quit = false;
    bool SeekPending = false;
    double SeekTarget = 0.0;
    bool EndOfStream = false;

    // Ring of interleaved stereo frames. Its first frame is at media time RingStartTime,
    // which is what keeps the sound aligned with the stream position FMOD reads from.
    std::vector<float> Ring;
    size_t RingRead = 0;
    size_t RingWrite = 0;
    size_t RingCount = 0;
    // Frames of silence handed to FMOD because the decoder was behind. The same number of
    // decoded frames is dropped when they arrive, so the sound does not slip a whole
    // underrun behind the picture.
    size_t SilenceDebt = 0;

    // ---- output -----------------------------------------------------------------------
    FMOD::System* System = nullptr;
    FMOD::Sound* Sound = nullptr;
    FMOD::Channel* Channel = nullptr;
    float Volume = 1.0f;
    bool Paused = true;

    // Diagnostics, read through VideoAudio::GetStatus.
    std::atomic<long long> DecodedFrames{ 0 };
    std::atomic<long long> UnderrunFrames{ 0 };
    std::atomic<int> Resyncs{ 0 };

    size_t RingSpace() const { return Ring.size() / kChannels - RingCount; }

    void PushFrames(const float* samples, size_t frames)
    {
        for (size_t i = 0; i < frames; ++i)
        {
            Ring[RingWrite * kChannels] = samples[i * kChannels];
            Ring[RingWrite * kChannels + 1] = samples[i * kChannels + 1];
            RingWrite = (RingWrite + 1) % (Ring.size() / kChannels);
            ++RingCount;
        }
    }

    // Mixes the decoder's channel layout down to stereo. Multichannel Opus uses the Vorbis
    // channel order, so 5.1 is FL, C, FR, RL, RR, LFE.
    void AppendDownmixed(const float* decoded, int frames, std::vector<float>& stereo) const
    {
        stereo.resize(static_cast<size_t>(frames) * kChannels);
        const int channels = SourceChannels;
        for (int i = 0; i < frames; ++i)
        {
            const float* in = decoded + static_cast<size_t>(i) * channels;
            float left = 0.0f;
            float right = 0.0f;
            switch (channels)
            {
            case 1:
                left = right = in[0];
                break;
            case 2:
                left = in[0];
                right = in[1];
                break;
            case 6:
                left = in[0] + 0.707f * in[1] + 0.707f * in[3] + 0.5f * in[5];
                right = in[2] + 0.707f * in[1] + 0.707f * in[4] + 0.5f * in[5];
                break;
            default:
                // Anything else: even channels left, odd channels right.
                for (int c = 0; c < channels; ++c)
                {
                    (c % 2 == 0 ? left : right) += in[c];
                }
                left /= static_cast<float>((channels + 1) / 2);
                right /= static_cast<float>(channels / 2);
                break;
            }

            stereo[static_cast<size_t>(i) * kChannels] = std::clamp(left, -1.0f, 1.0f);
            stereo[static_cast<size_t>(i) * kChannels + 1] = std::clamp(right, -1.0f, 1.0f);
        }
    }

    void WorkerMain()
    {
        std::vector<float> decoded(static_cast<size_t>(kMaxFramesPerPacket) * 8);
        std::vector<float> stereo;
        WebmDemuxer::Packet packet;
        // Samples earlier than this are decoded for their state but not played: the
        // pre-skip at the start of a stream, and everything between a seek's key frame and
        // the target.
        double discardBefore = 0.0;

        for (;;)
        {
            {
                std::unique_lock<std::mutex> lock(Mutex);
                WorkerWake.wait(lock, [&] {
                    return Quit || SeekPending || (!EndOfStream && RingSpace() > kMaxFramesPerPacket);
                });

                if (Quit)
                {
                    return;
                }

                if (SeekPending)
                {
                    SeekPending = false;
                    discardBefore = SeekTarget;
                    EndOfStream = false;
                    RingRead = RingWrite = RingCount = 0;
                    SilenceDebt = 0;
                    const double target = SeekTarget;
                    lock.unlock();
                    // Opus needs a little run-up to recover its internal state.
                    Demuxer.SeekToKeyFrame((std::max)(target - 0.08, 0.0));
                    opus_multistream_decoder_ctl(Decoder, OPUS_RESET_STATE);
                    continue;
                }
            }

            if (!Demuxer.ReadPacket(packet))
            {
                std::lock_guard<std::mutex> lock(Mutex);
                if (!SeekPending)
                {
                    EndOfStream = true;
                    DataReady.notify_all();
                }

                continue;
            }

            const int frames = opus_multistream_decode_float(
                Decoder, packet.Data.data(), static_cast<opus_int32>(packet.Data.size()), decoded.data(),
                kMaxFramesPerPacket, 0);
            if (frames <= 0)
            {
                continue;
            }

            // Media time of this packet's first decoded sample. CodecDelay is the part of
            // the decoded signal that precedes time zero, which is what the pre-skip drops.
            const double packetStart = packet.Time - CodecDelay;
            int first = 0;
            if (packetStart < discardBefore)
            {
                first = static_cast<int>(std::ceil((discardBefore - packetStart) * kSampleRate));
                first = (std::min)(first, frames);
            }

            if (first >= frames)
            {
                continue;
            }

            AppendDownmixed(decoded.data() + static_cast<size_t>(first) * SourceChannels, frames - first, stereo);

            std::unique_lock<std::mutex> lock(Mutex);
            if (SeekPending)
            {
                continue;
            }

            size_t offset = 0;
            size_t available = static_cast<size_t>(frames - first);
            if (SilenceDebt > 0)
            {
                const size_t dropped = (std::min)(SilenceDebt, available);
                SilenceDebt -= dropped;
                offset = dropped;
                available -= dropped;
            }

            const size_t written = (std::min)(available, RingSpace());
            PushFrames(stereo.data() + offset * kChannels, written);
            DecodedFrames.fetch_add(static_cast<long long>(written));
            if (written > 0)
            {
                DataReady.notify_all();
            }
        }
    }

    // FMOD asks for the next block of the stream here, on its own thread.
    FMOD_RESULT ReadPcm(void* data, unsigned int lengthBytes)
    {
        float* out = static_cast<float*>(data);
        size_t framesWanted = lengthBytes / (sizeof(float) * kChannels);

        std::unique_lock<std::mutex> lock(Mutex);
        while (framesWanted > 0)
        {
            if (RingCount == 0)
            {
                if (!EndOfStream)
                {
                    WorkerWake.notify_all();
                    DataReady.wait_for(lock, std::chrono::milliseconds(kUnderrunWaitMs),
                                       [&] { return RingCount > 0 || Quit; });
                }

                if (RingCount == 0)
                {
                    // Nothing to play: silence, and remember to drop as much decoded audio
                    // so what follows stays in step with the picture.
                    std::fill(out, out + framesWanted * kChannels, 0.0f);
                    if (!EndOfStream)
                    {
                        SilenceDebt += framesWanted;
                        UnderrunFrames.fetch_add(static_cast<long long>(framesWanted));
                    }

                    break;
                }
            }

            const size_t frames = (std::min)(framesWanted, RingCount);
            for (size_t i = 0; i < frames; ++i)
            {
                *out++ = Ring[RingRead * kChannels];
                *out++ = Ring[RingRead * kChannels + 1];
                RingRead = (RingRead + 1) % (Ring.size() / kChannels);
                --RingCount;
            }

            framesWanted -= frames;
        }

        WorkerWake.notify_all();
        return FMOD_OK;
    }

    void RequestSeek(double seconds)
    {
        {
            std::lock_guard<std::mutex> lock(Mutex);
            SeekPending = true;
            SeekTarget = std::clamp(seconds, 0.0, Duration > 0.0 ? Duration : seconds);
            RingRead = RingWrite = RingCount = 0;
            SilenceDebt = 0;
            EndOfStream = false;
        }

        WorkerWake.notify_all();
    }
};

namespace
{
    VideoAudio::Impl* ImplFromSound(FMOD_SOUND* sound)
    {
        void* userData = nullptr;
        reinterpret_cast<FMOD::Sound*>(sound)->getUserData(&userData);
        return static_cast<VideoAudio::Impl*>(userData);
    }

    FMOD_RESULT F_CALL PcmReadCallback(FMOD_SOUND* sound, void* data, unsigned int lengthBytes)
    {
        VideoAudio::Impl* impl = ImplFromSound(sound);
        return impl != nullptr ? impl->ReadPcm(data, lengthBytes) : FMOD_ERR_FILE_EOF;
    }

    FMOD_RESULT F_CALL PcmSetPosCallback(FMOD_SOUND* sound, int, unsigned int position, FMOD_TIMEUNIT positionType)
    {
        VideoAudio::Impl* impl = ImplFromSound(sound);
        if (impl == nullptr)
        {
            return FMOD_OK;
        }

        // The stream's timeline is media time, so a PCM position converts straight back.
        const double seconds = positionType == FMOD_TIMEUNIT_PCM
            ? static_cast<double>(position) / kSampleRate
            : static_cast<double>(position) / 1000.0;
        impl->RequestSeek(seconds);
        return FMOD_OK;
    }
}

VideoAudio::VideoAudio()
    : mImpl(std::make_unique<Impl>())
{
}

VideoAudio::~VideoAudio()
{
    Close();
}

bool VideoAudio::Open(const std::wstring& path, double duration, std::string& error)
{
    Close();
    Impl& d = *mImpl;

    if (!d.Demuxer.Open(path, error, WebmDemuxer::Kind::Audio))
    {
        return false;
    }

    // OpusHead: channel count, pre-skip and the channel mapping the decoder needs.
    const std::vector<std::uint8_t>& head = d.Demuxer.GetCodecPrivate();
    if (head.size() < 19 || std::memcmp(head.data(), "OpusHead", 8) != 0)
    {
        error = "The Opus track has no valid OpusHead.";
        Close();
        return false;
    }

    const int channels = head[9];
    const int mappingFamily = head[18];
    int streams = 1;
    int coupledStreams = channels == 2 ? 1 : 0;
    unsigned char mapping[255] = { 0, 1 };
    if (mappingFamily != 0)
    {
        if (head.size() < static_cast<size_t>(21 + channels))
        {
            error = "The Opus channel mapping table is truncated.";
            Close();
            return false;
        }

        streams = head[19];
        coupledStreams = head[20];
        std::memcpy(mapping, head.data() + 21, static_cast<size_t>(channels));
    }

    if (channels < 1 || channels > 255)
    {
        error = "Unsupported Opus channel count.";
        Close();
        return false;
    }

    int opusError = 0;
    d.Decoder = opus_multistream_decoder_create(kSampleRate, channels, streams, coupledStreams, mapping, &opusError);
    if (d.Decoder == nullptr || opusError != OPUS_OK)
    {
        error = std::string("libopus rejected the audio track: ") + opus_strerror(opusError);
        Close();
        return false;
    }

    d.SourceChannels = channels;
    d.CodecDelay = d.Demuxer.GetCodecDelay();
    d.Duration = duration > 0.0 ? duration : d.Demuxer.GetDuration();
    d.Ring.assign(kRingFrames * kChannels, 0.0f);

    d.System = AcquireSystem();
    if (d.System == nullptr)
    {
        error = "No audio device is available.";
        Close();
        return false;
    }

    d.Worker = std::thread([&d] { d.WorkerMain(); });

    // A user stream whose length is the video's: FMOD's playback position is then media
    // time, which is what Sync() compares against and what a seek sets directly.
    FMOD_CREATESOUNDEXINFO info{};
    info.cbsize = sizeof(info);
    info.numchannels = kChannels;
    info.defaultfrequency = kSampleRate;
    info.format = FMOD_SOUND_FORMAT_PCMFLOAT;
    info.decodebuffersize = 2048;
    info.length = static_cast<unsigned int>(
        (std::max)(d.Duration, 1.0) * kSampleRate * kChannels * sizeof(float));
    info.pcmreadcallback = &PcmReadCallback;
    info.pcmsetposcallback = &PcmSetPosCallback;
    info.userdata = &d;

    if (d.System->createStream(nullptr, FMOD_OPENUSER | FMOD_LOOP_NORMAL | FMOD_2D, &info, &d.Sound) != FMOD_OK)
    {
        error = "FMOD could not create the video audio stream.";
        Close();
        return false;
    }

    d.Sound->setUserData(&d);
    d.Sound->setLoopCount(-1);
    if (d.System->playSound(d.Sound, nullptr, true, &d.Channel) != FMOD_OK || d.Channel == nullptr)
    {
        error = "FMOD could not start the video audio stream.";
        Close();
        return false;
    }

    d.Channel->setVolume(d.Volume);
    d.Paused = true;
    return true;
}

void VideoAudio::Close()
{
    Impl& d = *mImpl;

    if (d.Channel != nullptr)
    {
        d.Channel->stop();
        d.Channel = nullptr;
    }

    if (d.Sound != nullptr)
    {
        // Released before the worker stops, so no read callback can run afterwards.
        d.Sound->release();
        d.Sound = nullptr;
    }

    if (d.Worker.joinable())
    {
        {
            std::lock_guard<std::mutex> lock(d.Mutex);
            d.Quit = true;
        }

        d.WorkerWake.notify_all();
        d.DataReady.notify_all();
        d.Worker.join();
    }

    d.Quit = false;

    if (d.Decoder != nullptr)
    {
        opus_multistream_decoder_destroy(d.Decoder);
        d.Decoder = nullptr;
    }

    if (d.System != nullptr)
    {
        d.System = nullptr;
        ReleaseSystem();
    }

    d.Demuxer.Close();
    d.Ring.clear();
    d.RingRead = d.RingWrite = d.RingCount = 0;
    d.SilenceDebt = 0;
    d.EndOfStream = false;
    d.SeekPending = false;
    d.Paused = true;
}

void VideoAudio::SetPaused(bool paused)
{
    Impl& d = *mImpl;
    if (d.Channel != nullptr && paused != d.Paused)
    {
        d.Channel->setPaused(paused);
        d.Paused = paused;
    }
}

void VideoAudio::Seek(double seconds)
{
    Impl& d = *mImpl;
    if (d.Channel == nullptr)
    {
        return;
    }

    // Repositioning the stream calls back into RequestSeek, so the decoder follows.
    d.Channel->setPosition(static_cast<unsigned int>((std::max)(seconds, 0.0) * kSampleRate), FMOD_TIMEUNIT_PCM);
}

void VideoAudio::SetVolume(float volume)
{
    Impl& d = *mImpl;
    d.Volume = std::clamp(volume, 0.0f, 1.0f);
    if (d.Channel != nullptr)
    {
        d.Channel->setVolume(d.Volume);
    }
}

float VideoAudio::GetVolume() const
{
    return mImpl->Volume;
}

VideoAudio::Status VideoAudio::GetStatus() const
{
    Impl& d = *mImpl;
    Status status;
    status.DecodedFrames = d.DecodedFrames.load();
    status.UnderrunFrames = d.UnderrunFrames.load();
    status.Resyncs = d.Resyncs.load();

    unsigned int position = 0;
    if (d.Channel != nullptr && d.Channel->getPosition(&position, FMOD_TIMEUNIT_PCM) == FMOD_OK)
    {
        status.Time = static_cast<double>(position) / kSampleRate;
    }

    return status;
}

void VideoAudio::Sync(double mediaTime, double rate, bool playing)
{
    Impl& d = *mImpl;
    if (d.Channel == nullptr)
    {
        return;
    }

    SetPaused(!playing);
    if (!playing)
    {
        return;
    }

    unsigned int position = 0;
    if (d.Channel->getPosition(&position, FMOD_TIMEUNIT_PCM) != FMOD_OK)
    {
        return;
    }

    const double audioTime = static_cast<double>(position) / kSampleRate;
    const double error = mediaTime - audioTime;

    if (std::abs(error) > kResyncThreshold)
    {
        // A loop point, a scrub or a stall: jump rather than crawl back.
        d.Resyncs.fetch_add(1);
        Seek(mediaTime);
        d.Channel->setFrequency(static_cast<float>(kSampleRate * rate));
        return;
    }

    // Otherwise pull it back over the next second or so, which is far too small a pitch
    // change to hear.
    const double nudge = std::clamp(error, -kMaxNudge, kMaxNudge);
    d.Channel->setFrequency(static_cast<float>(kSampleRate * rate * (1.0 + nudge)));
}
