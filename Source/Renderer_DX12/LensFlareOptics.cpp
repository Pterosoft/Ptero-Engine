#include "pch.h"
#include "LensFlareOptics.h"

#include "System/DataFiles.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>

namespace LensFlareOptics
{
    namespace
    {
        constexpr float kPi = 3.14159265358979f;

        // The prescriptions are flat, machine-written XML (see saveCameraPatent in the
        // framework's PhysicalCamera.cpp): one value per tag, no attributes, no nesting
        // beyond <elements><element>. A tag scanner is all they need.
        bool FindTag(const std::string& text, const char* tag, std::size_t from, std::size_t to, std::string& value)
        {
            const std::string open = std::string("<") + tag + ">";
            const std::string close = std::string("</") + tag + ">";
            const std::size_t start = text.find(open, from);
            if (start == std::string::npos || start >= to)
                return false;
            const std::size_t valueStart = start + open.size();
            const std::size_t end = text.find(close, valueStart);
            if (end == std::string::npos || end > to)
                return false;
            value = text.substr(valueStart, end - valueStart);
            return true;
        }

        float TagFloat(const std::string& text, const char* tag, std::size_t from, std::size_t to, float fallback)
        {
            std::string value;
            if (!FindTag(text, tag, from, to, value))
                return fallback;
            char* end = nullptr;
            const float parsed = std::strtof(value.c_str(), &end);
            return end != value.c_str() ? parsed : fallback;
        }

        // 2x2 paraxial ray-transfer matrices acting on (height, angle).
        struct Mat2
        {
            float A = 1.0f, B = 0.0f, C = 0.0f, D = 1.0f;
        };

        Mat2 Multiply(const Mat2& l, const Mat2& r)
        {
            return { l.A * r.A + l.B * r.C, l.A * r.B + l.B * r.D, l.C * r.A + l.D * r.C, l.C * r.B + l.D * r.D };
        }

        Mat2 Translation(float distance) { return { 1.0f, distance, 0.0f, 1.0f }; }

        Mat2 Refraction(float n1, float n2, float radius)
        {
            Mat2 m;
            m.D = n1 / n2;
            if (std::fabs(radius) > 1.0e-4f)
                m.C = (n1 - n2) / (n2 * radius);
            return m;
        }

        float Dot3(float ax, float ay, float az, float bx, float by, float bz)
        {
            return ax * bx + ay * by + az * bz;
        }

        // Thin-film AR coating reflectance (the framework's fresnelAR, from Hullin et al.).
        float FresnelAR(float theta0, float lambda, float n0, float n1, float n2, float d)
        {
            if (std::fabs(theta0) < 1.0e-3f)
                theta0 = 1.0e-3f;

            const float st0 = std::sin(theta0);
            const float s1 = st0 * n0 / n1;
            const float s2 = st0 * n0 / n2;
            if (std::fabs(s1) >= 1.0f || std::fabs(s2) >= 1.0f)
                return 1.0f; // total internal reflection inside the coating or the glass

            const float theta1 = std::asin(s1);
            const float theta2 = std::asin(s2);

            const float st01 = std::sin(theta0 + theta1);
            const float tt01 = std::tan(theta0 + theta1);

            const float rs01 = -std::sin(theta0 - theta1) / st01;
            const float rp01 = std::tan(theta0 - theta1) / tt01;
            const float ts01 = 2.0f * std::sin(theta1) * std::cos(theta0) / st01;
            const float tp01 = ts01 * std::cos(theta0 - theta1);

            const float rs12 = -std::sin(theta1 - theta2) / std::sin(theta1 + theta2);
            const float rp12 = std::tan(theta1 - theta2) / std::tan(theta1 + theta2);

            const float ris = ts01 * ts01 * rs12;
            const float rip = tp01 * tp01 * rp12;

            const float dy = d * n1;
            const float dx = std::tan(theta1) * dy;
            const float delay = std::sqrt(dx * dx + dy * dy);
            const float relPhase = 4.0f * kPi / lambda * (delay - dx * st0);
            const float crp = std::cos(relPhase);

            const float outS2 = rs01 * rs01 + ris * ris + 2.0f * rs01 * ris * crp;
            const float outP2 = rp01 * rp01 + rip * rip + 2.0f * rp01 * rip * crp;
            return (outS2 + outP2) * 0.5f;
        }

