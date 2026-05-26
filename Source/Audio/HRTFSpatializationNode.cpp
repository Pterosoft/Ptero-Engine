#define NOMINMAX
#include "framework.h"
#include "HRTFSpatializationNode.h"

#include <cassert>
#include <cmath>
#include <numbers>
#include <algorithm>

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace
{
    // Returns the smallest power-of-two >= n.
    int NextPowerOfTwo(int n)
    {
        if (n <= 1) return 1;
        int p = 1;
        while (p < n) p <<= 1;
        return p;
    }
}

// ---------------------------------------------------------------------------
// Initialize
// ---------------------------------------------------------------------------
bool HRTFSpatializationNode::Initialize(int hrirLength, int blockSize, int crossfadeSamples)
{
    if (hrirLength <= 0 || blockSize <= 0 || crossfadeSamples < 0)
        return false;

    m_hrirLength       = hrirLength;
    m_blockSize        = blockSize;
    m_crossfadeSamples = crossfadeSamples;
    // Overlap-Add: N >= L + P - 1  (next power-of-two)
    m_fftSize          = NextPowerOfTwo(blockSize + hrirLength - 1);

    // Active HRIR spectra
    m_hrirSpecL.assign(m_fftSize, Complex{});
    m_hrirSpecR.assign(m_fftSize, Complex{});

    // Pending HRIR spectra
    m_pendingSpecL.assign(m_fftSize, Complex{});
    m_pendingSpecR.assign(m_fftSize, Complex{});

    // Overlap-add save buffers (P-1 samples)
    m_overlapL.assign(m_hrirLength - 1, 0.0f);
    m_overlapR.assign(m_hrirLength - 1, 0.0f);
    m_overlapPendingL.assign(m_hrirLength - 1, 0.0f);
    m_overlapPendingR.assign(m_hrirLength - 1, 0.0f);

    // Scratch
    m_inputSpectrum.assign(m_fftSize, Complex{});
    m_convBufL.assign(m_fftSize, Complex{});
    m_convBufR.assign(m_fftSize, Complex{});
    m_convBufPendingL.assign(m_fftSize, Complex{});
    m_convBufPendingR.assign(m_fftSize, Complex{});
    m_blockScratch.assign(m_fftSize, 0.0f);

    m_pendingHRIR.store(false, std::memory_order_relaxed);
    m_crossfadeRemaining = 0;
    m_initialized        = true;
    return true;
}

