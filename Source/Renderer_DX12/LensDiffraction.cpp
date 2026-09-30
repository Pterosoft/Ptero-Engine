#include "pch.h"
#include "LensDiffraction.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace LensDiffraction
{
    namespace
    {
        constexpr float kPi = 3.14159265358979f;

        // Signed distance-like value of the iris polygon: 1 on the boundary. Same shape
        // function as LensFlareOptics::ApertureDistance and the ghost shader.
        float ApertureDistance(float x, float y, const ApertureParams& aperture)
        {
            const float radius = std::sqrt(x * x + y * y);
            const int blades = (std::max)(aperture.Blades, 3);
            const float sector = 2.0f * kPi / static_cast<float>(blades);
            float angle = std::atan2(y, x) - aperture.RotationRadians;
            angle = angle - sector * std::floor(angle / sector) - 0.5f * sector;
            const float polygon = radius * std::cos(angle) / std::cos(0.5f * sector);
            return polygon + (radius - polygon) * std::clamp(aperture.Roundness, 0.0f, 1.0f);
        }

        float Gaussian(float x, float mu, float sigmaLow, float sigmaHigh)
        {
            const float t = (x - mu) / (x < mu ? sigmaLow : sigmaHigh);
            return std::exp(-0.5f * t * t);
        }
    }

    void WavelengthToRgb(float lambdaNm, float& r, float& g, float& b)
    {
        // Wyman, Sloan & Shirley 2013 piecewise-Gaussian fit of the CIE 1931 observer.
        const float x = 1.056f * Gaussian(lambdaNm, 599.8f, 37.9f, 31.0f)
                      + 0.362f * Gaussian(lambdaNm, 442.0f, 16.0f, 26.7f)
                      - 0.065f * Gaussian(lambdaNm, 501.1f, 20.4f, 26.2f);
        const float y = 0.821f * Gaussian(lambdaNm, 568.8f, 46.9f, 40.5f)
                      + 0.286f * Gaussian(lambdaNm, 530.9f, 16.3f, 31.1f);
        const float z = 1.217f * Gaussian(lambdaNm, 437.0f, 11.8f, 36.0f)
                      + 0.681f * Gaussian(lambdaNm, 459.0f, 26.0f, 13.8f);

        // XYZ -> linear sRGB, negative lobes clipped.
        r = (std::max)(0.0f,  3.2406f * x - 1.5372f * y - 0.4986f * z);
        g = (std::max)(0.0f, -0.9689f * x + 1.8758f * y + 0.0415f * z);
        b = (std::max)(0.0f,  0.0557f * x - 0.2040f * y + 1.0570f * z);
    }

    void Fft1D(float* data, int n, bool inverse)
    {
        // Bit-reversal permutation.
        for (int i = 1, j = 0; i < n; ++i)
        {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1)
                j ^= bit;
            j ^= bit;
            if (i < j)
            {
                std::swap(data[2 * i], data[2 * j]);
                std::swap(data[2 * i + 1], data[2 * j + 1]);
            }
        }

        for (int length = 2; length <= n; length <<= 1)
        {
            const float angle = (inverse ? 2.0f : -2.0f) * kPi / static_cast<float>(length);
            const float wRe = std::cos(angle);
            const float wIm = std::sin(angle);
            for (int start = 0; start < n; start += length)
            {
                float curRe = 1.0f, curIm = 0.0f;
                for (int k = 0; k < length / 2; ++k)
                {
                    float* a = data + 2 * (start + k);
                    float* b = data + 2 * (start + k + length / 2);
                    const float tRe = b[0] * curRe - b[1] * curIm;
                    const float tIm = b[0] * curIm + b[1] * curRe;
                    b[0] = a[0] - tRe; b[1] = a[1] - tIm;
                    a[0] += tRe;       a[1] += tIm;
                    const float nextRe = curRe * wRe - curIm * wIm;
                    curIm = curRe * wIm + curIm * wRe;
                    curRe = nextRe;
                }
            }
        }
    }

    std::vector<float> BuildStarburst(int size, const ApertureParams& aperture, float chromaticSpread)
    {
        const int n = size;
        std::vector<float> field(static_cast<std::size_t>(n) * n * 2, 0.0f);

        // The iris covers a small part of the grid: the pattern's scale is the inverse of
        // the aperture's, so a small aperture gives a pattern that fills the image.
        const float apertureRadius = static_cast<float>(n) / 14.0f;
        constexpr int kSuper = 4;
        for (int y = 0; y < n; ++y)
        {
            for (int x = 0; x < n; ++x)
            {
                const float cx = static_cast<float>(x - n / 2);
                const float cy = static_cast<float>(y - n / 2);
                if (cx * cx + cy * cy > (apertureRadius + 2.0f) * (apertureRadius + 2.0f))
                    continue;

                int covered = 0;
                for (int sy = 0; sy < kSuper; ++sy)
                    for (int sx = 0; sx < kSuper; ++sx)
                    {
                        const float px = (cx + (sx + 0.5f) / kSuper - 0.5f) / apertureRadius;
                        const float py = (cy + (sy + 0.5f) / kSuper - 0.5f) / apertureRadius;
                        covered += ApertureDistance(px, py, aperture) <= 1.0f ? 1 : 0;
                    }

                // Stored at the origin-centred position so no shift is needed before the
                // transform; the output shift below centres the pattern.
                const int ox = (x - n / 2 + n) % n;
                const int oy = (y - n / 2 + n) % n;
                field[(static_cast<std::size_t>(oy) * n + ox) * 2] = static_cast<float>(covered) / (kSuper * kSuper);
            }
        }

        std::vector<float> column(static_cast<std::size_t>(n) * 2);
        for (int y = 0; y < n; ++y)
            Fft1D(field.data() + static_cast<std::size_t>(y) * n * 2, n, false);
        for (int x = 0; x < n; ++x)
        {
            for (int y = 0; y < n; ++y)
            {
                column[2 * y] = field[(static_cast<std::size_t>(y) * n + x) * 2];
                column[2 * y + 1] = field[(static_cast<std::size_t>(y) * n + x) * 2 + 1];
            }
            Fft1D(column.data(), n, false);
            for (int y = 0; y < n; ++y)
            {
                field[(static_cast<std::size_t>(y) * n + x) * 2] = column[2 * y];
                field[(static_cast<std::size_t>(y) * n + x) * 2 + 1] = column[2 * y + 1];
            }
        }

        // Power spectrum, shifted so the zero frequency sits in the centre.
        std::vector<float> power(static_cast<std::size_t>(n) * n);
        for (int y = 0; y < n; ++y)
        {
            for (int x = 0; x < n; ++x)
            {
                const std::size_t src = (static_cast<std::size_t>((y + n / 2) % n) * n + (x + n / 2) % n) * 2;
                power[static_cast<std::size_t>(y) * n + x] = field[src] * field[src] + field[src + 1] * field[src + 1];
            }
        }

        auto samplePower = [&](float fx, float fy) -> float
        {
            if (fx < 0.0f || fy < 0.0f || fx > n - 1.001f || fy > n - 1.001f)
                return 0.0f;
            const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
            const float tx = fx - x0, ty = fy - y0;
            const float* row0 = power.data() + static_cast<std::size_t>(y0) * n;
            const float* row1 = row0 + n;
            return (row0[x0] * (1 - tx) + row0[x0 + 1] * tx) * (1 - ty)
                 + (row1[x0] * (1 - tx) + row1[x0 + 1] * tx) * ty;
        };

        // Sum over the spectrum. The pattern's scale is proportional to wavelength.
        constexpr int kLambdas = 24;
        const float spread = std::clamp(chromaticSpread, 0.0f, 2.0f);
        std::vector<float> rgb(static_cast<std::size_t>(n) * n * 3, 0.0f);
        float weightR = 0.0f, weightG = 0.0f, weightB = 0.0f;
        for (int l = 0; l < kLambdas; ++l)
        {
            const float lambda = 400.0f + 300.0f * (l + 0.5f) / kLambdas;
            float r, g, b;
            WavelengthToRgb(lambda, r, g, b);
            weightR += r; weightG += g; weightB += b;

            const float scale = 1.0f + spread * (lambda / 550.0f - 1.0f);
            const float invScale = 1.0f / (std::max)(scale, 0.2f);
            // Energy is conserved as the pattern stretches.
            const float energy = invScale * invScale;
            for (int y = 0; y < n; ++y)
            {
                for (int x = 0; x < n; ++x)
                {
                    const float fx = n * 0.5f + (x - n * 0.5f) * invScale;
                    const float fy = n * 0.5f + (y - n * 0.5f) * invScale;
                    const float p = samplePower(fx, fy) * energy;
                    float* out = rgb.data() + (static_cast<std::size_t>(y) * n + x) * 3;
                    out[0] += p * r; out[1] += p * g; out[2] += p * b;
                }
            }
        }

        // White-balance the spectrum sum so a white source stays white, then put the
        // peak at 1.
        float peak = 0.0f;
        for (std::size_t i = 0; i < rgb.size(); i += 3)
        {
            rgb[i] /= weightR; rgb[i + 1] /= weightG; rgb[i + 2] /= weightB;
            peak = (std::max)(peak, rgb[i + 1]);
        }
        if (peak > 0.0f)
            for (float& v : rgb)
                v /= peak;

        return rgb;
    }
}
