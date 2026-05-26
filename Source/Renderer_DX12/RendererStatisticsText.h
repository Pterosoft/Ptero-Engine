#pragma once

#include <chrono>
#include <cstdio>
#include <string>

class RendererStatisticsText final
{
public:
    void MarkFrame()
    {
        const auto currentTime = std::chrono::steady_clock::now();
        if (!mHasPreviousFrame)
        {
            mPreviousFrameTime = currentTime;
            mLastFpsUpdateTime = currentTime;
            mHasPreviousFrame = true;
            RefreshText();
            return;
        }

        const float frameTimeSeconds = std::chrono::duration<float>(currentTime - mPreviousFrameTime).count();
        mPreviousFrameTime = currentTime;
        mFrameLatencyMilliseconds = frameTimeSeconds * 1000.0f;
        mAccumulatedFrameTime += frameTimeSeconds;
        ++mAccumulatedFrameCount;

        // Average FPS over a short window so the overlay stays readable while latency still reflects the latest frame.
        constexpr float fpsUpdateIntervalSeconds = 0.25f;
        if (std::chrono::duration<float>(currentTime - mLastFpsUpdateTime).count() >= fpsUpdateIntervalSeconds)
        {
            mFramesPerSecond = mAccumulatedFrameTime > 0.0f
                ? static_cast<float>(mAccumulatedFrameCount) / mAccumulatedFrameTime
                : 0.0f;
            mAccumulatedFrameTime = 0.0f;
            mAccumulatedFrameCount = 0;
            mLastFpsUpdateTime = currentTime;
        }

        RefreshText();
    }

    const char* GetText() const
    {
        return mStatisticsText.c_str();
    }

private:
    void RefreshText()
    {
        char statisticsBuffer[256] = {};
        sprintf_s(
            statisticsBuffer,
            "Ptero-Engine\nRenderer: DirectX 12\nFPS: %.1f\nLatency: %.2f ms",
            mFramesPerSecond,
            mFrameLatencyMilliseconds);
        mStatisticsText = statisticsBuffer;
    }

    std::chrono::steady_clock::time_point mPreviousFrameTime{};
    std::chrono::steady_clock::time_point mLastFpsUpdateTime{};
    std::string mStatisticsText;
    float mFramesPerSecond = 0.0f;
    float mFrameLatencyMilliseconds = 0.0f;
    float mAccumulatedFrameTime = 0.0f;
    unsigned int mAccumulatedFrameCount = 0;
    bool mHasPreviousFrame = false;
};
