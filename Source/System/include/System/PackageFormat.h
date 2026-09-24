#pragma once

// PackageFormat - the ".ppak" container used by the game Release pipeline.
//
// One .ppak holds one top-level Data/ subfolder (Textures, Geometry, UI, ...): a small
// header, then each entry's own header + relative path + ciphertext back to back. Every
// entry is LZ4-compressed, then sealed with libsodium's crypto_secretbox_easy (a random
// per-entry nonce, authenticated), so a corrupted or tampered .ppak fails to decrypt
// instead of being silently misread. This is asset obfuscation - it stops casual asset
// browsing - not real DRM: the key has to be recoverable by both the engine (to build/
// extract) and the shipped game (to play), so a determined reverse engineer can always
// pull it back out of the binary.
//
// Compiled directly into whichever project needs it (Renderer_DX12 for the editor's
// Release menu, GameLauncher for runtime unpacking), the same way System's CVar.cpp/
// AssetManager.cpp are compiled into more than one project rather than shared as a lib.

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Packaging
{
    // crypto_secretbox_KEYBYTES. Kept as a plain array (rather than pulling sodium.h into
    // every include site) so callers that only pass a key around don't need libsodium's
    // headers.
    using Key = std::array<unsigned char, 32>;

    struct BuildEntry
    {
        std::wstring AbsoluteSourcePath;  // file to read from disk
        std::string RelativePackagePath;  // stored path inside the package, forward-slash
    };

    // Called after each entry is processed; return false to cancel the remaining work.
    using ProgressCallback = std::function<bool(size_t doneCount, size_t totalCount, const std::string& relativePath)>;

    // Builds one .ppak from `entries`, encrypted with `key`. Overwrites `outputPakPath` if
    // it already exists. Returns false and fills outError on the first failure (a source
    // file that can't be read, or a write failure); entries already written are discarded
    // by deleting the partial output file.
    bool BuildPackage(const std::wstring& outputPakPath,
                       const std::vector<BuildEntry>& entries,
                       const Key& key,
                       std::string& outError,
                       const ProgressCallback& onProgress = {});

    // Decrypts and decompresses every entry of `pakPath` into `outputDirectory`, creating
    // subdirectories as needed. Returns false and fills outError on the first failure
    // (wrong key / corrupt file fails authentication, or a write failure).
    bool ExtractPackage(const std::wstring& pakPath,
                         const std::wstring& outputDirectory,
                         const Key& key,
                         std::string& outError,
                         const ProgressCallback& onProgress = {});

    // Random access into one .ppak, which is how a packaged game reads its assets: Open()
    // walks the entry headers once (skipping the payloads) to build an index, and Read()
    // then decrypts and decompresses a single entry straight into the caller's buffer.
    // Nothing is ever written to disk. Read() may be called from several threads at once.
    class PackageArchive
    {
    public:
        PackageArchive();
        ~PackageArchive();
        PackageArchive(const PackageArchive&) = delete;
        PackageArchive& operator=(const PackageArchive&) = delete;

        bool Open(const std::wstring& pakPath, std::string& outError);

        // Stored paths in file order, as BuildPackage wrote them (forward-slash,
        // relative to the packaged folder).
        const std::vector<std::string>& Paths() const;
        std::uint64_t PlaintextSize(size_t index) const;

        // `size` must equal PlaintextSize(index).
        bool Read(size_t index, const Key& key, void* buffer, std::uint64_t size, std::string& outError) const;

    private:
        struct Impl;
        Impl* mImpl;
    };
}
