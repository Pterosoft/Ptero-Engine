#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Watches the project's Data folder for files that appear in it from outside the editor -
// copied in from Explorer, unpacked from a Fab download, saved by another tool - so the
// editor can offer to import them, the way Unreal's content browser does.
//
// Changes are collected on a background thread with ReadDirectoryChangesW. A file is only
// reported once it has settled (no further changes for a moment and readable, so a copy
// in progress is not picked up half-written) and once the classifier says it still needs
// importing.
namespace DataFolderWatcher
{
    struct DetectedFile
    {
        std::filesystem::path Path;
        // What the classifier called it, e.g. "StaticMesh", "Texture2D", "FBX", "Texture".
        std::string Kind;
    };

    // Decides whether a settled file needs importing. Returns false to ignore the file;
    // otherwise fills `kind`. Runs on the watcher thread.
    using Classifier = std::function<bool(const std::filesystem::path& file, std::string& kind)>;

    // Starts watching (again, if the folder changed). Cheap to call every frame.
    void Start(const std::filesystem::path& dataDirectory, Classifier classifier);
    void Stop();

    // Files detected since the last call.
    std::vector<DetectedFile> TakeDetectedFiles();

    // True once after anything under Data changed (for refreshing file listings).
    bool ConsumeChanged();

    // Files the user declined to import; they are not offered again this session.
    void Ignore(const std::vector<std::filesystem::path>& files);
}
