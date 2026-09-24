#pragma once

#include <cstdint>
#include <memory>
#include <string>

// The soundtrack of a video: the Opus track of the same .webm, decoded with libopus and
// played through FMOD.
//
// The video's playhead is the master clock, because that is what the game drives. This
// class keeps the sound in step with it: the stream's own timeline is media time, so a
// seek is a position change, small drift is corrected by nudging the playback rate by a
// fraction of a percent, and anything larger (a loop point, a stall, a scrub) is a
// reposition.
//
// Owned and called only by VideoPlayer, from the thread that owns the player; decoding and
// FMOD's reads happen on their own threads behind the mutex.
class VideoAudio
{
public:
    VideoAudio();
    ~VideoAudio();

    VideoAudio(const VideoAudio&) = delete;
    VideoAudio& operator=(const VideoAudio&) = delete;

    // False when the file has no Opus track, when the decoder rejects it, or when no audio
    // device is available; the video then plays silently. `error` says which.
    bool Open(const std::wstring& path, double duration, std::string& error);
    void Close();

    void SetPaused(bool paused);
    void Seek(double seconds);
    // Linear gain, 0..1.
    void SetVolume(float volume);
    float GetVolume() const;

    // Called once per video frame with the video's playhead.
    void Sync(double mediaTime, double rate, bool playing);

    struct Status
    {
        // Where the sound is, in media time; compare with the video's playhead.
        double Time = 0.0;
        // Frames the Opus decoder has produced since the last seek.
        long long DecodedFrames = 0;
        // Frames of silence played because the decoder could not keep up.
        long long UnderrunFrames = 0;
        // Times the sound had to be repositioned instead of nudged.
        int Resyncs = 0;
    };
    Status GetStatus() const;

    // Public only so FMOD's C callbacks, which get it back as the stream's user data, can
    // reach it. Nothing outside this file should touch it.
    struct Impl;

private:
    std::unique_ptr<Impl> mImpl;
};
