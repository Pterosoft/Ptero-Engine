// ---------------------------------------------------------------------------
// Light shape gizmos.
//
// A light's reach and cone are invisible until something is lit by them, which
// makes placing one guesswork: you move it, wait for the GI to converge, and
// infer from the result where it actually pointed. These draw the shape the
// renderer will use - the sphere of a point light, the cone of a spot, the
// quad of a rect - so the placement is visible before the lighting is.
//
// They are projected line work on the QtUi overlay, like the placement icons
// and the terrain bounds, not a GPU pass. That keeps them out of the G-Buffer
// and out of every temporal history that reads it: a wireframe that only the
// editor can see must not be something TAA or the GI denoiser has to forget.
//
// The geometry here is derived exactly the way DX12SceneRenderer derives the
// GPU light record - local -Z is the direction, local +X and +Y span the
// rectangle, cone angles are halved, and entity scale is ignored because the
// renderer ignores it too. Anything else would draw a confident lie.
// ---------------------------------------------------------------------------

#include "pch.h"
#include "Editor.h"

#include "../QtUi/QtUi.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace
{
    // Enough that a circle reads as round at the size these are usually seen,
    // few enough that a scene full of lights does not bury the overlay painter
    // in antialiased segments. Unselected lights get the coarse count: they are
    // context, not the thing being placed.
    constexpr int kSelectedSegments = 32;
    constexpr int kUnselectedSegments = 16;

    // A light with no reach has no shape to draw, and the cone maths divides by
    // it further down.
    constexpr float kMinimumRadius = 0.001f;

    // Scales the light's own colour so the dimmest light is still legible
    // against the viewport, then applies the alpha that separates the selected
    // light from the rest.
    UiU32 ShapeColor(const PointLightComponent& light, bool selected, float alphaScale)
    {
        float r = (std::max)(light.ColorR, 0.0f);
        float g = (std::max)(light.ColorG, 0.0f);
        float b = (std::max)(light.ColorB, 0.0f);

        const float brightest = (std::max)({ r, g, b });
        if (brightest > 0.0001f)
        {
            r /= brightest;
            g /= brightest;
            b /= brightest;
        }
        else
        {
            // A black light still has a position and a reach worth seeing.
            r = g = b = 1.0f;
        }

        const auto channel = [](float value)
        {
            return static_cast<int>(std::lround((std::clamp)(value, 0.0f, 255.0f)));
        };
        const float alpha = (selected ? 235.0f : 110.0f) * alphaScale;
        return UI_COL32(channel(r * 255.0f), channel(g * 255.0f), channel(b * 255.0f), channel(alpha));
    }

    // Projects a world-space segment to viewport pixels, clipped against the near plane.
    // TryProjectWorldToViewport rejects any point off screen, so a segment drawn through
    // it vanishes as soon as either end leaves the view - which, for a volume tens of
    // metres across seen from inside or close up, is nearly every edge. Here only the
    // part behind the camera is cut; the rest may run off screen and the draw list clips
    // it to the viewport.
    bool ProjectClippedSegment(
        const XMMATRIX& viewProjection,
        const UiVec2& viewportOrigin,
        const UiVec2& viewportSize,
        const XMFLOAT3& a,
        const XMFLOAT3& b,
        UiVec2& outA,
        UiVec2& outB)
    {
        constexpr float kNearW = 0.01f;

        XMVECTOR clipA = XMVector4Transform(XMVectorSetW(XMLoadFloat3(&a), 1.0f), viewProjection);
        XMVECTOR clipB = XMVector4Transform(XMVectorSetW(XMLoadFloat3(&b), 1.0f), viewProjection);
        const float wA = XMVectorGetW(clipA);
        const float wB = XMVectorGetW(clipB);
        if (wA < kNearW && wB < kNearW)
            return false;
        if (wA < kNearW)
            clipA = XMVectorLerp(clipA, clipB, (kNearW - wA) / (wB - wA));
        else if (wB < kNearW)
            clipB = XMVectorLerp(clipB, clipA, (kNearW - wB) / (wA - wB));

        const auto toViewport = [&](const XMVECTOR& clip)
        {
            const float w = XMVectorGetW(clip);
            const float ndcX = XMVectorGetX(clip) / w;
            const float ndcY = XMVectorGetY(clip) / w;
            return UiVec2(
                viewportOrigin.x + (ndcX * 0.5f + 0.5f) * viewportSize.x,
                viewportOrigin.y + (0.5f - ndcY * 0.5f) * viewportSize.y);
        };
        outA = toViewport(clipA);
        outB = toViewport(clipB);

        // Then to the viewport rectangle (Liang-Barsky): a point just past the near
        // plane projects hundreds of thousands of pixels out, which is no coordinate to
        // hand the painter.
        const float minX = viewportOrigin.x, maxX = viewportOrigin.x + viewportSize.x;
        const float minY = viewportOrigin.y, maxY = viewportOrigin.y + viewportSize.y;
        const float dx = outB.x - outA.x;
        const float dy = outB.y - outA.y;
        float t0 = 0.0f;
        float t1 = 1.0f;
        const float p[4] = { -dx, dx, -dy, dy };
        const float q[4] = { outA.x - minX, maxX - outA.x, outA.y - minY, maxY - outA.y };
        for (int edge = 0; edge < 4; ++edge)
        {
            if (std::abs(p[edge]) < 1e-6f)
            {
                if (q[edge] < 0.0f)
                    return false;
                continue;
            }
            const float t = q[edge] / p[edge];
            if (p[edge] < 0.0f)
                t0 = (std::max)(t0, t);
            else
                t1 = (std::min)(t1, t);
            if (t0 > t1)
                return false;
        }

        const UiVec2 start = outA;
        outA = UiVec2(start.x + dx * t0, start.y + dy * t0);
        outB = UiVec2(start.x + dx * t1, start.y + dy * t1);
        return true;
    }
}

