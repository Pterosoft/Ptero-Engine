# Copilot Instructions

## Project Guidelines
- Place renderer feature implementation code under Ptero-Engine\Source\Renderer_DX12 and runtime shaders under Ptero-Engine\Data\Shaders; avoid creating files under nested Ptero-Engine\Source\Ptero-Engine\Ptero-Engine paths.
- When adding entities to the level, ensure new entities maintain X/Y coordinates directly under the user's mouse position, with Z set to 0.
- Prefer uninterrupted execution through all plan steps instead of step-by-step narration during fixes.

## Performance Settings
- For radiance cache GI, keep the rays-per-probe setting at 1 for both radiance cache and radiance cascades work, as higher values tank performance. Focus fixes on Radiance Cascades GI rather than Radiance Probes GI.