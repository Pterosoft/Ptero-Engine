#pragma once

// Voice chat with the other player: Steam records and compresses the microphone
// (ISteamUser voice API), SteamSession carries the packets, and the decompressed 16-bit
// mono PCM is played through an XAudio2 source voice of our own. Playing it here rather
// than through Audio.dll keeps voice independent of the game's FMOD banks.

#include <cstdint>
#include <deque>
#include <vector>

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

class VoiceChat
{
public:
    VoiceChat() = default;
    ~VoiceChat();

    // Recording runs only while the microphone is on and someone is there to hear it.
    void SetMicrophone(bool on) { mMicrophone = on; }
    bool GetMicrophone() const { return mMicrophone; }
    void SetPeerMuted(bool muted) { mPeerMuted = muted; }
    bool IsPeerMuted() const { return mPeerMuted; }
    bool IsTalking(bool local) const { return (local ? mLocalTalk : mPeerTalk) > 0.0f; }

    // Compressed packets recorded since the last call (empty when there are none).
    // `transmit` is false while there is no peer, which stops recording altogether.
    const std::vector<unsigned char>& Capture(float deltaSeconds, bool transmit);
    // A compressed packet from the other player.
    void Play(const void* data, std::uint32_t size);
    // Leaving a table: stop recording, flush what is queued, forget the talk indicators.
    void Reset();

private:
    bool EnsureOutput(std::uint32_t sampleRate);
    void ReleaseFinishedBuffers();

    bool mMicrophone = false, mPeerMuted = false, mRecording = false;
    float mLocalTalk = 0.0f, mPeerTalk = 0.0f;
    std::vector<unsigned char> mCaptured;
    std::vector<std::int16_t> mDecoded;

    bool mComInitialized = false, mOutputFailed = false;
    std::uint32_t mSampleRate = 0;
    IXAudio2* mAudio = nullptr;
    IXAudio2MasteringVoice* mMaster = nullptr;
    IXAudio2SourceVoice* mSource = nullptr;
    // XAudio2 reads submitted buffers in place, so each stays alive until it has played.
    std::deque<std::vector<std::int16_t>> mQueued;
};