void Editor::DrawVegetationAreaBounds(
    const UiVec2& viewportOrigin,
    const UiVec2& viewportSize,
    const EditorCamera& camera) const
{
    UiDrawList* drawList = QtUi::GetWindowDrawList();
    const XMMATRIX viewProjection = camera.GetViewMatrix() * camera.GetProjectionMatrix();

    for (std::size_t entityIndex = 0; entityIndex < mEntities.size(); ++entityIndex)
    {
        const Entity& entity = mEntities[entityIndex];
        if (!entity.VegetationArea.has_value())
            continue;

        const VegetationAreaComponent& area = *entity.VegetationArea;
        const bool selected = IsEntitySelected(static_cast<int>(entityIndex));
        // A selected area always shows its volume: it is the thing being placed.
        if (!area.ShowBounds && !selected)
            continue;

        // Green adds vegetation, red carves it out - the same colours the scatter
        // debug view has always used.
        const int alpha = selected ? 240 : 150;
        const UiU32 color = area.IsExclusionVolume
            ? UI_COL32(242, 90, 76, alpha)
            : UI_COL32(102, 217, 89, alpha);
        const float thickness = selected ? 2.0f : 1.3f;

        // The scatter's convention: rotation and translation only, no entity scale, so
        // the outline is exactly the volume vegetation is placed in.
        const XMMATRIX rotation = PteroTransform::ComposeRotation(entity.Transform.Rotation);
        const XMVECTOR origin = XMLoadFloat3(&entity.Transform.Position);
        const auto toWorld = [&](float x, float y, float z)
        {
            XMFLOAT3 world{};
            XMStoreFloat3(&world, XMVectorAdd(origin, XMVector3TransformNormal(XMVectorSet(x, y, z, 0.0f), rotation)));
            return world;
        };
        const auto line = [&](const XMFLOAT3& a, const XMFLOAT3& b)
        {
            UiVec2 sa{};
            UiVec2 sb{};
            if (ProjectClippedSegment(viewProjection, viewportOrigin, viewportSize, a, b, sa, sb))
                drawList->AddLine(sa, sb, color, thickness);
        };
        // A prism: the footprint outline at the bottom and top of the volume, joined
        // at every corner.
        const auto prism = [&](const std::vector<XMFLOAT2>& footprint, float halfHeight)
        {
            const std::size_t count = footprint.size();
            for (std::size_t i = 0; i < count; ++i)
            {
                const XMFLOAT2& p = footprint[i];
                const XMFLOAT2& q = footprint[(i + 1) % count];
                line(toWorld(p.x, p.y, -halfHeight), toWorld(q.x, q.y, -halfHeight));
                line(toWorld(p.x, p.y,  halfHeight), toWorld(q.x, q.y,  halfHeight));
                line(toWorld(p.x, p.y, -halfHeight), toWorld(p.x, p.y,  halfHeight));
            }
        };

        switch (area.Shape)
        {
        case VegetationAreaShape::Box:
            prism({ { -area.ExtentX, -area.ExtentY }, { area.ExtentX, -area.ExtentY },
                    {  area.ExtentX,  area.ExtentY }, { -area.ExtentX, area.ExtentY } },
                  area.ExtentZ);
            break;

        case VegetationAreaShape::Sphere:
        {
            const float radius = area.ExtentX;
            const int segments = selected ? kSelectedSegments * 2 : kSelectedSegments;
            for (int step = 0; step < segments; ++step)
            {
                const float a0 = XM_2PI * static_cast<float>(step) / static_cast<float>(segments);
                const float a1 = XM_2PI * static_cast<float>(step + 1) / static_cast<float>(segments);
                const float c0 = std::cos(a0) * radius, s0 = std::sin(a0) * radius;
                const float c1 = std::cos(a1) * radius, s1 = std::sin(a1) * radius;
                line(toWorld(c0, s0, 0.0f), toWorld(c1, s1, 0.0f));
                line(toWorld(c0, 0.0f, s0), toWorld(c1, 0.0f, s1));
                line(toWorld(0.0f, c0, s0), toWorld(0.0f, c1, s1));
            }
            break;
        }

        case VegetationAreaShape::Polygon:
            if (area.PolygonPoints.size() >= 2)
                prism(area.PolygonPoints, area.ExtentZ);
            break;
        }
    }
}