// ---------------------------------------------------------------------------
// SetHRIR  (thread-safe)
// ---------------------------------------------------------------------------
void HRTFSpatializationNode::SetHRIR(std::span<const float> leftHrir,
                                      std::span<const float> rightHrir)
{
    if (!m_initialized) return;

    const int safeLen = std::min(static_cast<int>(leftHrir.size()),  m_hrirLength);
    const int safeR   = std::min(static_cast<int>(rightHrir.size()), m_hrirLength);

    // Build the new spectra into the pending slots.
    // We do this on the calling thread so no allocation happens on the audio thread.
    std::vector<Complex> tmpL(m_fftSize, Complex{});
    std::vector<Complex> tmpR(m_fftSize, Complex{});

    for (int i = 0; i < safeLen; ++i) tmpL[i] = Complex{ leftHrir[i], 0.0f };
    for (int i = 0; i < safeR;   ++i) tmpR[i] = Complex{ rightHrir[i], 0.0f };

    FFT(tmpL.data(), m_fftSize);
    FFT(tmpR.data(), m_fftSize);

    // Swap into the pending buffers.  We rely on the fact that std::vector
    // swap is O(1) and does not allocate.
    m_pendingSpecL.swap(tmpL);
    m_pendingSpecR.swap(tmpR);

    // Signal the audio thread that new spectra are ready.
    m_pendingHRIR.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// ProcessAudio
// ---------------------------------------------------------------------------
void HRTFSpatializationNode::ProcessAudio(std::span<const float> monoIn,
                                           std::span<float>       stereoOut)
{
    assert(m_initialized);
    assert(static_cast<int>(monoIn.size())   == m_blockSize);
    assert(static_cast<int>(stereoOut.size()) == m_blockSize * 2);

    // ---- Bypass path -------------------------------------------------------
    if (!m_enabled.load(std::memory_order_relaxed))
    {
        for (int i = 0; i < m_blockSize; ++i)
        {
            stereoOut[i * 2 + 0] = monoIn[i];
            stereoOut[i * 2 + 1] = monoIn[i];
        }
        return;
    }

    // ---- Check for a pending HRIR swap ------------------------------------
    if (m_pendingHRIR.load(std::memory_order_acquire))
    {
        m_pendingHRIR.store(false, std::memory_order_relaxed);

        // Copy the current overlap buffers so crossfade can blend smoothly.
        m_overlapPendingL = m_overlapL;
        m_overlapPendingR = m_overlapR;

        // Swap active <-> pending spectra (O(1), no allocation).
        m_hrirSpecL.swap(m_pendingSpecL);
        m_hrirSpecR.swap(m_pendingSpecR);

        // Begin crossfade from the (now stale) pending spectra over N samples.
        m_crossfadeRemaining = m_crossfadeSamples;
    }

    // ---- Build zero-padded input and compute its DFT ----------------------
    for (int i = 0; i < m_blockSize; ++i)
        m_inputSpectrum[i] = Complex{ monoIn[i], 0.0f };
    for (int i = m_blockSize; i < m_fftSize; ++i)
        m_inputSpectrum[i] = Complex{};

    FFT(m_inputSpectrum.data(), m_fftSize);

    // ---- Convolve with active HRIR -----------------------------------------
    ConvolveAndOverlapAdd(m_inputSpectrum, m_hrirSpecL, m_overlapL,
                          reinterpret_cast<float*>(m_convBufL.data()));
    ConvolveAndOverlapAdd(m_inputSpectrum, m_hrirSpecR, m_overlapR,
                          reinterpret_cast<float*>(m_convBufR.data()));

    // ---- Crossfade blend (if active) --------------------------------------
    if (m_crossfadeRemaining > 0)
    {
        // Convolve with the old (pending) HRIR for blend source.
        ConvolveAndOverlapAdd(m_inputSpectrum, m_pendingSpecL, m_overlapPendingL,
                              reinterpret_cast<float*>(m_convBufPendingL.data()));
        ConvolveAndOverlapAdd(m_inputSpectrum, m_pendingSpecR, m_overlapPendingR,
                              reinterpret_cast<float*>(m_convBufPendingR.data()));

        // Blend sample-by-sample (linear fade).
        const int fadeSamples = std::min(m_crossfadeRemaining, m_blockSize);
        for (int i = 0; i < fadeSamples; ++i)
        {
            const float alpha = 1.0f - static_cast<float>(m_crossfadeRemaining - i)
                                       / static_cast<float>(m_crossfadeSamples);
            // alpha = 0 → full old,  alpha = 1 → full new
            const float newL = reinterpret_cast<const float*>(m_convBufL.data())[i];
            const float newR = reinterpret_cast<const float*>(m_convBufR.data())[i];
            const float oldL = reinterpret_cast<const float*>(m_convBufPendingL.data())[i];
            const float oldR = reinterpret_cast<const float*>(m_convBufPendingR.data())[i];

            stereoOut[i * 2 + 0] = oldL + alpha * (newL - oldL);
            stereoOut[i * 2 + 1] = oldR + alpha * (newR - oldR);
        }
        // Samples beyond the crossfade region use pure new HRIR.
        for (int i = fadeSamples; i < m_blockSize; ++i)
        {
            stereoOut[i * 2 + 0] = reinterpret_cast<const float*>(m_convBufL.data())[i];
            stereoOut[i * 2 + 1] = reinterpret_cast<const float*>(m_convBufR.data())[i];
        }
        m_crossfadeRemaining -= fadeSamples;
    }
    else
    {
        // No crossfade — copy directly.
        const float* pL = reinterpret_cast<const float*>(m_convBufL.data());
        const float* pR = reinterpret_cast<const float*>(m_convBufR.data());
        for (int i = 0; i < m_blockSize; ++i)
        {
            stereoOut[i * 2 + 0] = pL[i];
            stereoOut[i * 2 + 1] = pR[i];
        }
    }
}

// ---------------------------------------------------------------------------
// ConvolveAndOverlapAdd
//
// Multiplies inputSpectrum * hrirSpectrum in the frequency domain, IFFTs the
// result, adds the saved overlap tail, writes m_blockSize samples to outChannel
// (stored as plain floats at outChannel[0..blockSize-1]), and saves the tail.
//
// NOTE: outChannel is a float* pointing into one of the m_convBuf* vectors
//       (which are Complex vectors) — we only use the first m_blockSize real
//       values, so there is no aliasing issue.
// ---------------------------------------------------------------------------
void HRTFSpatializationNode::ConvolveAndOverlapAdd(
    const std::vector<Complex>& inputSpectrum,
    const std::vector<Complex>& hrirSpectrum,
    std::vector<float>&         overlapSave,
    float*                      outChannel)
{
    // Point-wise multiply into m_blockScratch (reused as a Complex buffer via
    // m_convBufL / R which we write via outChannel after the IFFT).
    // We do the multiply into a temporary local that IS one of m_convBufL/R/etc.
    // because the caller passes outChannel = reinterpret_cast<float*>(m_convBufX).

    // Step 1: freq-domain multiply into a local scratch (m_blockScratch used as
    //         a real buffer here; we need a Complex scratch — use the same
    //         m_inputSpectrum? No — it's still needed.  Use m_convBufL/R directly.)
    //   Since outChannel IS pointing to one of m_convBufL/R, cast it back.
    Complex* out = reinterpret_cast<Complex*>(outChannel);

    for (int k = 0; k < m_fftSize; ++k)
        out[k] = inputSpectrum[k] * hrirSpectrum[k];

    IFFT(out, m_fftSize);

    // Step 2: Overlap-Add — add the saved tail and emit m_blockSize samples.
    const int overlapLen = m_hrirLength - 1;
    for (int i = 0; i < overlapLen; ++i)
        outChannel[i] = out[i].real() + overlapSave[i];

    for (int i = overlapLen; i < m_blockSize; ++i)
        outChannel[i] = out[i].real();

    // Step 3: Save the new tail (samples [blockSize .. blockSize + overlapLen - 1]).
    for (int i = 0; i < overlapLen; ++i)
        overlapSave[i] = out[m_blockSize + i].real();
}

// ---------------------------------------------------------------------------
// Radix-2 Cooley-Tukey FFT (in-place, DIT)
// ---------------------------------------------------------------------------
void HRTFSpatializationNode::FFT(Complex* x, int n)
{
    // Bit-reversal permutation.
    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }

    // Butterfly stages.
    for (int len = 2; len <= n; len <<= 1)
    {
        const float angle = -2.0f * static_cast<float>(std::numbers::pi) / static_cast<float>(len);
        const Complex wLen{ std::cos(angle), std::sin(angle) };

        for (int i = 0; i < n; i += len)
        {
            Complex w{ 1.0f, 0.0f };
            for (int j = 0; j < len / 2; ++j)
            {
                const Complex u = x[i + j];
                const Complex v = x[i + j + len / 2] * w;
                x[i + j]            = u + v;
                x[i + j + len / 2]  = u - v;
                w *= wLen;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// IFFT — conjugate twiddle factors + scale by 1/n.
// ---------------------------------------------------------------------------
void HRTFSpatializationNode::IFFT(Complex* x, int n)
{
    // Conjugate input.
    for (int i = 0; i < n; ++i)
        x[i] = std::conj(x[i]);

    FFT(x, n);

    // Conjugate and scale.
    const float invN = 1.0f / static_cast<float>(n);
    for (int i = 0; i < n; ++i)
        x[i] = std::conj(x[i]) * invN;
}
