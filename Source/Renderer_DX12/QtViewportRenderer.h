#pragma once
#include "../QtUi/UiTypes.h"
#include <d3d12.h>

namespace QtViewportRenderer
{
bool Initialize(ID3D12Device *device);
void Shutdown();
void Draw(ID3D12GraphicsCommandList *commands, UiTextureID texture, unsigned width, unsigned height);
// Composites a premultiplied-alpha texture over what Draw just put in the viewport.
void DrawOverlay(ID3D12GraphicsCommandList *commands, UiTextureID texture, unsigned width, unsigned height);
void DrawDebugViews(ID3D12GraphicsCommandList *commands);
void PresentDebugViews();
} // namespace QtViewportRenderer
