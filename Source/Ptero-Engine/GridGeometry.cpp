#include "GridGeometry.h"

using namespace DirectX;

std::vector<Vertex> GenerateFloorGridVertices()
{
    constexpr float gridExtent = 50.0f;
    constexpr float lineSpacing = 1.0f;
    constexpr XMFLOAT4 gridColor(0.2f, 0.2f, 0.2f, 1.0f);

    std::vector<Vertex> vertices;
    vertices.reserve(404);

    // Emit X-aligned and Z-aligned line segments so the result can be drawn as a line list.
    for (float position = -gridExtent; position <= gridExtent; position += lineSpacing)
    {
        // The floor grid does not need UVs, so those remain at zero while the normal points upward.
        vertices.push_back({ XMFLOAT3(position, 0.0f, -gridExtent), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.0f, 0.0f), gridColor });
        vertices.push_back({ XMFLOAT3(position, 0.0f,  gridExtent), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.0f, 0.0f), gridColor });
        vertices.push_back({ XMFLOAT3(-gridExtent, 0.0f, position), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.0f, 0.0f), gridColor });
        vertices.push_back({ XMFLOAT3( gridExtent, 0.0f, position), XMFLOAT3(0.0f, 1.0f, 0.0f), XMFLOAT2(0.0f, 0.0f), gridColor });
    }

    return vertices;
}
