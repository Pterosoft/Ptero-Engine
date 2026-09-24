#include "VideoLayer.h"

#include "VideoPlayer.h"

#include "System/DataFiles.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>

namespace
{
    std::filesystem::path FindDataDirectory()
    {
        // The repository's Data folder, or a packaged game's virtual one (DataFiles.h).
        const std::filesystem::path dataDirectory = DataFiles::FindDataDirectory();
        return dataDirectory.empty() ? std::filesystem::path(L"Data") : dataDirectory;
    }

    std::wstring Widen(const std::string& text)
    {
        if (text.empty())
        {
            return {};
        }

        const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring result(static_cast<size_t>(size), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), size);
        return result;
    }

    bool EqualsIgnoreCase(const std::wstring& a, const std::wstring& b)
    {
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); });
    }

    std::filesystem::path ResolveVideo(const std::string& fileName)
    {
        namespace fs = std::filesystem;
        std::error_code error;

        fs::path requested(Widen(fileName));
        if (requested.empty())
        {
            return {};
        }

        if (requested.extension().empty())
        {
            requested += L".webm";
        }

        if (requested.is_absolute())
        {
            return DataFiles::IsFile(requested) ? requested : fs::path();
        }

        const fs::path data = FindDataDirectory();
        const fs::path direct = (data / requested).lexically_normal();
        if (DataFiles::IsFile(direct))
        {
            return direct;
        }

        const std::wstring wanted = requested.filename().wstring();
        for (const fs::path& file : DataFiles::ListFiles(data, true))
        {
            if (EqualsIgnoreCase(file.filename().wstring(), wanted))
            {
                return file;
            }
        }

        return {};
    }

    VideoTexture::Fit ParseFit(const std::string& fit)
    {
        if (_stricmp(fit.c_str(), "Fill") == 0)
        {
            return VideoTexture::Fit::Fill;
        }

        if (_stricmp(fit.c_str(), "Stretch") == 0)
        {
            return VideoTexture::Fit::Stretch;
        }

        return VideoTexture::Fit::Letterbox;
    }
}

struct VideoLayer::Impl
{
    VideoPlayer Player;
    VideoTexture Texture;
    VideoTexture::Fit Fit = VideoTexture::Fit::Letterbox;
    float Volume = 1.0f;
    bool Visible = false;
    bool Finished = false;
    std::string LastError;
};

VideoLayer::VideoLayer()
    : mImpl(std::make_unique<Impl>())
{
}

VideoLayer::~VideoLayer() = default;

bool VideoLayer::InitializeGpu(ID3D12Device* device,
                               const D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandles,
                               const D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandles,
                               DXGI_FORMAT renderTargetFormat)
{
    return mImpl->Texture.Initialize(device, cpuHandles, gpuHandles, renderTargetFormat);
}

void VideoLayer::ShutdownGpu()
{
    mImpl->Texture.Shutdown();
}

bool VideoLayer::IsGpuInitialized() const
{
    return mImpl->Texture.IsInitialized();
}

bool VideoLayer::Play(const std::string& fileName, bool loop, const std::string& fit)
{
    Impl& d = *mImpl;
    d.Finished = false;

    const std::filesystem::path path = ResolveVideo(fileName);
    if (path.empty())
    {
        d.LastError = "Video '" + fileName + "' was not found under Data.";
        Stop();
        return false;
    }

    // The picture of the previous video must not flash up before the new one's first
    // frame has been uploaded.
    d.Texture.Clear();
    if (!d.Player.Open(path.wstring()))
    {
        d.LastError = d.Player.GetLastError();
        Stop();
        return false;
    }

    d.LastError.clear();
    d.Fit = ParseFit(fit);
    d.Player.SetVolume(d.Volume);
    d.Player.SetLooping(loop);
    d.Player.Play();
    d.Visible = true;
    return true;
}

void VideoLayer::Pause()
{
    mImpl->Player.Pause();
}

void VideoLayer::Resume()
{
    if (mImpl->Visible && mImpl->Player.GetState() == VideoPlayer::State::Paused)
    {
        mImpl->Player.Play();
    }
}

void VideoLayer::Stop()
{
    mImpl->Player.Close();
    mImpl->Texture.Clear();
    mImpl->Visible = false;
}

void VideoLayer::Seek(double seconds)
{
    mImpl->Player.Seek(seconds);
}

void VideoLayer::SetLooping(bool loop)
{
    mImpl->Player.SetLooping(loop);
}

void VideoLayer::SetVolume(float volume)
{
    mImpl->Volume = std::clamp(volume, 0.0f, 1.0f);
    mImpl->Player.SetVolume(mImpl->Volume);
}

float VideoLayer::GetVolume() const
{
    return mImpl->Volume;
}

bool VideoLayer::HasAudio() const
{
    return mImpl->Player.HasAudio();
}

bool VideoLayer::IsVisible() const
{
    return mImpl->Visible;
}

bool VideoLayer::IsPlaying() const
{
    return mImpl->Visible && mImpl->Player.IsPlaying();
}

double VideoLayer::GetTime() const
{
    return mImpl->Player.GetTime();
}

double VideoLayer::GetDuration() const
{
    return mImpl->Player.GetDuration();
}

bool VideoLayer::ConsumeFinished()
{
    const bool finished = mImpl->Finished;
    mImpl->Finished = false;
    return finished;
}

const std::string& VideoLayer::GetLastError() const
{
    return mImpl->LastError;
}

void VideoLayer::Update(double elapsedSeconds)
{
    Impl& d = *mImpl;
    if (!d.Visible)
    {
        return;
    }

    d.Player.Update(elapsedSeconds);
    if (d.Player.ConsumeFinished())
    {
        // A cut-scene that has ended gets out of the way of the game.
        d.Finished = true;
        Stop();
    }
}

void VideoLayer::Record(ID3D12GraphicsCommandList* commandList, unsigned targetWidth, unsigned targetHeight)
{
    Impl& d = *mImpl;
    if (!d.Texture.IsInitialized())
    {
        return;
    }

    d.Texture.BeginFrame();
    if (!d.Visible)
    {
        return;
    }

    VideoPlayer::Frame frame;
    if (d.Player.GetFrame(frame))
    {
        d.Texture.Upload(commandList, frame);
    }

    d.Texture.Draw(commandList, targetWidth, targetHeight, d.Fit);
}
