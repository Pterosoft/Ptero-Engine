#pragma once

#include "UnrealPackage.h"

#include <array>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Ptero::Unreal
{
    // A texture as one material input consumes it. Channel is 0-3 when the input reads a
    // single channel (R, G, B, A) and -1 when it uses the colour as a whole.
    struct TextureBinding
    {
        std::string TexturePath;   // object path, e.g. "/Game/T/T_Rock_N.T_Rock_N"
        int Channel = -1;
        std::string ParameterName;
    };

    // What a material (instance) resolves to after walking its parent chain: the textures
    // feeding each material input, keyed by the UE input name ("BaseColor", "Normal",
    // "Roughness", "Metallic", "AmbientOcclusion", "EmissiveColor", "Opacity",
    // "OpacityMask", "Displacement").
    struct ResolvedMaterial
    {
        std::string Name;
        std::map<std::string, TextureBinding> Inputs;
        // Texture parameters that could not be tied to an input (the root material was
        // unavailable); the importer classifies these by name instead.
        std::vector<TextureBinding> LooseTextures;
        std::map<std::string, float> Scalars;
        std::map<std::string, std::array<float, 4>> Vectors;
        std::string BlendMode = "BLEND_Opaque";
        bool TwoSided = false;
    };

    // Loads (and caches) the package that holds an object path; returns nullptr when the
    // package cannot be found.
    using PackageLoader = std::function<const Package*(const std::string& objectPath)>;

    bool ResolveMaterial(const std::string& materialObjectPath, const PackageLoader& loadPackage, ResolvedMaterial& material, std::string& error);
}
