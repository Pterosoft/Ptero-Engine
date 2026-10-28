#pragma once

// How the bloom glow is produced. The two methods share Intensity / Threshold / Knee, so
// switching between them keeps the overall look roughly where it was.
enum class BloomMethod : int
{
    // Downsample / upsample mip chain (Dual-Kawase style). Cheap, soft, shapeless.
    MipChain = 0,

    // The thresholded image is convolved with a full-frame point spread function in the
    // frequency domain (FFT -> multiply -> inverse FFT). Costs more, but the kernel can be
    // anything - here a physically motivated glare PSF with a sharp core, a power-law halo
    // and the diffraction spikes of the lens aperture - and every bright pixel gets the
    // same shape regardless of how large the kernel is.
    FftConvolution = 1,
};

// Bloom settings exposed to both the scene renderer and the Graphics Settings UI.
struct BloomSettings
{
    bool  Enabled         = true;

    BloomMethod Method    = BloomMethod::MipChain;

    // Overall bloom intensity multiplied onto the accumulated scatter result.
    float Intensity       = 0.03f;

    // Threshold (in linear scene luminance) below which pixels do not contribute.
    float Threshold       = 0.9f;

    // Soft knee width around the threshold. 0 = hard cut, 1 = very gradual.
    float Knee            = 0.35f;

    // ---- Mip chain ----------------------------------------------------------
    // Radius scale: multiplies the scatter radius at each mip level.
    float Radius          = 1.0f;

    // Number of downsample mip levels. Range [2..8].
    int   MipLevels       = 6;

    // ---- FFT convolution ------------------------------------------------------
    // Side of the square FFT grid: 256, 512 or 1024. The image occupies half of it (the
    // other half is padding that keeps the circular convolution from wrapping the far
    // edge of the frame onto the near one), so 512 convolves a 256-pixel-wide copy.
    int   FftResolution   = 512;

    // How far the kernel reaches, as a fraction of the frame width. 1 = a bright pixel in
    // one corner can light up the opposite edge.
    float FftKernelSize   = 0.6f;

    // Exponent of the power-law halo around the core. Higher = tighter glow.
    float FftHaloFalloff  = 2.2f;

    // Share of the kernel's energy in the halo (the rest stays in the sharp core).
    float FftHaloStrength = 0.6f;

    // Strength of the aperture diffraction spikes baked into the kernel.
    float FftStreakStrength = 0.35f;

    // Aperture that produces the spikes: blade count and rotation in degrees. An even
    // blade count gives that many spikes; an odd one gives twice as many.
    int   FftApertureBlades   = 6;
    float FftApertureRotation = 15.0f;

    // Wavelength spread of the halo and spikes: red reaches further than blue, as it
    // does in real glare. 0 = neutral grey kernel.
    float FftChromaticSpread = 0.5f;
};
