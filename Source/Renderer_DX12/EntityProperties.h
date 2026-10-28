#pragma once

struct TimeOfDaySettings;
struct VolumetricFogSettings;

// The owner object of the "Environment" property domain: the engine-wide settings a
// cinematic or a graph may drive. The scene renderer owns the settings; this only points
// at them. A system that wants its own settings animatable (weather, say) adds a pointer
// here and registers its properties in EntityProperties.cpp - nothing in the Sequencer or
// the Node Graph changes.
struct EngineEnvironment
{
    TimeOfDaySettings* TimeOfDay = nullptr;
    VolumetricFogSettings* VolumetricFog = nullptr;
};

// Registers every "Entity" property (owner: Entity*) and every "Environment" property
// (owner: EngineEnvironment*) with the PropertyRegistry. Safe to call any number of times;
// only the first does anything. Called by whatever first needs the registry - the
// cinematic host, the Node Graph host, the Sequencer.
void RegisterEngineProperties();
