#include "pch.h"
#include "ParticleEffects.h"

#include "Components.h"
#include "System/DataFiles.h"
#include "..\SDKs\nlohmann\json.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>

namespace ParticleEffects
{
namespace
{
    // Bumped if the layout ever changes in a way old readers would misread. Readers
    // ignore unknown keys and default missing ones, so additions do not need it.
    constexpr int kFileVersion = 1;

    std::filesystem::path DataDirectory()
    {
        return DataFiles::FindDataDirectory();
    }
}

std::string AbsolutePath(const std::string& relativePath)
{
    const std::filesystem::path requested(relativePath);
    if (requested.is_absolute())
        return requested.string();
    const std::filesystem::path data = DataDirectory();
    return (data.empty() ? requested : data / requested).lexically_normal().string();
}

bool Load(const std::string& relativePath, ParticleSystemComponent& effect, std::string* error)
{
    if (relativePath.empty())
    {
        if (error) *error = "No particle effect file given.";
        return false;
    }

    std::string text;
    if (!DataFiles::ReadText(AbsolutePath(relativePath), text))
    {
        if (error) *error = "Could not read " + relativePath + ".";
        return false;
    }

    try
    {
        const nlohmann::json root = nlohmann::json::parse(text);
        if (!root.is_object())
        {
            if (error) *error = relativePath + " is not a particle effect (expected a JSON object).";
            return false;
        }
        // The effect settings sit at the top level of the file, next to the header.
        ParticleSystemComponent loaded = effect;
        ParticleEffectFromJson(root, loaded);
        CopyParticleEffect(loaded, effect);
        return true;
    }
    catch (const std::exception& exception)
    {
        if (error) *error = relativePath + ": " + exception.what();
        return false;
    }
}

bool Save(const std::string& relativePath, const ParticleSystemComponent& effect, std::string* error)
{
    if (DataFiles::IsPackaged())
    {
        if (error) *error = "Particle effects cannot be saved in a packaged game.";
        return false;
    }

    const std::filesystem::path path(AbsolutePath(relativePath));
    std::error_code directoryError;
    std::filesystem::create_directories(path.parent_path(), directoryError);

    nlohmann::json root = ParticleEffectToJson(effect);
    root["Type"] = "ParticleEffect";
    root["Version"] = kFileVersion;
    root["Name"] = path.stem().string();

    // Write to a sibling and swap it in, so a crash mid-write cannot leave a half file
    // that every level using the effect would then fail to load.
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file)
        {
            if (error) *error = "Could not write " + path.string() + ".";
            return false;
        }
        file << root.dump(4) << '\n';
        if (!file)
        {
            if (error) *error = "Could not write " + path.string() + ".";
            return false;
        }
    }
    std::error_code renameError;
    std::filesystem::rename(temporary, path, renameError);
    if (renameError)
    {
        std::filesystem::remove(temporary, renameError);
        if (error) *error = "Could not replace " + path.string() + ".";
        return false;
    }
    return true;
}

std::vector<std::string> List()
{
    std::vector<std::string> result;
    const std::filesystem::path data = DataDirectory();
    if (data.empty())
        return result;

    for (const std::filesystem::path& file : DataFiles::ListFiles(data, true))
    {
        std::string extension = file.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (extension != kExtension)
            continue;
        std::error_code relativeError;
        const std::filesystem::path relative = std::filesystem::relative(file, data, relativeError);
        if (!relativeError && !relative.empty())
            result.push_back(relative.generic_string());
    }
    std::sort(result.begin(), result.end(), [](const std::string& a, const std::string& b) {
        return _stricmp(a.c_str(), b.c_str()) < 0;
    });
    return result;
}

void LoadAll(std::vector<Entity>& entities, std::vector<std::string>* problems)
{
    // One read per file, however many emitters share it - a level of forty torches
    // should not parse the same torch forty times.
    std::map<std::string, std::optional<ParticleSystemComponent>> loaded;
    for (Entity& entity : entities)
    {
        if (!entity.ParticleSystem.has_value() || entity.ParticleSystem->ParticlePath.empty())
            continue;

        ParticleSystemComponent& system = *entity.ParticleSystem;
        auto found = loaded.find(system.ParticlePath);
        if (found == loaded.end())
        {
            ParticleSystemComponent effect;
            std::string error;
            std::optional<ParticleSystemComponent> result;
            if (Load(system.ParticlePath, effect, &error))
                result = effect;
            else if (problems)
                problems->push_back(error + " Emitters using it keep their current settings.");
            found = loaded.emplace(system.ParticlePath, std::move(result)).first;
        }
        if (found->second.has_value())
            CopyParticleEffect(*found->second, system);
    }
}

std::string EnsureDefaultEffect(std::string* error)
{
    if (DataFiles::Exists(AbsolutePath(kDefaultEffect)))
        return kDefaultEffect;
    if (!Save(kDefaultEffect, ParticleSystemComponent{}, error))
        return {};
    return kDefaultEffect;
}
}
