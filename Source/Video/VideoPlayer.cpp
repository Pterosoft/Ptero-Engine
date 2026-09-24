#include "VideoPlayer.h"

#include "VideoAudio.h"
#include "VpxDecoder.h"
#include "WebmDemuxer.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
    // Frames decoded ahead of the playhead. Enough to ride out a slow frame on either
    // side; more only costs memory (a 1080p frame is 8 MB).
    constexpr size_t kQueueDepth = 4;
    constexpr double kTimeEpsilon = 1e-4;

    struct DecodedFrame
    {
        std::vector<std::uint8_t> Pixels;
        int Width = 0;
        int Height = 0;
        double Time = 0.0;
        // Which pass through the file this frame belongs to. The worker wraps around on
        // its own when looping, so the next pass is already buffered when the playhead
        // reaches the end and the loop point does not stall.
        int Loop = 0;
    };
}

#pragma warning(push)
#pragma warning(disable : 4251)
struct VideoPlayer::Impl
{
    // ---- owned by the calling thread -------------------------------------------------
    std::wstring Path;
    State PlayerState = State::Closed;
    double Clock = 0.0;
    int ClockLoop = 0;
    double Rate = 1.0;
    DecodedFrame Current;
    bool HasCurrent = false;
    bool AwaitingSeekFrame = false;
    std::uint64_t Serial = 0;
    bool FinishedLatch = false;
    std::string LastError;

    // Stream facts, written by Open before the worker starts and immutable after.
    int Width = 0;
    int Height = 0;
    double Duration = 0.0;
    double FrameRate = 0.0;
    WebmDemuxer::Codec Codec = WebmDemuxer::Codec::Unknown;

    // ---- shared with the worker, guarded by Mutex ------------------------------------
    std::mutex Mutex;
    std::condition_variable WorkerWake;
    std::condition_variable FrameReady;
    std::deque<DecodedFrame> Queue;
    std::vector<std::vector<std::uint8_t>> FreeBuffers;
    bool Quit = false;
    bool SeekPending = false;
    double SeekTarget = 0.0;
    bool EndOfStream = false;
    bool Looping = false;
    std::string WorkerError;

    // ---- owned by the worker ---------------------------------------------------------
    std::thread Worker;
    WebmDemuxer Demuxer;
    VpxDecoder Decoder;

    // ---- soundtrack, driven by the playhead below --------------------------------------
    std::unique_ptr<VideoAudio> Audio;
    float Volume = 1.0f;

    double LoopLength() const
    {
        if (Duration > 0.0)
        {
            return Duration;
        }

        return FrameRate > 0.0 ? 1.0 / FrameRate : 1.0 / 30.0;
    }

    // Transport changes reach the sound straight away rather than waiting for the next
    // Update, so a pause is silent immediately.
    void ApplyAudioPause()
    {
        if (Audio)
        {
            Audio->SetPaused(PlayerState != State::Playing);
        }
    }

    void Recycle(DecodedFrame& frame)
    {
        if (!frame.Pixels.empty())
        {
            FreeBuffers.push_back(std::move(frame.Pixels));
        }

        frame.Pixels.clear();
    }

    void ClearQueueLocked()
    {
        for (DecodedFrame& frame : Queue)
        {
            Recycle(frame);
        }

        Queue.clear();
    }

    void RequestSeekLocked(double seconds)
    {
        ClearQueueLocked();
        SeekPending = true;
        SeekTarget = seconds;
        EndOfStream = false;
        WorkerWake.notify_all();
    }

