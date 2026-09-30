#include "pch.h"

#include "GameProjectSettings.h"

#include "System/DataFiles.h"

#include "..\SDKs\nlohmann\json.hpp"

#include <fstream>

namespace
{
    const char* ControllerKindId(PlayerControllerKind kind)
    {
        return kind == PlayerControllerKind::NodeGraph ? "NodeGraph" : "Native";
    }
}

std::filesystem::path GameProjectSettings::FilePath(const std::filesystem::path& dataDirectory)
{
    return dataDirectory / "Game" / "GameSettings.json";
}

std::filesystem::path GameProjectSettings::DefaultControllerDirectory(const std::filesystem::path& dataDirectory)
{
    return dataDirectory / "Game" / "Controllers";
}

bool GameProjectSettings::Load(const std::filesystem::path& dataDirectory, std::string* errorMessage)
{
    *this = GameProjectSettings{};

    std::string text;
    if (!DataFiles::ReadText(FilePath(dataDirectory), text))
        return true;

    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object())
    {
        if (errorMessage != nullptr)
            *errorMessage = FilePath(dataDirectory).string() + " is not valid JSON; using the defaults.";
        return false;
    }

    PlayerController = root.value("PlayerController", std::string("Native")) == "NodeGraph"
        ? PlayerControllerKind::NodeGraph
        : PlayerControllerKind::Native;
    ControllerGraph = root.value("ControllerGraph", std::string());
    StartupLevel = root.value("StartupLevel", std::string());
    return true;
}

bool GameProjectSettings::Save(const std::filesystem::path& dataDirectory, std::string* errorMessage) const
{
    nlohmann::json root;
    root["PlayerController"] = ControllerKindId(PlayerController);
    root["ControllerGraph"] = ControllerGraph;
    root["StartupLevel"] = StartupLevel;

    const std::filesystem::path path = FilePath(dataDirectory);
    std::error_code ignored;
    std::filesystem::create_directories(path.parent_path(), ignored);

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        if (errorMessage != nullptr)
            *errorMessage = "Could not open " + path.string() + " for writing.";
        return false;
    }

    output << root.dump(4) << "\n";
    if (!output)
    {
        if (errorMessage != nullptr)
            *errorMessage = "Failed while writing " + path.string() + ".";
        return false;
    }

    return true;
}
