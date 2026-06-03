#pragma once

#include "..\System\include\System\Mesh.h"
#include "..\SDKs\nlohmann\json.hpp"

#include <DirectXMath.h>

#include <memory>
#include <optional>
#include <string>

struct TransformComponent
{
    static constexpr float MeshWorldScale = 0.1f;

    DirectX::XMFLOAT3 Position{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 Rotation{ 0.0f, 0.0f, 0.0f };
    DirectX::XMFLOAT3 Scale{ 1.0f, 1.0f, 1.0f };

    DirectX::XMMATRIX GetTransform() const
    {
        // Compose the entity transform from the editor-facing position, Euler rotation, and scale values.
        // Imported meshes render 10x too large in this renderer, so apply a fixed global mesh scale.
        const DirectX::XMMATRIX scaleMatrix = DirectX::XMMatrixScaling(
            Scale.x * MeshWorldScale,
            Scale.y * MeshWorldScale,
            Scale.z * MeshWorldScale);
        const DirectX::XMMATRIX rotationMatrix = DirectX::XMMatrixRotationRollPitchYaw(Rotation.x, Rotation.y, Rotation.z);
        const DirectX::XMMATRIX translationMatrix = DirectX::XMMatrixTranslation(Position.x, Position.y, Position.z);
        return scaleMatrix * rotationMatrix * translationMatrix;
    }
};

struct MeshComponent
{
    std::shared_ptr<Mesh> MeshAsset;
    // Relative path (from Data/) of the imported .fbx used to load MeshAsset.
    std::string MeshPath;
    // Relative path (from Data/) of the assigned .json material or multi-material file.
    std::string MaterialPath;
    // Multiplies the camera-distance heuristic used to pick lower LODs.
    // Values above 1 switch to cheaper LODs sooner; values below 1 keep detail longer.
    float LodUsageScale = 1.0f;
    // -1 = automatic, otherwise force a specific LOD level for debugging.
    int DebugForcedLod = -1;
};

struct NameComponent
{
    std::string Name = "Entity";
};

// Point light component: a positional light that radiates equally in all directions.
// All properties here are artist-facing and saved directly to the level file.
struct PointLightComponent
{
    // Luminous flux (total light output) in lumens.
    // Reference: ~800 lm ≈ a 60W incandescent bulb, ~1600 lm ≈ 100W equivalent.
    float IntensityLumens = 800.0f;

    // Radius of the light's influence sphere in world units.
    // Surfaces beyond this distance receive no contribution from this light.
    float Radius = 5.0f;

    // Linear RGB light color (HDR – values above 1 are valid).
    float ColorR = 1.0f;
    float ColorG = 1.0f;
    float ColorB = 1.0f;

    // Whether this light contributes to shadow maps.
    bool CastShadows = true;

    // Whether this light contributes scattering to volumetric fog.
    bool AffectVolumetricFog = true;

    // Physical source radius in world units; non-zero gives area-light soft shadows.
    float SourceRadius = 0.0f;

    // Attenuation falloff exponent. 2 = physically correct inverse-square,
    // lower values give softer/more stylised falloff.
    float FalloffExponent = 2.0f;

    // When non-zero, overrides the RGB color with a blackbody temperature (Kelvin).
    // Set UseTemperature = false to use the RGB color directly.
    bool UseTemperature = false;
    float TemperatureKelvin = 6500.0f;
};

struct AudioEmitterComponent
{
    std::string EventPath;
    bool AutoPlay = true;
    int RuntimeEmitterHandle = -1;
    bool RuntimeAutoPlayStarted = false;
    std::string RuntimeRegisteredEventPath;
};

// DecalComponent: projects a material onto surfaces within an oriented box volume.
// The decal faces -Z in local space; rotate the entity to control projection direction.
struct DecalComponent
{
    // Relative path (from Data/) of the .json material assigned to this decal.
    // The material must have "isDecalMaterial": true to be eligible.
    std::string MaterialPath;

    // Half-extents of the projection box in world units along each local axis.
    float SizeX = 1.0f;
    float SizeY = 1.0f;
    float SizeZ = 1.0f;
};

// RainComponent: places a camera-relative rain particle simulation at this entity's location.
// All fields here are artist-facing and saved directly to the level file.
struct RainComponent
{
    // Physics
    float WindX           = 0.5f;
    float WindY           = 0.0f;
    float WindZ           = 0.2f;
    float Gravity         = 9.8f;