    void WorkerMain()
    {
        int loop = 0;
        // While seeking, frames before the target are decoded (they are the reference
        // chain) but not shown. The last of them is the picture that is on screen *at*
        // the target, so it is held back rather than thrown away.
        bool seeking = false;
        double seekTarget = 0.0;
        DecodedFrame candidate;
        bool hasCandidate = false;

        WebmDemuxer::Packet packet;
        std::string error;

        for (;;)
        {
            {
                std::unique_lock<std::mutex> lock(Mutex);
                WorkerWake.wait(lock, [&] {
                    return Quit || SeekPending || (!EndOfStream && Queue.size() < kQueueDepth) ||
                           (EndOfStream && Looping && Queue.size() < kQueueDepth);
                });

                if (Quit)
                {
                    return;
                }

                if (SeekPending)
                {
                    SeekPending = false;
                    seeking = true;
                    seekTarget = SeekTarget;
                    loop = 0;
                    if (hasCandidate)
                    {
                        Recycle(candidate);
                        hasCandidate = false;
                    }

                    lock.unlock();
                    Demuxer.SeekToKeyFrame(seekTarget);
                    continue;
                }

                if (EndOfStream)
                {
                    // Only reachable when looping was switched on after the end.
                    EndOfStream = false;
                    ++loop;
                    lock.unlock();
                    Demuxer.SeekToKeyFrame(0.0);
                    continue;
                }
            }

            if (!Demuxer.ReadPacket(packet))
            {
                std::lock_guard<std::mutex> lock(Mutex);
                if (SeekPending)
                {
                    continue;
                }

                if (seeking && hasCandidate)
                {
                    // The target lies past the last frame: show the last frame.
                    candidate.Loop = loop;
                    Queue.push_back(std::move(candidate));
                    hasCandidate = false;
                    FrameReady.notify_all();
                }

                seeking = false;
                if (Looping)
                {
                    ++loop;
                    Demuxer.SeekToKeyFrame(0.0);
                }
                else
                {
                    EndOfStream = true;
                    FrameReady.notify_all();
                }

                continue;
            }

            DecodedFrame frame;
            {
                std::lock_guard<std::mutex> lock(Mutex);
                if (!FreeBuffers.empty())
                {
                    frame.Pixels = std::move(FreeBuffers.back());
                    FreeBuffers.pop_back();
                }
            }

            error.clear();
            const bool shown = Decoder.Decode(packet.Data, frame.Pixels, frame.Width, frame.Height, error);

            std::lock_guard<std::mutex> lock(Mutex);
            if (!error.empty())
            {
                // A damaged frame breaks the reference chain only until the next key
                // frame, so keep going rather than ending playback.
                WorkerError = error;
            }

            if (!shown || SeekPending)
            {
                Recycle(frame);
                continue;
            }

            frame.Time = packet.Time;
            frame.Loop = loop;

            if (seeking)
            {
                if (frame.Time <= seekTarget + kTimeEpsilon)
                {
                    if (hasCandidate)
                    {
                        Recycle(candidate);
                    }

                    candidate = std::move(frame);
                    hasCandidate = true;
                    continue;
                }

                seeking = false;
                if (hasCandidate)
                {
                    candidate.Loop = loop;
                    Queue.push_back(std::move(candidate));
                    hasCandidate = false;
                }
            }

            Queue.push_back(std::move(frame));
            FrameReady.notify_all();
        }
    }

    void StopWorker()
    {
        if (Worker.joinable())
        {
            {
                std::lock_guard<std::mutex> lock(Mutex);
                Quit = true;
            }

            WorkerWake.notify_all();
            Worker.join();
        }

        Quit = false;
    }

    // Moves every frame that is due into Current.
    void LatchFrames()
    {
        std::lock_guard<std::mutex> lock(Mutex);

        if (!WorkerError.empty())
        {
            LastError = WorkerError;
            WorkerError.clear();
        }

        for (;;)
        {
            if (Queue.empty())
            {
                break;
            }

            DecodedFrame& front = Queue.front();

            if (front.Loop < ClockLoop)
            {
                // Left over from a pass the playhead has already finished.
                Recycle(front);
                Queue.pop_front();
                continue;
            }

            if (front.Loop > ClockLoop)
            {
                // The next pass is buffered; wrap the playhead once it reaches the end.
                if (Clock + kTimeEpsilon < LoopLength() || !Looping)
                {
                    break;
                }

                Clock = (std::max)(Clock - LoopLength(), 0.0);
                ClockLoop = front.Loop;
                continue;
            }

            if (!AwaitingSeekFrame && HasCurrent && front.Time > Clock + kTimeEpsilon)
            {
                break;
            }

            AwaitingSeekFrame = false;
            Recycle(Current);
            Current = std::move(front);
            Queue.pop_front();
            HasCurrent = true;
            ++Serial;
        }

        WorkerWake.notify_all();
    }
};
#pragma warning(pop)

VideoPlayer::VideoPlayer()
    : mImpl(std::make_unique<Impl>())
{
}

VideoPlayer::~VideoPlayer()
{
    Close();
}

