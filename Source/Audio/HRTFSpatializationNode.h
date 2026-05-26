#pragma once

#include <vector>
#include <complex>
#include <cstddef>
#include <atomic>
#include <span>

#ifdef AUDIO_EXPORTS
#define AUDIO_API __declspec(dllexport)
#else
#define AUDIO_API __declspec(dllimport)
#endif

// ---------------------------------------------------------------------------
// HRTFSpatializationNode
//
// Real-time HRTF spatialization via frequency-domain Overlap-Add convolution.
// Zero allocations occur in ProcessAudio() -- all buffers are pre-allocated
// during Initialize().
//
// Usage:
//   1. Call Initialize(hrirLength, blockSize) once.
//   2. Call SetHRIR(leftHrir, rightHrir) to load an initial HRIR pair.
//      Subsequent calls crossfade smoothly over the next block.
//   3. Call ProcessAudio(monoIn, stereoOut) each audio callback.
// ---------------------------------------------------------------------------
#pragma warning(push)
#pragma warning(disable: 4251)
class AUDIO_API HRTFSpatializationNode
{
public:
    HRTFSpatializationNode() = default;
    ~HRTFSpatializationNode() = default;

    HRTFSpatializationNode(const HRTFSpatializationNode&)            = delete;
    HRTFSpatializationNode& operator=(const HRTFSpatializationNode&) = delete;

    // Allocate all working buffers.
    bool Initialize(int hrirLength, int blockSize, int crossfadeSamples = 256);

    // Load a new HRIR pair. Thread-safe: may be called from any thread.
    void SetHRIR(std::span<const float> leftHrir, std::span<const float> rightHrir);

    // Process one block of mono audio into interleaved stereo output.
    //   monoIn   : exactly blockSize dry mono samples.
    //   stereoOut: exactly blockSize * 2 interleaved L/R samples.
    void ProcessAudio(std::span<const float> monoIn, std::span<float> stereoOut);

    void SetEnabled(bool enabled) { m_enabled.store(enabled, std::memory_order_relaxed); }
    bool IsEnabled()        const { return m_enabled.load(std::memory_order_relaxed); }
    bool IsInitialized()    const { return m_initialized; }

private:
    using Complex = std::complex<float>;

    static void FFT(Complex* x, int n);
    static void IFFT(Complex* x, int n);

    void ConvolveAndOverlapAdd(
        const std::vector<Complex>& inputSpectrum,
        const std::vector<Complex>& hrirSpectrum,
        std::vector<float>&         overlapSave,
        float*                      outChannel);

    // --- dimensions ---
    int m_hrirLength = 0;
    int m_blockSize  = 0;
    int m_fftSize    = 0;

    // --- active HRIR spectra ---
    std::vector<Complex> m_hrirSpecL;
    std::vector<Complex> m_hrirSpecR;

    // --- pending HRIR spectra (written by SetHRIR, consumed by ProcessAudio) ---
    std::vector<Complex> m_pendingSpecL;
    std::vector<Complex> m_pendingSpecR;
    std::atomic<bool>    m_pendingHRIR{ false };

    // --- overlap-add tails (P-1 samples) ---
    std::vector<float>   m_overlapL;
    std::vector<float>   m_overlapR;

    // --- overlap-add tails for crossfade source ---
    std::vector<float>   m_overlapPendingL;
    std::vector<float>   m_overlapPendingR;

    // --- per-block scratch (pre-allocated, never reallocated in ProcessAudio) ---
    std::vector<Complex> m_inputSpectrum;
    std::vector<Complex> m_convBufL;
    std::vector<Complex> m_convBufR;
    std::vector<Complex> m_convBufPendingL;
    std::vector<Complex> m_convBufPendingR;
    std::vector<float>   m_blockScratch;

    // --- crossfade state ---
    int  m_crossfadeSamples   = 256;
    int  m_crossfadeRemaining = 0;

    std::atomic<bool> m_enabled{ true };
    bool              m_initialized = false;
};
#pragma warning(pop)