    // Bounding box half-extents (XYZ) around the camera.
    float BoxExtentX      = 20.0f;
    float BoxExtentY      = 15.0f;
    float BoxExtentZ      = 20.0f;

    // Visual
    float Intensity       = 1.0f;
    float StreakLength     = 0.18f;
    float ColorR           = 0.65f;
    float ColorG           = 0.75f;
    float ColorB           = 0.85f;
    float ColorA           = 0.35f;
    float WetnessIntensity = 0.7f;

    bool  Enabled          = true;
};

struct Entity
{
    std::string Name = "Entity";
    TransformComponent Transform;
    std::optional<MeshComponent> Mesh;
    std::optional<PointLightComponent> PointLight;
    std::optional<AudioEmitterComponent> AudioEmitter;
    std::optional<DecalComponent> Decal;
    std::optional<RainComponent> Rain;

    bool HasMeshComponent() const
    {
        return Mesh.has_value();
    }

    MeshComponent& AddMeshComponent()
    {
        if (!Mesh.has_value())
        {
            Mesh.emplace();
        }

        return *Mesh;
    }

    bool HasNameComponent() const
    {
        return !Name.empty();
    }

    NameComponent GetNameComponent() const
    {
        return NameComponent{ Name };
    }

    void AddNameComponent(const NameComponent& nameComponent)
    {
        // Mirror the serialized NameComponent back into the editor-facing entity name used by the current tools.
        Name = nameComponent.Name;
    }

    bool HasTransformComponent() const
    {
        return true;
    }

    const TransformComponent& GetTransformComponent() const
    {
        return Transform;
    }

    TransformComponent& AddTransformComponent(const TransformComponent& transformComponent)
    {
        Transform = transformComponent;
        return Transform;
    }

    bool HasPointLightComponent() const
    {
        return PointLight.has_value();
    }

    PointLightComponent& AddPointLightComponent()
    {
        if (!PointLight.has_value())
        {
            PointLight.emplace();
        }

        return *PointLight;
    }

    bool HasAudioEmitterComponent() const
    {
        return AudioEmitter.has_value();
    }

    AudioEmitterComponent& AddAudioEmitterComponent()
    {
        if (!AudioEmitter.has_value())
        {
            AudioEmitter.emplace();
        }

        return *AudioEmitter;
    }

    bool HasDecalComponent() const
    {
        return Decal.has_value();
    }

    DecalComponent& AddDecalComponent()
    {
        if (!Decal.has_value())
        {
            Decal.emplace();
        }

        return *Decal;
    }

    bool HasRainComponent() const
    {
        return Rain.has_value();
    }

