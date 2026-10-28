#pragma once

// LensFlareOptics - the CPU half of the physically based lens flare.
//
// Loads a lens prescription (Data/LensFlares/Lenses/*.xml, the format of the lens flare
// framework in Source/SDKs/LensFlareFramework-master), prepares its surfaces for one
// wavelength, and traces rays through it the same way LensFlare_Common.hlsli does on
// the GPU. The GPU does the per-frame work; the CPU tracer is used once per lens to rank
// the ghosts by how bright they get, so only the ones worth drawing are sent down.
//
// Coordinates follow the framework: the optical axis is z, the front of the lens sits at
// z = SensorDistance() and the sensor at z = 0, rays travel towards -z, and all lengths
// are millimetres.

#include <filesystem>
#include <string>
#include <vector>

namespace LensFlareOptics
{
    // One row of the prescription: a refracting surface (or the aperture stop, or the
    // sensor), the index of the medium behind it, and the distance to the next surface.
    struct Element
    {
        float Thickness = 0.0f;
        float Radius = 0.0f;          // radius of curvature; 0 = flat
        float Ior = 1.0f;             // index (d line) of the medium after this surface
        float Abbe = 89.3f;
        float Height = 0.0f;          // clear semi-aperture
        float CoatingLambda = 620.0f; // quarter-wave AR coating tuned to this wavelength
        float CoatingIor = 1.38f;
        bool  IsAperture = false;
    };

    struct Lens
    {
        std::string Name;
        float FNumber = 8.0f;
        float FocalLength = 50.0f;
        float FieldOfView = 45.0f;
        float FilmWidth = 36.0f;
        float FilmHeight = 24.0f;
        // Element 0 is a flat dummy entrance plane, the last element is the sensor.
        std::vector<Element> Elements;
        int ApertureIndex = -1;
    };

    // Lens folder under the Data root; resolves through DataFiles, so it works packaged.
    std::filesystem::path LensDirectory();

    // File names (no extension) of every lens in LensDirectory(), sorted.
    std::vector<std::string> ListLenses();

    bool LoadLens(const std::string& name, Lens& out, std::string& error);

    // Glass dispersion from the d-line index and Abbe number (Cauchy fit).
    float CauchyIor(float lambdaNm, float nd, float abbe);

    // Distance from the entrance plane to the sensor along the axis.
    float SensorDistance(const Lens& lens);

    // Radius of the entrance pupil for an f-number: EFL / (2 N).
    float EntrancePupilRadius(const Lens& lens, float fNumber);

    // Height of the physical iris that gives that entrance pupil, found with a paraxial
    // trace up to the stop. Never larger than the stop's own clear aperture.
    float PhysicalApertureHeight(const Lens& lens, float fNumber, float lambdaNm = 575.0f);

    // A surface prepared for one wavelength. Must match LensSurface in
    // LensFlare_Common.hlsli: two float4s per surface.
    struct Surface
    {
        float CenterZ;     // z of the sphere centre (of the plane itself when flat)
        float Radius;      // 0 = flat
        float Height;      // clear semi-aperture
        float Aperture;    // > 0 marks the iris: its physical height
        float IorBefore;
        float CoatingIor;
        float IorAfter;
        float CoatingThickness;
    };

    std::vector<Surface> BuildSurfaces(const Lens& lens, float lambdaNm, float apertureHeight);

    // A two-bounce ghost: the ray reflects off First on the way in, travels back and
    // reflects off Second (Second < First), then continues to the sensor. Both surfaces
    // lie on the same side of the aperture, as in the framework.
    struct Ghost
    {
        int First = -1;
        int Second = -1;
    };

    std::vector<Ghost> EnumerateGhosts(const Lens& lens);

    // Aperture shape, shared with the GPU: 1 on the boundary of the iris.
    struct ApertureShape
    {
        int   Blades = 6;
        float RotationRadians = 0.0f;
        float Roundness = 0.0f;
    };

    float ApertureDistance(float x, float y, const ApertureShape& shape);

    struct Ray
    {
        float Px, Py, Pz;
        float Dx, Dy, Dz;
    };

    struct TraceResult
    {
        float SensorX = 0.0f, SensorY = 0.0f;
        float ApertureX = 0.0f, ApertureY = 0.0f; // normalised by the iris height
        float Intensity = 0.0f;                   // product of the two Fresnel reflections
        float RelativeRadius = 0.0f;              // worst |xy| / clear height over all surfaces
        bool  Valid = false;                      // hit every surface, no total internal reflection
    };

    // ghost.First < 0 traces the direct (image forming) path.
    TraceResult TraceRay(
        const std::vector<Surface>& surfaces,
        const Ghost& ghost,
        float lambdaNm,
        Ray ray,
        const ApertureShape& aperture);

    // Where on the entrance plane the direct path gets through the iris, for a light at
    // tan(angle) = tanAngle in the xz plane. A stopped-down lens lets light in through a
    // pupil only a few millimetres wide, smaller than a coarse scan of the whole front
    // element can reliably hit, so ghost searches sample this region as well.
    struct PupilRegion
    {
        float MinX = 0.0f, MinY = 0.0f, MaxX = -1.0f, MaxY = -1.0f; // empty when Max < Min
        bool IsEmpty() const { return MaxX < MinX || MaxY < MinY; }
    };

    PupilRegion DirectPupilRegion(
        const Lens& lens,
        const std::vector<Surface>& surfaces,
        float lambdaNm,
        float tanAngle,
        const ApertureShape& aperture);

    // Millimetres of image height per unit of tan(field angle), measured by tracing the
    // direct path near the axis. The nominal focal length in the lens files is often 10-30%
    // off what their surfaces actually do, and the flare is placed on screen with this so
    // the image of the light lands exactly on the light.
    float ImageScale(const Lens& lens, float fNumber, const ApertureShape& aperture);

    struct RankedGhost
    {
        Ghost Surfaces;
        float Score = 0.0f;
    };

    // All ghosts of the lens, brightest first. Scored by energy over apparent size at a few
    // off-axis light angles up to maxTan (tangent of the light's angle to the axis).
    std::vector<RankedGhost> RankGhosts(const Lens& lens, float fNumber, float maxTan, const ApertureShape& aperture);
}