        float CoatingThickness(float coatingLambda, float n0, float n1, float n2)
        {
            return coatingLambda / 4.0f / (std::max)(std::sqrt(n0 * n2), n1);
        }
    }

    std::filesystem::path LensDirectory()
    {
        return DataFiles::FindDataDirectory() / L"LensFlares" / L"Lenses";
    }

    std::vector<std::string> ListLenses()
    {
        std::vector<std::string> names;
        const std::filesystem::path directory = LensDirectory();
        for (const std::filesystem::path& path : DataFiles::ListFiles(directory, false))
        {
            std::wstring extension = path.extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
            if (extension == L".xml")
                names.push_back(path.stem().string());
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    bool LoadLens(const std::string& name, Lens& out, std::string& error)
    {
        const std::filesystem::path path = LensDirectory() / (name + ".xml");
        std::string text;
        if (!DataFiles::ReadText(path, text))
        {
            error = "Lens file not found: " + path.string();
            return false;
        }

        const std::size_t systemStart = text.find("<opticalSystem>");
        if (systemStart == std::string::npos)
        {
            error = "Not a lens prescription (no <opticalSystem>): " + path.string();
            return false;
        }

        Lens lens;
        lens.Name = name;
        const std::size_t end = text.size();
        lens.FNumber = TagFloat(text, "fnumber", systemStart, end, 8.0f);
        lens.FocalLength = TagFloat(text, "effectiveFocalLength", systemStart, end, 50.0f);
        lens.FieldOfView = TagFloat(text, "fieldOfView", systemStart, end, 45.0f);
        lens.FilmWidth = TagFloat(text, "filmWidth", systemStart, end, 36.0f);
        lens.FilmHeight = TagFloat(text, "filmHeight", systemStart, end, 24.0f);
        const float heightMultiplier = TagFloat(text, "heightMultiplier", systemStart, end, 1.0f);

        // Front dummy: a flat entrance plane the rays are launched from.
        lens.Elements.push_back(Element{});

        std::size_t cursor = text.find("<elements>", systemStart);
        while (cursor != std::string::npos)
        {
            const std::size_t elementStart = text.find("<element>", cursor);
            if (elementStart == std::string::npos)
                break;
            const std::size_t elementEnd = text.find("</element>", elementStart);
            if (elementEnd == std::string::npos)
                break;

            std::string type;
            FindTag(text, "type", elementStart, elementEnd, type);

            Element element;
            element.Height = TagFloat(text, "height", elementStart, elementEnd, 10.0f);
            element.Thickness = TagFloat(text, "thickness", elementStart, elementEnd, 0.0f);
            element.Radius = TagFloat(text, "radius", elementStart, elementEnd, 0.0f);
            element.Ior = TagFloat(text, "refractiveIndex", elementStart, elementEnd, 1.0f);
            element.Abbe = TagFloat(text, "abbeNumber", elementStart, elementEnd, 89.3f);
            element.CoatingLambda = TagFloat(text, "coatingLambda", elementStart, elementEnd, 620.0f);
            element.CoatingIor = TagFloat(text, "coatingIor", elementStart, elementEnd, 1.38f);
            if (element.Abbe <= 0.0f)
                element.Abbe = 89.3f;
            if (element.Ior < 1.0f)
                element.Ior = 1.0f;

            if (type == "lensSpherical")
            {
                element.Height *= heightMultiplier;
            }
            else if (type == "apertureStop")
            {
                // The file stores the stop's diameter.
                element.Height *= 0.5f;
                element.Radius = 0.0f;
                element.IsAperture = true;
            }

            lens.Elements.push_back(element);
            cursor = elementEnd + 1;
        }

        if (lens.Elements.size() < 4)
        {
            error = "Lens has too few elements: " + path.string();
            return false;
        }

        lens.Elements[0].Height = lens.Elements[1].Height;
        // The sensor never clips: the flare is allowed to reach past the frame.
        lens.Elements.back().Height = (std::max)(lens.FilmWidth, lens.FilmHeight);

        // The engine's own coating model (the framework does the same in
        // propagateCoatingInformation): one broadband MgF2 coating on every surface.
        for (Element& element : lens.Elements)
        {
            element.CoatingIor = 1.38f;
            element.CoatingLambda = 620.0f;
        }

        for (int i = 0; i < static_cast<int>(lens.Elements.size()); ++i)
        {
            if (lens.Elements[i].IsAperture)
            {
                lens.ApertureIndex = i;
                break;
            }
        }
        if (lens.ApertureIndex < 0)
        {
            error = "Lens has no aperture stop: " + path.string();
            return false;
        }

        out = std::move(lens);
        return true;
    }

    float CauchyIor(float lambdaNm, float nd, float abbe)
    {
        const float b = ((nd - 1.0f) / abbe) * 0.52345f;
        const float a = nd - b * 2.897f;
        const float lambdaUm = lambdaNm * 1.0e-3f;
        return a + b / (lambdaUm * lambdaUm);
    }

    float SensorDistance(const Lens& lens)
    {
        float distance = 0.0f;
        for (const Element& element : lens.Elements)
            distance += element.Thickness;
        return distance;
    }

    float EntrancePupilRadius(const Lens& lens, float fNumber)
    {
        return lens.FocalLength / (std::max)(fNumber, 0.5f) * 0.5f;
    }

    float PhysicalApertureHeight(const Lens& lens, float fNumber, float lambdaNm)
    {
        const float entrance = EntrancePupilRadius(lens, fNumber);
        Mat2 abcd;
        for (int i = 1; i < static_cast<int>(lens.Elements.size()); ++i)
        {
            const Element& element = lens.Elements[i];
            if (element.IsAperture)
                return (std::min)(entrance * std::fabs(abcd.A), element.Height);

            const float n1 = CauchyIor(lambdaNm, lens.Elements[i - 1].Ior, lens.Elements[i - 1].Abbe);
            const float n2 = CauchyIor(lambdaNm, element.Ior, element.Abbe);
            abcd = Multiply(Translation(element.Thickness), Multiply(Refraction(n1, n2, element.Radius), abcd));
        }
        return entrance;
    }

    std::vector<Surface> BuildSurfaces(const Lens& lens, float lambdaNm, float apertureHeight)
    {
        std::vector<Surface> surfaces;
        surfaces.reserve(lens.Elements.size());

        float distance = SensorDistance(lens);
        for (int i = 0; i < static_cast<int>(lens.Elements.size()); ++i)
        {
            const Element& element = lens.Elements[i];
            const Element& previous = lens.Elements[(std::max)(i - 1, 0)];

            Surface surface{};
            surface.Radius = element.IsAperture ? 0.0f : element.Radius;
            surface.Height = element.IsAperture ? apertureHeight : element.Height;
            surface.Aperture = element.IsAperture ? apertureHeight : -1.0f;
            // The framework's convention, which its lens files are written in: the centre
            // sits at vertex + radius. (Standard optics puts it on the image side, -z here;
            // tracing the files that way leaves every lens unable to form an image.)
            surface.CenterZ = distance + surface.Radius;
            surface.IorBefore = CauchyIor(lambdaNm, previous.Ior, previous.Abbe);
            surface.CoatingIor = element.CoatingIor;
            surface.IorAfter = CauchyIor(lambdaNm, element.Ior, element.Abbe);
            surface.CoatingThickness = CoatingThickness(
                element.CoatingLambda, surface.IorBefore, surface.CoatingIor, surface.IorAfter);
            surfaces.push_back(surface);

            distance -= element.Thickness;
        }
        return surfaces;
    }

    std::vector<Ghost> EnumerateGhosts(const Lens& lens)
    {
        std::vector<Ghost> ghosts;
        const int aperture = lens.ApertureIndex;
        const int last = static_cast<int>(lens.Elements.size()) - 1; // sensor

        for (int i = 1; i < aperture; ++i)
            for (int j = i + 1; j < aperture; ++j)
                ghosts.push_back({ j, i });

        for (int i = aperture + 1; i < last; ++i)
            for (int j = i + 1; j < last; ++j)
                ghosts.push_back({ j, i });

        return ghosts;
    }

    float ApertureDistance(float x, float y, const ApertureShape& shape)
    {
        const float radius = std::sqrt(x * x + y * y);
        const int blades = (std::max)(shape.Blades, 3);
        const float sector = 2.0f * kPi / static_cast<float>(blades);
        float angle = std::atan2(y, x) - shape.RotationRadians;
        angle = angle - sector * std::floor(angle / sector) - 0.5f * sector;
        const float polygon = radius * std::cos(angle) / std::cos(0.5f * sector);
        return polygon + (radius - polygon) * std::clamp(shape.Roundness, 0.0f, 1.0f);
    }

    TraceResult TraceRay(
        const std::vector<Surface>& surfaces,
        const Ghost& ghost,
        float lambdaNm,
        Ray ray,
        const ApertureShape& aperture)
    {
        TraceResult result;
        result.Intensity = 1.0f;

        const int count = static_cast<int>(surfaces.size());
        const int reflect[2] = { ghost.First, ghost.Second };
        int phase = 0;
        int delta = 1;

        for (int index = 1; index >= 1 && index < count; index += delta)
        {
            const Surface& s = surfaces[index];

            float px, py, pz, nx, ny, nz;
            if (s.Radius == 0.0f)
            {
                const float t = (s.CenterZ - ray.Pz) / ray.Dz;
                px = ray.Px + ray.Dx * t;
                py = ray.Py + ray.Dy * t;
                pz = ray.Pz + ray.Dz * t;
                nx = 0.0f; ny = 0.0f; nz = ray.Dz > 0.0f ? -1.0f : 1.0f;
            }
            else
            {
                const float dx = ray.Px, dy = ray.Py, dz = ray.Pz - s.CenterZ;
                const float b = Dot3(dx, dy, dz, ray.Dx, ray.Dy, ray.Dz);
                const float c = Dot3(dx, dy, dz, dx, dy, dz) - s.Radius * s.Radius;
                const float discriminant = b * b - c;
                if (discriminant <= 0.0f)
                    return TraceResult{};

                // The lens surface is the cap of the sphere around its vertex; of the two
                // roots, take the one on that hemisphere. Works in both directions of
                // travel, which a ghost needs.
                const float root = std::sqrt(discriminant);
                const float vertexZ = s.CenterZ - s.Radius;
                float t = -b - root;
                if (std::fabs(ray.Pz + ray.Dz * t - vertexZ) > std::fabs(s.Radius))
                    t = -b + root;
                px = ray.Px + ray.Dx * t;
                py = ray.Py + ray.Dy * t;
                pz = ray.Pz + ray.Dz * t;
                const float invR = 1.0f / std::fabs(s.Radius);
                nx = px * invR;
                ny = py * invR;
                nz = (pz - s.CenterZ) * invR;
                if (Dot3(nx, ny, nz, ray.Dx, ray.Dy, ray.Dz) > 0.0f)
                {
                    nx = -nx; ny = -ny; nz = -nz;
                }
            }

            ray.Px = px; ray.Py = py; ray.Pz = pz;

            if (s.Aperture > 0.0f)
            {
                result.ApertureX = px / s.Aperture;
                result.ApertureY = py / s.Aperture;
                continue;
            }

            result.RelativeRadius = (std::max)(result.RelativeRadius, std::sqrt(px * px + py * py) / s.Height);

            const float cosI = -Dot3(ray.Dx, ray.Dy, ray.Dz, nx, ny, nz);
            const bool backwards = (phase % 2) == 1;
            const float n0 = backwards ? s.IorAfter : s.IorBefore;
            const float n2 = backwards ? s.IorBefore : s.IorAfter;

            if (phase < 2 && index == reflect[phase])
            {
                const float theta = std::acos(std::clamp(cosI, -1.0f, 1.0f));
                result.Intensity *= FresnelAR(theta, lambdaNm, n0, s.CoatingIor, n2, s.CoatingThickness);
                ray.Dx += 2.0f * cosI * nx;
                ray.Dy += 2.0f * cosI * ny;
                ray.Dz += 2.0f * cosI * nz;
                delta = -delta;
                ++phase;
            }
            else
            {
                const float eta = n0 / n2;
                const float k = 1.0f - eta * eta * (1.0f - cosI * cosI);
                if (k < 0.0f)
                    return TraceResult{};
                const float scale = eta * cosI - std::sqrt(k);
                ray.Dx = eta * ray.Dx + scale * nx;
                ray.Dy = eta * ray.Dy + scale * ny;
                ray.Dz = eta * ray.Dz + scale * nz;
            }

            const float length = std::sqrt(Dot3(ray.Dx, ray.Dy, ray.Dz, ray.Dx, ray.Dy, ray.Dz));
            ray.Dx /= length; ray.Dy /= length; ray.Dz /= length;
        }

        if (ghost.First >= 0 && phase < 2)
            return TraceResult{};

        (void)aperture;
        result.SensorX = ray.Px;
        result.SensorY = ray.Py;
        result.Valid = std::isfinite(result.Intensity) && std::isfinite(ray.Px) && std::isfinite(ray.Py);
        return result;
    }

    namespace
    {
        Ray EntranceRay(float x, float y, float startZ, float tanAngle)
        {
            const float invLength = 1.0f / std::sqrt(1.0f + tanAngle * tanAngle);
            Ray ray{};
            ray.Px = x;
            ray.Py = y;
            ray.Pz = startZ;
            ray.Dx = -tanAngle * invLength;
            ray.Dy = 0.0f;
            ray.Dz = -invLength;
            return ray;
        }

        bool SurvivesToSensor(const TraceResult& hit, const ApertureShape& aperture)
        {
            return hit.Valid && hit.RelativeRadius <= 1.0f
                && ApertureDistance(hit.ApertureX, hit.ApertureY, aperture) <= 1.0f;
        }
    }

    PupilRegion DirectPupilRegion(
        const Lens& lens,
        const std::vector<Surface>& surfaces,
        float lambdaNm,
        float tanAngle,
        const ApertureShape& aperture)
    {
        constexpr int kGrid = 64;
        const float pupil = lens.Elements[0].Height;
        const float startZ = SensorDistance(lens);
        const float cell = 2.0f * pupil / (kGrid - 1);

        PupilRegion region;
        region.MinX = region.MinY = FLT_MAX;
        region.MaxX = region.MaxY = -FLT_MAX;
        for (int y = 0; y < kGrid; ++y)
        {
            for (int x = 0; x < kGrid; ++x)
            {
                const float px = -pupil + x * cell;
                const float py = -pupil + y * cell;
                const TraceResult hit = TraceRay(surfaces, Ghost{}, lambdaNm, EntranceRay(px, py, startZ, tanAngle), aperture);
                // Only the iris decides: the rims that vignette the direct image need not
                // clip a ghost, whose path through the lens is different.
                if (!hit.Valid || ApertureDistance(hit.ApertureX, hit.ApertureY, aperture) > 1.0f)
                    continue;
                region.MinX = (std::min)(region.MinX, px);
                region.MinY = (std::min)(region.MinY, py);
                region.MaxX = (std::max)(region.MaxX, px);
                region.MaxY = (std::max)(region.MaxY, py);
            }
        }

        if (region.IsEmpty())
            return PupilRegion{};

        // One cell of slack each way, and never less than a few cells across.
        const float minHalf = 2.0f * cell;
        const float cx = 0.5f * (region.MinX + region.MaxX), cy = 0.5f * (region.MinY + region.MaxY);
        const float hx = (std::max)(0.5f * (region.MaxX - region.MinX) + cell, minHalf);
        const float hy = (std::max)(0.5f * (region.MaxY - region.MinY) + cell, minHalf);
        region.MinX = (std::max)(cx - hx, -pupil);
        region.MaxX = (std::min)(cx + hx, pupil);
        region.MinY = (std::max)(cy - hy, -pupil);
        region.MaxY = (std::min)(cy + hy, pupil);
        return region;
    }

    float ImageScale(const Lens& lens, float fNumber, const ApertureShape& aperture)
    {
        constexpr int kGrid = 24;
        constexpr float kLambda = 550.0f;
        const float t = 0.05f * 0.5f * lens.FilmHeight / lens.FocalLength;

        const float apertureHeight = PhysicalApertureHeight(lens, fNumber, kLambda);
        const std::vector<Surface> surfaces = BuildSurfaces(lens, kLambda, apertureHeight);
        const float startZ = SensorDistance(lens);
        const PupilRegion region = DirectPupilRegion(lens, surfaces, kLambda, t, aperture);
        if (region.IsEmpty())
            return lens.FocalLength;

        double sum = 0.0;
        int count = 0;
        for (int y = 0; y < kGrid; ++y)
        {
            for (int x = 0; x < kGrid; ++x)
            {
                const float px = region.MinX + (x + 0.5f) * (region.MaxX - region.MinX) / kGrid;
                const float py = region.MinY + (y + 0.5f) * (region.MaxY - region.MinY) / kGrid;
                const TraceResult hit = TraceRay(surfaces, Ghost{}, kLambda, EntranceRay(px, py, startZ, t), aperture);
                if (!SurvivesToSensor(hit, aperture))
                    continue;
                sum += hit.SensorX;
                ++count;
            }
        }

        const float measured = count > 0 ? static_cast<float>(-sum / count) / t : 0.0f;
        // A lens this far off is broken rather than imprecise; fall back to the nominal.
        if (!(measured > 0.3f * lens.FocalLength && measured < 3.0f * lens.FocalLength))
            return lens.FocalLength;
        return measured;
    }

    std::vector<RankedGhost> RankGhosts(const Lens& lens, float fNumber, float maxTan, const ApertureShape& aperture)
    {
        constexpr int kGrid = 14;
        constexpr float kLambda = 550.0f;
        const float angleFractions[] = { 0.1f, 0.45f, 0.85f };

        const float apertureHeight = PhysicalApertureHeight(lens, fNumber, kLambda);
        const std::vector<Surface> surfaces = BuildSurfaces(lens, kLambda, apertureHeight);
        const float pupil = lens.Elements[0].Height;
        const float startZ = SensorDistance(lens);
        const float entranceRadius = EntrancePupilRadius(lens, fNumber);
        const float entranceArea = kPi * entranceRadius * entranceRadius;
        const float halfFilm = 0.5f * lens.FilmHeight;

        // Each angle is sampled twice: over the whole front element, and over the direct
        // path's pupil, which a stopped-down lens makes too small for the first scan to hit.
        struct Domain { float MinX, MinY, MaxX, MaxY; };
        std::vector<std::vector<Domain>> domains;
        for (float fraction : angleFractions)
        {
            std::vector<Domain> perAngle{ { -pupil, -pupil, pupil, pupil } };
            const PupilRegion region = DirectPupilRegion(lens, surfaces, kLambda, fraction * maxTan, aperture);
            if (!region.IsEmpty())
                perAngle.push_back({ region.MinX, region.MinY, region.MaxX, region.MaxY });
            domains.push_back(perAngle);
        }

        std::vector<RankedGhost> ranked;
        for (const Ghost& ghost : EnumerateGhosts(lens))
        {
            float best = 0.0f;
            for (std::size_t a = 0; a < domains.size(); ++a)
            {
                const float t = angleFractions[a] * maxTan;
                for (const Domain& domain : domains[a])
                {
                    const float stepX = (domain.MaxX - domain.MinX) / kGrid;
                    const float stepY = (domain.MaxY - domain.MinY) / kGrid;
                    const float cellArea = stepX * stepY;

                    float energy = 0.0f;
                    float minX = FLT_MAX, minY = FLT_MAX, maxX = -FLT_MAX, maxY = -FLT_MAX;
                    for (int y = 0; y < kGrid; ++y)
                    {
                        for (int x = 0; x < kGrid; ++x)
                        {
                            const float px = domain.MinX + (x + 0.5f) * stepX;
                            const float py = domain.MinY + (y + 0.5f) * stepY;
                            const TraceResult hit = TraceRay(surfaces, ghost, kLambda, EntranceRay(px, py, startZ, t), aperture);
                            if (!SurvivesToSensor(hit, aperture))
                                continue;

                            energy += hit.Intensity * cellArea;
                            minX = (std::min)(minX, hit.SensorX); maxX = (std::max)(maxX, hit.SensorX);
                            minY = (std::min)(minY, hit.SensorY); maxY = (std::max)(maxY, hit.SensorY);
                        }
                    }

                    if (energy <= 0.0f)
                        continue;

                    // Energy over apparent size: a focused ghost is a bright spot, a defocused
                    // one the same energy spread into a faint veil. Size in film heights.
                    const float extent = ((maxX - minX) + (maxY - minY)) * 0.5f / halfFilm;
                    const float score = (energy / entranceArea) / (0.05f + extent);
                    best = (std::max)(best, score);
                }
            }

            if (best > 0.0f)
                ranked.push_back({ ghost, best });
        }

        std::sort(ranked.begin(), ranked.end(),
            [](const RankedGhost& a, const RankedGhost& b) { return a.Score > b.Score; });
        return ranked;
    }
}
