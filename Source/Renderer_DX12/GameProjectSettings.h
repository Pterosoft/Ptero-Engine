#pragma once

// GameProjectSettings
// -------------------
// The project's Game Settings (Editor: Windows > Game Settings...): which player
// controller runs the player, and for a Node Graph controller, which .nodegraph. The
// character's movement, camera and control values are not here: they belong to the
// controller - the Set Movement/Camera/Control Settings nodes of the graph, or
// ConfigureCharacter() in Game.dll's FirstPersonCharacter.cpp.
//
// Not to be confused with GameSettings.cpp, which holds the *player's* options menu
// (graphics quality, audio volume). These are the designer's, fixed at build time.
//
// Saved as Data/Game/GameSettings.json, so they ship inside the packaged game with the
// rest of Data and a controller graph can sit next to them in Data/Game.

#include <cstdint>
#include <filesystem>
#include <string>

enum class PlayerControllerKind
{
    // FirstPersonCharacter in Game.dll.
    Native,
    // A .nodegraph run by the engine alongside the level graph.
    NodeGraph
};

struct GameProjectSettings
{
    PlayerControllerKind PlayerController = PlayerControllerKind::Native;
    // Data-relative, forward slashes: "Game/Controllers/FirstPersonController.nodegraph".
    std::string ControllerGraph;
    // The level the packaged game opens with, Data-relative: "Levels/House.json". The
    // editor plays whatever level is open and ignores this. GameLauncher reads the key
    // straight out of the file, so its name is part of the launcher's contract too.
    std::string StartupLevel;
    // The game's Steam App ID; 0 = not a Steam build. When set, the packaged game only
    // runs through the Steam client (GameLauncher's SteamSupport.cpp) and Build Game
    // ships steam_api64.dll. Read by GameLauncher under this key name too.
    std::uint32_t SteamAppId = 0;
    // The game's product ID on the Pterosoft store; 0 = not sold through Pterosoft
    // Launcher. When set, the packaged game only runs when started by the launcher, which
    // hands it a one-shot ticket that GameLauncher verifies online before the window
    // opens (GameLauncher's PterosoftGameLicense.h). Read by GameLauncher under this key.
    std::uint32_t PterosoftProductId = 0;

    // Missing file or missing keys keep their defaults, so an old or hand-trimmed file
    // still loads. False only when the file exists but is not valid JSON.
    bool Load(const std::filesystem::path& dataDirectory, std::string* errorMessage = nullptr);
    bool Save(const std::filesystem::path& dataDirectory, std::string* errorMessage = nullptr) const;

    static std::filesystem::path FilePath(const std::filesystem::path& dataDirectory);
    // Where Game Settings suggests saving an exported controller graph.
    static std::filesystem::path DefaultControllerDirectory(const std::filesystem::path& dataDirectory);
};
