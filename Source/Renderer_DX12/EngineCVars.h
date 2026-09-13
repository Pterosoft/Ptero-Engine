#pragma once

class DX12SceneRenderer;

// Binds a cvar to every renderer setting. Call once, after the scene renderer
// exists and before the first frame: the cvars hold pointers into its settings
// structs, so the renderer must outlive the registry (it does - it lives for
// the process).
void RegisterEngineCVars(DX12SceneRenderer& renderer);