bool VideoPlayer::Open(const std::wstring& path)
{
    Close();

    Impl& d = *mImpl;
    std::string error;
    if (!d.Demuxer.Open(path, error) || !d.Decoder.Open(d.Demuxer.GetCodec(), error))
    {
        d.Demuxer.Close();
        d.Decoder.Close();
        d.LastError = error;
        d.PlayerState = State::Error;
        return false;
    }

    d.Path = path;
    d.Width = d.Demuxer.GetWidth();
    d.Height = d.Demuxer.GetHeight();
    d.Duration = d.Demuxer.GetDuration();
    d.FrameRate = d.Demuxer.GetFrameRate();
    d.Codec = d.Demuxer.GetCodec();
    d.Clock = 0.0;
    d.ClockLoop = 0;
    d.EndOfStream = false;
    d.SeekPending = false;
    d.AwaitingSeekFrame = true;
    d.FinishedLatch = false;
    d.LastError.clear();
    d.PlayerState = State::Stopped;

    d.Worker = std::thread([this] { mImpl->WorkerMain(); });

    // A video without a usable soundtrack plays silently rather than failing to open.
    auto audio = std::make_unique<VideoAudio>();
    std::string audioError;
    if (audio->Open(path, d.Duration, audioError))
    {
        audio->SetVolume(d.Volume);
        d.Audio = std::move(audio);
    }
    else
    {
        d.LastError = audioError;
    }

    // Wait for the first picture so a caller that opens and immediately draws, such as
    // the preview window, has something to show. Bounded: a broken file must not hang
    // the editor.
    {
        std::unique_lock<std::mutex> lock(d.Mutex);
        d.FrameReady.wait_for(lock, std::chrono::seconds(3), [&] { return !d.Queue.empty() || d.EndOfStream; });
    }

    d.LatchFrames();
    if (!d.HasCurrent)
    {
        if (d.LastError.empty())
        {
            d.LastError = "The video has no decodable frames.";
        }

        Close();
        d.PlayerState = State::Error;
        return false;
    }

    return true;
}

void VideoPlayer::Close()
{
    Impl& d = *mImpl;
    d.Audio.reset();
    d.StopWorker();
    d.Demuxer.Close();
    d.Decoder.Close();

    d.ClearQueueLocked();
    d.Recycle(d.Current);
    d.FreeBuffers.clear();
    d.Current = DecodedFrame{};
    d.HasCurrent = false;
    d.Path.clear();
    d.Width = d.Height = 0;
    d.Duration = d.FrameRate = 0.0;
    d.Codec = WebmDemuxer::Codec::Unknown;
    d.Clock = 0.0;
    d.ClockLoop = 0;
    d.FinishedLatch = false;
    d.PlayerState = State::Closed;
}

void VideoPlayer::Play()
{
    Impl& d = *mImpl;
    switch (d.PlayerState)
    {
    case State::Finished:
        Seek(0.0);
        [[fallthrough]];
    case State::Stopped:
    case State::Paused:
        d.PlayerState = State::Playing;
        break;
    default:
        break;
    }

    d.ApplyAudioPause();
}

void VideoPlayer::Pause()
{
    if (mImpl->PlayerState == State::Playing)
    {
        mImpl->PlayerState = State::Paused;
        mImpl->ApplyAudioPause();
    }
}

void VideoPlayer::Stop()
{
    Impl& d = *mImpl;
    if (d.PlayerState == State::Closed || d.PlayerState == State::Error)
    {
        return;
    }

    Seek(0.0);
    d.PlayerState = State::Stopped;
    d.ApplyAudioPause();
}

void VideoPlayer::Seek(double seconds)
{
    Impl& d = *mImpl;
    if (d.PlayerState == State::Closed || d.PlayerState == State::Error)
    {
        return;
    }

    const double target = std::clamp(seconds, 0.0, (std::max)(d.Duration, 0.0));
    {
        std::lock_guard<std::mutex> lock(d.Mutex);
        d.RequestSeekLocked(target);
    }

    d.Clock = target;
    d.ClockLoop = 0;
    d.AwaitingSeekFrame = true;
    // An end reached before the seek no longer describes where playback is.
    d.FinishedLatch = false;

    if (d.Audio)
    {
        d.Audio->Seek(target);
    }
    if (d.PlayerState == State::Finished)
    {
        d.PlayerState = State::Paused;
    }
}

void VideoPlayer::SetLooping(bool looping)
{
    Impl& d = *mImpl;
    std::lock_guard<std::mutex> lock(d.Mutex);
    d.Looping = looping;
    d.WorkerWake.notify_all();
}

bool VideoPlayer::IsLooping() const
{
    std::lock_guard<std::mutex> lock(mImpl->Mutex);
    return mImpl->Looping;
}

void VideoPlayer::SetPlaybackRate(double rate)
{
    mImpl->Rate = std::clamp(rate, 0.1, 4.0);
}

double VideoPlayer::GetPlaybackRate() const
{
    return mImpl->Rate;
}

bool VideoPlayer::HasAudio() const
{
    return mImpl->Audio != nullptr;
}

