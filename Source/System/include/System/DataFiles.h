#pragma once

// DataFiles - one place to ask "does this asset exist / give me its bytes" that works
// both in the editor, where Data/ is a real folder, and in a packaged game, where it is
// a set of encrypted Content/*.ppak archives that are never unpacked to disk.
//
// A packaged game's exe (GameLauncher) mounts the archives and exports a small C API
// (PteroData_*). Every module that includes this header - Renderer_DX12, Audio, Video -
// finds that API on the process's own exe at first use, so all of them read from the
// one mounted set without any of them owning it. When the exe does not export it (the
// editor, tools), every call here is a plain disk operation.
//
// Paths are the same absolute paths the engine already builds: the game's Data root is
// "<exe dir>\Data" (PackagedRoot()), which simply does not exist on disk. Anything under
// it is looked up in the archives, case-insensitively; anything else goes to disk, so
// writable things (logs, the shader cache, settings) are unaffected.

#include <cstdint>
#include <filesystem>
#include <istream>
#include <memory>
#include <string>
#include <vector>

namespace DataFiles
{
    // True when this process serves Data/ from mounted .ppak archives.
    bool IsPackaged();

    // "<exe dir>\Data" when packaged, empty otherwise.
    std::filesystem::path PackagedRoot();

    // Walks up from the exe looking for a Data folder, the way the editor always has.
    // When packaged, returns PackagedRoot() instead. Empty if neither applies.
    std::filesystem::path FindDataDirectory();

    // For a path under PackagedRoot(): its archive path ("Textures/Foo.dds"). Empty for
    // anything else, including every path when not packaged.
    std::string PackagedRelativePath(const std::filesystem::path& path);

    bool Exists(const std::filesystem::path& path);
    bool IsFile(const std::filesystem::path& path);
    bool IsDirectory(const std::filesystem::path& path);

    bool ReadBytes(const std::filesystem::path& path, std::vector<std::uint8_t>& out);
    bool ReadText(const std::filesystem::path& path, std::string& out);

    // A readable stream over the file: an ifstream on disk, an in-memory copy of the
    // decrypted entry when packaged. Null if the file cannot be opened.
    std::unique_ptr<std::istream> OpenStream(const std::filesystem::path& path);

    // Files (not directories) under `directory`, as full paths in the same form as the
    // argument - virtual ones when packaged. Order is unspecified.
    std::vector<std::filesystem::path> ListFiles(const std::filesystem::path& directory, bool recursive);

    // Drop-in for a read-only std::ifstream on an asset: same constructor shape, same
    // is_open(), and an istream either way - over the file on disk, or over the
    // decrypted bytes of an archive entry.
    class InputFile : public std::istream
    {
    public:
        explicit InputFile(const std::filesystem::path& path, std::ios_base::openmode mode = std::ios_base::in);
        ~InputFile() override;
        InputFile(const InputFile&) = delete;
        InputFile& operator=(const InputFile&) = delete;

        bool is_open() const { return mOpen; }

    private:
        std::unique_ptr<std::streambuf> mBuffer;
        bool mOpen = false;
    };
}
