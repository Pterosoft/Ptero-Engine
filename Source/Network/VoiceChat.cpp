#include "pch.h"
#include "VoiceChat.h"

#pragma warning(push)
#pragma warning(disable : 4996)
#include <steam/steam_api.h>
#pragma warning(pop)

#include <xaudio2.h>

namespace
{
    // Seconds the talking indicator stays lit after the last packet.
    constexpr float kTalkHold = 0.3f;
    // More than this much voice waiting to play means we have fallen behind (a stalled frame,
    // a burst after a network hiccup): drop the backlog rather than lag further.
    constexpr size_t kMaxQueuedBuffers = 12;
    constexpr uint32 kCaptureBytes = 8 * 1024;
    constexpr uint32 kDecodeSamples = 22050; // ~0.5 s at 44.1 kHz, more than one packet holds
}

VoiceChat::~VoiceChat()
{
    // Never reached in practice (the session lives for the process), but correct if it is.
    if (mSource) mSource->DestroyVoice();
    if (mMaster) mMaster->DestroyVoice();
    if (mAudio) mAudio->Release();
    if (mComInitialized) CoUninitialize();
}

const std::vector<unsigned char>& VoiceChat::Capture(float deltaSeconds, bool transmit)
{
    mCaptured.clear();
    mLocalTalk = (std::max)(0.0f, mLocalTalk - deltaSeconds);
    mPeerTalk = (std::max)(0.0f, mPeerTalk - deltaSeconds);
    ReleaseFinishedBuffers();

    ISteamUser* user = SteamUser();
    const bool record = mMicrophone && transmit;
    if (record != mRecording)
    {
        mRecording = record;
        if (record)
            user->StartVoiceRecording();
        else
            user->StopVoiceRecording();
    }
    if (!mRecording)
        return mCaptured;

    uint32 available = 0;
    if (user->GetAvailableVoice(&available) != k_EVoiceResultOK || available == 0)
        return mCaptured;
    mCaptured.resize(kCaptureBytes);
    uint32 written = 0;
    if (user->GetVoice(true, mCaptured.data(), kCaptureBytes, &written) != k_EVoiceResultOK)
        written = 0;
    mCaptured.resize(written);
    if (written > 0)
        mLocalTalk = kTalkHold;
    return mCaptured;
}

void VoiceChat::Play(const void* data, std::uint32_t size)
{
    mPeerTalk = kTalkHold;
    if (mPeerMuted)
        return;
    ISteamUser* user = SteamUser();
    const uint32 rate = user->GetVoiceOptimalSampleRate();
    if (!EnsureOutput(rate))
        return;

    mDecoded.resize(kDecodeSamples);
    uint32 bytes = 0;
    const EVoiceResult result = user->DecompressVoice(data, size, mDecoded.data(),
        uint32(mDecoded.size() * sizeof(std::int16_t)), &bytes, rate);
    if (result != k_EVoiceResultOK || bytes == 0)
        return;

    ReleaseFinishedBuffers();
    if (mQueued.size() >= kMaxQueuedBuffers)
    {
        mSource->FlushSourceBuffers();
        ReleaseFinishedBuffers();
    }
    mQueued.emplace_back(mDecoded.begin(), mDecoded.begin() + bytes / sizeof(std::int16_t));
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = bytes;
    buffer.pAudioData = reinterpret_cast<const BYTE*>(mQueued.back().data());
    if (FAILED(mSource->SubmitSourceBuffer(&buffer)))
        mQueued.pop_back();
}

bool VoiceChat::EnsureOutput(std::uint32_t sampleRate)
{
    if (mSource && sampleRate == mSampleRate)
        return true;
    if (mOutputFailed)
        return false;

    if (!mAudio)
    {
        // The host thread may already be in a COM apartment of its own; that is fine.
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        mComInitialized = SUCCEEDED(com);
        if (FAILED(XAudio2Create(&mAudio, 0, XAUDIO2_DEFAULT_PROCESSOR))
            || FAILED(mAudio->CreateMasteringVoice(&mMaster)))
        {
            mOutputFailed = true;
            return false;
        }
    }
    if (mSource)
    {
        mSource->DestroyVoice();
        mSource = nullptr;
        mQueued.clear();
    }

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = sampleRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 2;
    format.nAvgBytesPerSec = sampleRate * 2;
    if (FAILED(mAudio->CreateSourceVoice(&mSource, &format)) || FAILED(mSource->Start()))
    {
        mOutputFailed = true;
        return false;
    }
    mSampleRate = sampleRate;
    return true;
}

void VoiceChat::ReleaseFinishedBuffers()
{
    if (!mSource)
        return;
    XAUDIO2_VOICE_STATE state{};
    mSource->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);
    while (mQueued.size() > state.BuffersQueued)
        mQueued.pop_front();
}

void VoiceChat::Reset()
{
    if (mRecording)
    {
        SteamUser()->StopVoiceRecording();
        mRecording = false;
    }
    if (mSource)
    {
        mSource->FlushSourceBuffers();
        ReleaseFinishedBuffers();
    }
    mLocalTalk = mPeerTalk = 0.0f;
}