void VideoPlayer::SetVolume(float volume)
{
    Impl& d = *mImpl;
    d.Volume = std::clamp(volume, 0.0f, 1.0f);
    if (d.Audio)
    {
        d.Audio->SetVolume(d.Volume);
    }
}

float VideoPlayer::GetVolume() const
{
    return mImpl->Volume;
}

VideoPlayer::AudioStatus VideoPlayer::GetAudioStatus() const
{
    AudioStatus status;
    if (mImpl->Audio)
    {
        const VideoAudio::Status audio = mImpl->Audio->GetStatus();
        status.Present = true;
        status.Time = audio.Time;
        status.DecodedFrames = audio.DecodedFrames;
        status.UnderrunFrames = audio.UnderrunFrames;
        status.Resyncs = audio.Resyncs;
    }

    return status;
}

void VideoPlayer::Update(double elapsedSeconds)
{
    Impl& d = *mImpl;
    if (d.PlayerState == State::Closed || d.PlayerState == State::Error)
    {
        return;
    }

    // Whichever way this call ends, the soundtrack is told where the playhead got to.
    struct SyncAudioOnExit
    {
        Impl& Player;
        ~SyncAudioOnExit()
        {
            if (Player.Audio)
            {
                Player.Audio->Sync(Player.Clock, Player.Rate, Player.PlayerState == State::Playing);
            }
        }
    } syncAudioOnExit{ d };

    if (d.PlayerState == State::Playing)
    {
        d.Clock += (std::max)(elapsedSeconds, 0.0) * d.Rate;
    }

    d.LatchFrames();

    if (d.PlayerState != State::Playing)
    {
        return;
    }

    bool looping = false;
    bool drained = false;
    {
        std::lock_guard<std::mutex> lock(d.Mutex);
        looping = d.Looping;
        // Frames queued for a later pass also mean this pass is over: the worker wraps on
        // its own, and looping may have been switched off after it did.
        drained = !d.SeekPending &&
                  (d.Queue.empty() ? d.EndOfStream : d.Queue.front().Loop > d.ClockLoop);
    }

    if (d.Clock + kTimeEpsilon < d.LoopLength() || !drained)
    {
        return;
    }

    if (looping)
    {
        // Normally LatchFrames wraps the playhead when the next pass's frames arrive.
        // Reaching here means looping was switched on after the worker had stopped at
        // the end; it is wrapping now, so restart the playhead in step with it.
        std::lock_guard<std::mutex> lock(d.Mutex);
        if (d.Queue.empty())
        {
            d.Clock = 0.0;
            d.ClockLoop += 1;
        }

        return;
    }

    d.Clock = d.LoopLength();
    d.PlayerState = State::Finished;
    d.FinishedLatch = true;
}

bool VideoPlayer::GetFrame(Frame& frame) const
{
    const Impl& d = *mImpl;
    if (!d.HasCurrent)
    {
        return false;
    }

    frame.Pixels = d.Current.Pixels.data();
    frame.Width = d.Current.Width;
    frame.Height = d.Current.Height;
    frame.Pitch = d.Current.Width * 4;
    frame.Time = d.Current.Time;
    frame.Serial = d.Serial;
    return true;
}

VideoPlayer::State VideoPlayer::GetState() const
{
    return mImpl->PlayerState;
}

bool VideoPlayer::IsOpen() const
{
    return mImpl->PlayerState != State::Closed && mImpl->PlayerState != State::Error;
}

double VideoPlayer::GetTime() const
{
    return std::clamp(mImpl->Clock, 0.0, (std::max)(mImpl->Duration, 0.0));
}

double VideoPlayer::GetDuration() const
{
    return mImpl->Duration;
}

int VideoPlayer::GetWidth() const
{
    return mImpl->Width;
}

int VideoPlayer::GetHeight() const
{
    return mImpl->Height;
}

double VideoPlayer::GetFrameRate() const
{
    return mImpl->FrameRate;
}

const char* VideoPlayer::GetCodecName() const
{
    switch (mImpl->Codec)
    {
    case WebmDemuxer::Codec::VP8: return "VP8";
    case WebmDemuxer::Codec::VP9: return "VP9";
    default: return "";
    }
}

const std::wstring& VideoPlayer::GetPath() const
{
    return mImpl->Path;
}

const std::string& VideoPlayer::GetLastError() const
{
    return mImpl->LastError;
}

bool VideoPlayer::ConsumeFinished()
{
    const bool finished = mImpl->FinishedLatch;
    mImpl->FinishedLatch = false;
    return finished;
}
