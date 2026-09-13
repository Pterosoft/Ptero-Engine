#pragma once

#include <DirectXMath.h>

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

    // Cost of building and reconciling the editor UI for one frame, measured across
    // QtUi::NewFrame..EndFrame. Retained widgets are far more expensive to drive per
    // frame than immediate-mode drawing, so this is the number to watch.
    void SetUiFrameMilliseconds(float uiFrameMilliseconds)
    {
        mUiFrameMilliseconds = uiFrameMilliseconds;
    }

    void SetFrameBreakdown(float renderMilliseconds, float outsideMilliseconds, float tailMilliseconds)
    {
        mRenderMilliseconds = renderMilliseconds;
        mOutsideMilliseconds = outsideMilliseconds;
        mTailMilliseconds = tailMilliseconds;
    }

    void SetLoopTimings(float pumpMilliseconds, float audioMilliseconds, unsigned messageCount, unsigned paintMessageCount)
    {
        mPumpMilliseconds = pumpMilliseconds;
        mAudioMilliseconds = audioMilliseconds;
        mMessageCount = messageCount;
        mPaintMessageCount = paintMessageCount;
    }

    void SetQtEventCounts(unsigned updates, unsigned layouts, unsigned paints, const char* topPainter, unsigned topPainterCount, const char* eventTypes)
    {
        mQtUpdates = updates;
        mQtLayouts = layouts;
        mQtPaints = paints;
        mTopPainter = topPainter ? topPainter : "-";
        mTopPainterCount = topPainterCount;
        mEventTypes = eventTypes ? eventTypes : "-";
    }

    void SetUiPhases(float sceneMilliseconds, float uiBuildMilliseconds, float qtEventMilliseconds)
    {
        mSceneMilliseconds = sceneMilliseconds;
        mUiBuildMilliseconds = uiBuildMilliseconds;
        mQtEventMilliseconds = qtEventMilliseconds;
    }

    void SetViewportInfo(unsigned width, unsigned height, unsigned swapChainResizeCount)
    {
        mViewportWidth = width;
        mViewportHeight = height;
        mSwapChainResizeCount = swapChainResizeCount;
    }

    const char* GetText() const
    {
        return mStatisticsText.c_str();
    }

    void SetRuntimeStatistics(
        float cpuUsagePercent,
        float gpuUsagePercent,
        float ramUsagePercent,
        const DirectX::XMFLOAT3& cameraPosition,
        const DirectX::XMFLOAT3& cameraRotation)
    {
        mCpuUsagePercent = cpuUsagePercent;
        mGpuUsagePercent = gpuUsagePercent;
        mRamUsagePercent = ramUsagePercent;
        mCameraPosition = cameraPosition;
        mCameraRotation = cameraRotation;
        RefreshText();
    }

private:
    void RefreshText()
    {
        // This text drives a label in the translucent viewport overlay, and every change
        // repaints that layered window. Rebuilding it once per frame meant a full DWM
        // surface push per frame; four times a second reads the same to a human.
        const auto now = std::chrono::steady_clock::now();
        if (mHasRefreshedText
            && std::chrono::duration<float>(now - mLastTextRefreshTime).count() < 0.25f)
        {
            return;
        }
        mLastTextRefreshTime = now;
        mHasRefreshedText = true;

        char statisticsBuffer[768] = {};
        sprintf_s(
            statisticsBuffer,
            "Ptero-Engine\nRenderer: DirectX 12\nFPS: %.1f\nLatency: %.2f ms\nEditor UI: %.2f ms\nCPU Usage: %.1f%%\nGPU Usage: %.1f%%\nRAM Usage: %.1f%%\nCamera Position: (%.2f, %.2f, %.2f)\nCamera Rotation: (%.1f, %.1f, %.1f)\nViewport: %ux%u  resizes: %u\nRender: %.2f ms  Outside: %.2f ms  Tail: %.2f ms\nScene: %.2f  UiBuild: %.2f  QtEvents: %.2f ms\nPump: %.2f  Audio: %.2f ms  msgs: %u (paint %u)\nQt: upd %u  lay %u  paint %u   top: %s x%u\nEvents: %s",
            mFramesPerSecond,
            mFrameLatencyMilliseconds,
            mUiFrameMilliseconds,
            mCpuUsagePercent,
            mGpuUsagePercent,
            mRamUsagePercent,
            mCameraPosition.x,
            mCameraPosition.y,
            mCameraPosition.z,
            DirectX::XMConvertToDegrees(mCameraRotation.x),
            DirectX::XMConvertToDegrees(mCameraRotation.y),
            DirectX::XMConvertToDegrees(mCameraRotation.z),
            mViewportWidth,
            mViewportHeight,
            mSwapChainResizeCount,
            mRenderMilliseconds,
            mOutsideMilliseconds,
            mTailMilliseconds,
            mSceneMilliseconds,
            mUiBuildMilliseconds,
            mQtEventMilliseconds,
            mPumpMilliseconds,
            mAudioMilliseconds,
            mMessageCount,
            mPaintMessageCount,
            mQtUpdates,
            mQtLayouts,
            mQtPaints,
            mTopPainter.c_str(),
            mTopPainterCount,
            mEventTypes.c_str());
        mStatisticsText = statisticsBuffer;
    }

    std::chrono::steady_clock::time_point mPreviousFrameTime{};
    std::chrono::steady_clock::time_point mLastFpsUpdateTime{};
    std::chrono::steady_clock::time_point mLastTextRefreshTime{};
    bool mHasRefreshedText = false;
    std::string mStatisticsText;
    float mFramesPerSecond = 0.0f;
    float mFrameLatencyMilliseconds = 0.0f;
    float mUiFrameMilliseconds = 0.0f;
    unsigned mViewportWidth = 0;
    unsigned mViewportHeight = 0;
    unsigned mSwapChainResizeCount = 0;
    float mRenderMilliseconds = 0.0f;
    float mOutsideMilliseconds = 0.0f;
    float mTailMilliseconds = 0.0f;
    float mSceneMilliseconds = 0.0f;
    float mUiBuildMilliseconds = 0.0f;
    float mQtEventMilliseconds = 0.0f;
    float mPumpMilliseconds = 0.0f;
    float mAudioMilliseconds = 0.0f;
    unsigned mMessageCount = 0;
    unsigned mPaintMessageCount = 0;
    unsigned mQtUpdates = 0;
    unsigned mQtLayouts = 0;
    unsigned mQtPaints = 0;
    std::string mTopPainter = "-";
    unsigned mTopPainterCount = 0;
    std::string mEventTypes = "-";
    float mCpuUsagePercent = 0.0f;
    float mGpuUsagePercent = 0.0f;
    float mRamUsagePercent = 0.0f;
    DirectX::XMFLOAT3 mCameraPosition = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
    DirectX::XMFLOAT3 mCameraRotation = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
    float mAccumulatedFrameTime = 0.0f;
    unsigned int mAccumulatedFrameCount = 0;
    bool mHasPreviousFrame = false;
};
