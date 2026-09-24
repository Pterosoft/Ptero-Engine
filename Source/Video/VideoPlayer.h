#pragma once

#include "VideoAPI.h"

#include <cstdint>
#include <memory>
#include <string>

// Plays one .webm file.
//
// Decoding runs on a worker thread that stays a few frames ahead of the playhead; the
// owner advances the playhead by calling Update() once per frame with the elapsed time.
// Driving the clock from outside rather than from a wall clock is deliberate: a paused
// game, a stalled frame or a debugger break then pauses the video with it instead of
// letting it race ahead and drop everything in between.
//
// All methods are meant to be called from one thread (the owner's). The frame returned by
// GetFrame() stays valid until the next Update(), Open() or Close().
class VIDEO_API VideoPlayer
{
public:
    enum class State
    {
        Closed,
        Stopped,
        Playing,
        Paused,
        // Reached the end without looping. Play() restarts from the beginning.
        Finished,
        Error
    };

    struct Frame
    {
        const std::uint8_t* Pixels = nullptr;  // BGRA8, top row first
        int Width = 0;
        int Height = 0;
        int Pitch = 0;                          // bytes per row
        double Time = 0.0;                      // presentation time in seconds
        // Increments whenever a different picture becomes current, so a consumer can tell
        // whether it has to upload again.
        std::uint64_t Serial = 0;
    };

    VideoPlayer();
    ~VideoPlayer();

    VideoPlayer(const VideoPlayer&) = delete;
    VideoPlayer& operator=(const VideoPlayer&) = delete;

    // Opens the file and decodes its first frame, leaving the player Stopped at time 0.
    bool Open(const std::wstring& path);
    void Close();

    void Play();
    void Pause();
    // Pauses and rewinds to the first frame.
    void Stop();
    void Seek(double seconds);

    void SetLooping(bool looping);
    bool IsLooping() const;
    void SetPlaybackRate(double rate);
    double GetPlaybackRate() const;

    // Soundtrack. A video whose Opus track is missing or unplayable plays silently, which
    // HasAudio() reports; the volume is remembered either way.
    bool HasAudio() const;
    void SetVolume(float volume);
    float GetVolume() const;

    // Where the soundtrack actually is and how it is coping, for diagnostics: AudioTime
    // should track GetTime() closely, and underruns should stay at zero.
    struct AudioStatus
    {
        bool Present = false;
        double Time = 0.0;
        long long DecodedFrames = 0;
        long long UnderrunFrames = 0;
        int Resyncs = 0;
    };
    AudioStatus GetAudioStatus() const;

    // Advances the playhead and picks up newly decoded frames.
    void Update(double elapsedSeconds);

    // True once there is a picture to show. After Stop() the first frame stays current.
    bool GetFrame(Frame& frame) const;

    State GetState() const;
    bool IsOpen() const;
    bool IsPlaying() const { return GetState() == State::Playing; }
    double GetTime() const;
    double GetDuration() const;
    int GetWidth() const;
    int GetHeight() const;
    double GetFrameRate() const;
    // "VP8" or "VP9".
    const char* GetCodecName() const;
    const std::wstring& GetPath() const;
    const std::string& GetLastError() const;

    // Latches once per end of stream reached without looping; reading it clears it, and
    // so does anything that moves the playhead (Seek, Stop, Play after finishing, Open).
    bool ConsumeFinished();

private:
    struct Impl;
#pragma warning(push)
#pragma warning(disable : 4251)
    std::unique_ptr<Impl> mImpl;
#pragma warning(pop)
};