void Editor::DrawLightShapeGizmos(
    const UiVec2& viewportOrigin,
    const UiVec2& viewportSize,
    const EditorCamera& camera) const
{
    // Deliberately the same switch as the placement icons. These are the same
    // kind of thing - editor furniture drawn over the scene - and a user who
    // has turned the icons off is asking for an unobstructed view of the
    // render, which a cage of light wireframes would defeat just as much.
    if (!mShowViewportPlacementIcons)
        return;

    UiDrawList* drawList = QtUi::GetWindowDrawList();
    const XMMATRIX viewProjection = camera.GetViewMatrix() * camera.GetProjectionMatrix();

    const auto line = [&](const XMFLOAT3& a, const XMFLOAT3& b, UiU32 color, float thickness)
    {
        UiVec2 sa{};
        UiVec2 sb{};
        // Clipped rather than clamped: a segment with an endpoint behind the camera
        // clamped to the viewport edge would be drawn as a line that does not exist.
        if (ProjectClippedSegment(viewProjection, viewportOrigin, viewportSize, a, b, sa, sb))
            drawList->AddLine(sa, sb, color, thickness);
    };

    // A circle of `radius` around `center`, spanned by two orthogonal unit
    // vectors. Drawn segment by segment through `line` so a circle that is
    // half behind the camera still draws the half that is not.
    const auto circle = [&](const XMVECTOR& center, const XMVECTOR& axisU, const XMVECTOR& axisV,
                            float radius, int segments, UiU32 color, float thickness)
    {
        XMFLOAT3 previous{};
        for (int step = 0; step <= segments; ++step)
        {
            const float angle = XM_2PI * static_cast<float>(step) / static_cast<float>(segments);
            const XMVECTOR point = center
                + axisU * (std::cos(angle) * radius)
                + axisV * (std::sin(angle) * radius);

            XMFLOAT3 current{};
            XMStoreFloat3(&current, point);
            if (step > 0)
                line(previous, current, color, thickness);
            previous = current;
        }
    };

    for (std::size_t entityIndex = 0; entityIndex < mEntities.size(); ++entityIndex)
    {
        const Entity& entity = mEntities[entityIndex];
        if (!entity.PointLight.has_value())
            continue;

        const PointLightComponent& light = *entity.PointLight;

        const float radius = (std::max)(light.Radius, kMinimumRadius);
        const bool selected = IsEntitySelected(static_cast<int>(entityIndex));
        const int segments = selected ? kSelectedSegments : kUnselectedSegments;
        const float thickness = selected ? 1.6f : 1.2f;

        const XMVECTOR origin = XMLoadFloat3(&entity.Transform.Position);

        // Scale is not read: the renderer builds the light record from position
        // and rotation only, so a scaled entity must not draw a scaled light.
        const XMMATRIX rotation = PteroTransform::ComposeRotation(entity.Transform.Rotation);
        const XMVECTOR right   = XMVector3Normalize(XMVector3TransformNormal(g_XMIdentityR0, rotation));
        const XMVECTOR up      = XMVector3Normalize(XMVector3TransformNormal(g_XMIdentityR1, rotation));
        const XMVECTOR forward = XMVector3Normalize(XMVector3TransformNormal(g_XMNegIdentityR2, rotation));

        const UiU32 mainColor = ShapeColor(light, selected, 1.0f);
        const UiU32 hintColor = ShapeColor(light, selected, 0.45f);

        switch (light.Type)
        {
        case LightType::Point:
        {
            // Three world-aligned great circles. A single camera-facing circle
            // is cheaper but reads as a flat ring; three planes read as a
            // sphere from any angle, which is what the light actually is.
            circle(origin, g_XMIdentityR0, g_XMIdentityR1, radius, segments, mainColor, thickness);
            circle(origin, g_XMIdentityR1, g_XMIdentityR2, radius, segments, mainColor, thickness);
            circle(origin, g_XMIdentityR0, g_XMIdentityR2, radius, segments, mainColor, thickness);

            // The emissive source is a sphere in its own right whenever it is
            // not a point, and it is what softens the shadow - worth seeing.
            if (light.SourceRadius > kMinimumRadius)
            {
                circle(origin, g_XMIdentityR0, g_XMIdentityR1, light.SourceRadius, segments, hintColor, 1.0f);
                circle(origin, g_XMIdentityR1, g_XMIdentityR2, light.SourceRadius, segments, hintColor, 1.0f);
            }
            break;
        }

        case LightType::Spot:
        {
            // The component stores full cone angles, which is what an artist
            // measures; the cone's half-angle is what the geometry needs.
            const float outerHalf = XMConvertToRadians((std::clamp)(light.SpotOuterConeDegrees, 0.1f, 179.0f) * 0.5f);
            const float innerHalf = XMConvertToRadians((std::clamp)(light.SpotInnerConeDegrees, 0.1f, 179.0f) * 0.5f);

            // A flat cap at the range distance rather than a spherical one:
            // the straight edges from the apex are what communicate the angle,
            // and a curved cap only makes the drawing busier.
            const XMVECTOR capCenter = origin + forward * radius;
            const float outerCapRadius = radius * std::tan(outerHalf);
            const float innerCapRadius = radius * std::tan((std::min)(innerHalf, outerHalf));

            circle(capCenter, right, up, outerCapRadius, segments, mainColor, thickness);

            XMFLOAT3 apex{};
            XMStoreFloat3(&apex, origin);
            for (int edge = 0; edge < 4; ++edge)
            {
                const float angle = XM_PIDIV2 * static_cast<float>(edge);
                const XMVECTOR rim = capCenter
                    + right * (std::cos(angle) * outerCapRadius)
                    + up * (std::sin(angle) * outerCapRadius);
                XMFLOAT3 rimPoint{};
                XMStoreFloat3(&rimPoint, rim);
                line(apex, rimPoint, mainColor, thickness);
            }

            // Only when it says something the outer cone does not: an inner
            // cone equal to the outer is a hard-edged spot, and drawing the
            // same circle twice would just thicken it.
            if (innerCapRadius < outerCapRadius * 0.98f)
                circle(capCenter, right, up, innerCapRadius, segments, hintColor, 1.0f);
            break;
        }

        case LightType::Rect:
        {
            const float halfWidth = (std::max)(light.RectWidth, 0.0f) * 0.5f;
            const float halfHeight = (std::max)(light.RectHeight, 0.0f) * 0.5f;

            // The emitting quad, in the local XY plane exactly as the shader
            // reconstructs it.
            XMFLOAT3 corner[4]{};
            XMStoreFloat3(&corner[0], origin - right * halfWidth - up * halfHeight);
            XMStoreFloat3(&corner[1], origin + right * halfWidth - up * halfHeight);
            XMStoreFloat3(&corner[2], origin + right * halfWidth + up * halfHeight);
            XMStoreFloat3(&corner[3], origin - right * halfWidth + up * halfHeight);
            for (int edge = 0; edge < 4; ++edge)
                line(corner[edge], corner[(edge + 1) % 4], mainColor, thickness);

            // Which way it faces, and how far it reaches, in one line. A rect
            // light that is pointing away from the surface it was meant to lift
            // looks identical to one that is not until this is drawn.
            XMFLOAT3 center{};
            XMFLOAT3 reach{};
            XMStoreFloat3(&center, origin);
            XMStoreFloat3(&reach, origin + forward * radius);
            line(center, reach, hintColor, thickness);

            if (light.RectTwoSided)
            {
                XMFLOAT3 backReach{};
                XMStoreFloat3(&backReach, origin - forward * radius);
                line(center, backReach, hintColor, thickness);
            }

            // Radius is the falloff range for a rect exactly as it is for a
            // point, so it is drawn the same way rather than invented anew -
            // faintly, because the quad is the part being placed.
            circle(origin, g_XMIdentityR0, g_XMIdentityR1, radius, kUnselectedSegments, hintColor, 1.0f);
            circle(origin, g_XMIdentityR1, g_XMIdentityR2, radius, kUnselectedSegments, hintColor, 1.0f);
            circle(origin, g_XMIdentityR0, g_XMIdentityR2, radius, kUnselectedSegments, hintColor, 1.0f);
            break;
        }
        }
    }
}