    RainComponent& AddRainComponent()
    {
        if (!Rain.has_value())
        {
            Rain.emplace();
        }

        return *Rain;
    }
};

namespace DirectX
{
    NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(XMFLOAT3, x, y, z)
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(TransformComponent, Position, Rotation, Scale)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(NameComponent, Name)

// MeshComponent serialization only persists the file paths; the runtime asset handle is transient.
inline void to_json(nlohmann::json& j, const MeshComponent& mc)
{
    j = nlohmann::json{
        { "MeshPath", mc.MeshPath },
        { "MaterialPath", mc.MaterialPath },
        { "LodUsageScale", mc.LodUsageScale },
        { "DebugForcedLod", mc.DebugForcedLod }
    };
}

inline void from_json(const nlohmann::json& j, MeshComponent& mc)
{
    mc.MeshPath    = j.value("MeshPath",    std::string{});
    mc.MaterialPath = j.value("MaterialPath", std::string{});
    mc.LodUsageScale = j.value("LodUsageScale", 1.0f);
    mc.DebugForcedLod = j.value("DebugForcedLod", -1);
}

// PointLightComponent – all fields are plain scalars so a single macro handles both directions.
inline void to_json(nlohmann::json& j, const PointLightComponent& pl)
{
    j = nlohmann::json{
        { "IntensityLumens",   pl.IntensityLumens   },
        { "Radius",            pl.Radius            },
        { "ColorR",            pl.ColorR            },
        { "ColorG",            pl.ColorG            },
        { "ColorB",            pl.ColorB            },
        { "CastShadows",       pl.CastShadows       },
        { "AffectVolumetricFog", pl.AffectVolumetricFog },
        { "SourceRadius",      pl.SourceRadius      },
        { "FalloffExponent",   pl.FalloffExponent   },
        { "UseTemperature",    pl.UseTemperature    },
        { "TemperatureKelvin", pl.TemperatureKelvin }
    };
}

inline void from_json(const nlohmann::json& j, PointLightComponent& pl)
{
    pl.IntensityLumens   = j.value("IntensityLumens",   800.0f);
    pl.Radius            = j.value("Radius",            5.0f);
    pl.ColorR            = j.value("ColorR",            1.0f);
    pl.ColorG            = j.value("ColorG",            1.0f);
    pl.ColorB            = j.value("ColorB",            1.0f);
    pl.CastShadows       = j.value("CastShadows",       true);
    pl.AffectVolumetricFog = j.value("AffectVolumetricFog", true);
    pl.SourceRadius      = j.value("SourceRadius",      0.0f);
    pl.FalloffExponent   = j.value("FalloffExponent",   2.0f);
    pl.UseTemperature    = j.value("UseTemperature",    false);
    pl.TemperatureKelvin = j.value("TemperatureKelvin", 6500.0f);
}

inline void to_json(nlohmann::json& j, const AudioEmitterComponent& ae)
{
    j = nlohmann::json{
        { "EventPath", ae.EventPath },
        { "AutoPlay", ae.AutoPlay }
    };
}

inline void from_json(const nlohmann::json& j, AudioEmitterComponent& ae)
{
    ae.EventPath = j.value("EventPath", std::string{});
    ae.AutoPlay = j.value("AutoPlay", true);
}

inline void to_json(nlohmann::json& j, const DecalComponent& dc)
{
    j = nlohmann::json{
        { "MaterialPath", dc.MaterialPath },
        { "SizeX",        dc.SizeX        },
        { "SizeY",        dc.SizeY        },
        { "SizeZ",        dc.SizeZ        }
    };
}

inline void from_json(const nlohmann::json& j, DecalComponent& dc)
{
    dc.MaterialPath = j.value("MaterialPath", std::string{});
    dc.SizeX        = j.value("SizeX",        1.0f);
    dc.SizeY        = j.value("SizeY",        1.0f);
    dc.SizeZ        = j.value("SizeZ",        1.0f);
}

inline void to_json(nlohmann::json& j, const RainComponent& rc)
{
    j = nlohmann::json{
        { "WindX",            rc.WindX            },
        { "WindY",            rc.WindY            },
        { "WindZ",            rc.WindZ            },
        { "Gravity",          rc.Gravity          },
        { "BoxExtentX",       rc.BoxExtentX       },
        { "BoxExtentY",       rc.BoxExtentY       },
        { "BoxExtentZ",       rc.BoxExtentZ       },
        { "Intensity",        rc.Intensity        },
        { "StreakLength",     rc.StreakLength      },
        { "ColorR",           rc.ColorR           },
        { "ColorG",           rc.ColorG           },
        { "ColorB",           rc.ColorB           },
        { "ColorA",           rc.ColorA           },
        { "WetnessIntensity", rc.WetnessIntensity },
        { "Enabled",          rc.Enabled          }
    };
}

inline void from_json(const nlohmann::json& j, RainComponent& rc)
{
    rc.WindX            = j.value("WindX",            0.5f);
    rc.WindY            = j.value("WindY",            0.0f);
    rc.WindZ            = j.value("WindZ",            0.2f);
    rc.Gravity          = j.value("Gravity",          9.8f);
    rc.BoxExtentX       = j.value("BoxExtentX",       20.0f);
    rc.BoxExtentY       = j.value("BoxExtentY",       15.0f);
    rc.BoxExtentZ       = j.value("BoxExtentZ",       20.0f);
    rc.Intensity        = j.value("Intensity",        1.0f);
    rc.StreakLength      = j.value("StreakLength",     0.18f);
    rc.ColorR           = j.value("ColorR",           0.65f);
    rc.ColorG           = j.value("ColorG",           0.75f);
    rc.ColorB           = j.value("ColorB",           0.85f);
    rc.ColorA           = j.value("ColorA",           0.35f);
    rc.WetnessIntensity = j.value("WetnessIntensity", 0.7f);
    rc.Enabled          = j.value("Enabled",          true);
}
