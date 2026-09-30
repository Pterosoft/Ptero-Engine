#pragma once

// LensDiffraction - Fraunhofer diffraction of a lens aperture, computed on the CPU.
//
// The far-field pattern of an aperture is the squared magnitude of its Fourier transform.
// For a bladed iris that is the familiar starburst: one spike perpendicular to each blade
// edge (so an even blade count gives that many spikes, an odd one twice as many). The
// pattern scales with wavelength, so summing it over the visible spectrum gives the
// coloured fringes along the spikes.
//
// Used by LensFlareRenderer (the starburst sprite on each light) and by FftBloomRenderer
// (the spikes baked into its convolution kernel). Both build it only when a setting
// changes, never per frame.

#include <vector>

namespace LensDiffraction
{
    struct ApertureParams
    {
        int   Blades = 6;
        float RotationRadians = 0.0f;
        float Roundness = 0.0f;     // 0 = straight blades, 1 = circle
    };

    // Linear-sRGB colour of a spectral line, roughly CIE 1931. Not normalised.
    void WavelengthToRgb(float lambdaNm, float& r, float& g, float& b);

    // In-place radix-2 complex FFT over n = 2^k values (interleaved re/im).
    void Fft1D(float* data, int n, bool inverse);

    // Square RGB image (size x size, 3 floats per texel, row-major) of the aperture's
    // spectral diffraction pattern, centred, with its peak normalised to 1.
    // chromaticSpread scales how far the spectrum fans out (0 = one wavelength).
    std::vector<float> BuildStarburst(int size, const ApertureParams& aperture, float chromaticSpread);
}
