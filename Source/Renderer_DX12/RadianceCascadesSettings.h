#pragma once

enum class GlobalIlluminationMode
{
	Disabled = 0,
	Rtgi,
	RadianceCascades
};

struct RadianceCascadesSettings
{
	bool Enabled = false;
	unsigned int CascadeCount = 4;
	unsigned int ProbeSpacingBase = 16;
	unsigned int RaysPerProbe = 8;
	float RayLengthBase = 1.5f;
	float RayLengthScale = 2.0f;
	float IntervalLengthScale = 1.0f;
	float Hysteresis = 0.8f;
	float GiIntensity = 1.0f;
	float ColorBleedingStrength = 2.0f;
	unsigned int SparseProbeTableCapacity = 65536;
	unsigned int SparseProbeCellSize = 16;
	unsigned int SparseProbeSearchSteps = 32;
	float SparseProbeReuseStrength = 0.35f;
	float RayBias = 0.02f;
	float SpatialFilterStrength = 1.0f;
	float HistoryClampScale = 0.4f;
	float HistoryDepthSensitivity = 128.0f;
	float HistoryNormalThreshold = 0.92f;
	int DebugView = 0;
};
