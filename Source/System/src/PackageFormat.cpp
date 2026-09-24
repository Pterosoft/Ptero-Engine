#include "System/PackageFormat.h"

#ifndef SODIUM_STATIC
#define SODIUM_STATIC
#endif
#include <sodium.h>

#include <lz4.h>

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace Packaging
{
    namespace
    {
        constexpr char kMagic[4] = { 'P', 'T', 'P', 'K' };
        constexpr std::uint32_t kFormatVersion = 1;

#pragma pack(push, 1)
        struct FileHeader
        {
            char Magic[4];
            std::uint32_t Version;
            std::uint32_t EntryCount;
        };

        struct EntryHeader
        {
            std::uint32_t PathLength;
            std::uint64_t PlaintextSize;
            std::uint64_t CompressedSize;   // size of the LZ4 block before encryption; 0 for empty files
            unsigned char Nonce[crypto_secretbox_NONCEBYTES];
            std::uint64_t CiphertextSize;   // 0 for empty files, otherwise CompressedSize + MACBYTES
        };
#pragma pack(pop)

        bool EnsureSodiumInitialized()
        {
            // sodium_init() is safe to call repeatedly and from multiple threads; it only
            // does real work the first time.
            return sodium_init() >= 0;
        }

        void WriteRaw(std::ofstream& stream, const void* data, size_t size)
        {
            stream.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
        }

        bool ReadRaw(std::ifstream& stream, void* data, size_t size)
        {
            stream.read(reinterpret_cast<char*>(data), static_cast<std::streamsize>(size));
            return stream.good() || (stream.eof() && stream.gcount() == static_cast<std::streamsize>(size));
        }
    }

    bool BuildPackage(const std::wstring& outputPakPath,
                       const std::vector<BuildEntry>& entries,
                       const Key& key,
                       std::string& outError,
                       const ProgressCallback& onProgress)
    {
        if (!EnsureSodiumInitialized())
        {
            outError = "libsodium failed to initialize.";
            return false;
        }

        std::filesystem::path outputPath(outputPakPath);
        std::error_code makeDirError;
        std::filesystem::create_directories(outputPath.parent_path(), makeDirError);

        std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            outError = "Could not create package file: " + outputPath.string();
            return false;
        }

        FileHeader fileHeader{};
        std::memcpy(fileHeader.Magic, kMagic, sizeof(kMagic));
        fileHeader.Version = kFormatVersion;
        fileHeader.EntryCount = static_cast<std::uint32_t>(entries.size());
        WriteRaw(out, &fileHeader, sizeof(fileHeader));

        std::vector<char> plaintext;
        std::vector<char> compressed;
        std::vector<unsigned char> ciphertext;

        for (size_t i = 0; i < entries.size(); ++i)
        {
            const BuildEntry& entry = entries[i];

            std::ifstream in(entry.AbsoluteSourcePath, std::ios::binary | std::ios::ate);
            if (!in)
            {
                outError = "Could not read source asset: " + std::filesystem::path(entry.AbsoluteSourcePath).string();
                out.close();
                std::error_code removeError;
                std::filesystem::remove(outputPath, removeError);
                return false;
            }

            const std::streamsize plaintextSize = in.tellg();
            in.seekg(0, std::ios::beg);
            plaintext.resize(static_cast<size_t>(plaintextSize));
            if (plaintextSize > 0 && !ReadRaw(in, plaintext.data(), plaintext.size()))
            {
                outError = "Failed reading source asset: " + std::filesystem::path(entry.AbsoluteSourcePath).string();
                out.close();
                std::error_code removeError;
                std::filesystem::remove(outputPath, removeError);
                return false;
            }

            EntryHeader entryHeader{};
            entryHeader.PathLength = static_cast<std::uint32_t>(entry.RelativePackagePath.size());
            entryHeader.PlaintextSize = static_cast<std::uint64_t>(plaintext.size());

            if (!plaintext.empty())
            {
                compressed.resize(static_cast<size_t>(LZ4_compressBound(static_cast<int>(plaintext.size()))));
                const int compressedBytes = LZ4_compress_default(
                    plaintext.data(), compressed.data(),
                    static_cast<int>(plaintext.size()), static_cast<int>(compressed.size()));
                if (compressedBytes <= 0)
                {
                    outError = "LZ4 compression failed for: " + entry.RelativePackagePath;
                    out.close();
                    std::error_code removeError;
                    std::filesystem::remove(outputPath, removeError);
                    return false;
                }
                compressed.resize(static_cast<size_t>(compressedBytes));

                randombytes_buf(entryHeader.Nonce, sizeof(entryHeader.Nonce));
                ciphertext.resize(compressed.size() + crypto_secretbox_MACBYTES);
                crypto_secretbox_easy(ciphertext.data(),
                                       reinterpret_cast<const unsigned char*>(compressed.data()), compressed.size(),
                                       entryHeader.Nonce, key.data());

                entryHeader.CompressedSize = compressed.size();
                entryHeader.CiphertextSize = ciphertext.size();
            }
            else
            {
                entryHeader.CompressedSize = 0;
                entryHeader.CiphertextSize = 0;
                ciphertext.clear();
            }

            WriteRaw(out, &entryHeader, sizeof(entryHeader));
            WriteRaw(out, entry.RelativePackagePath.data(), entry.RelativePackagePath.size());
            if (!ciphertext.empty())
                WriteRaw(out, ciphertext.data(), ciphertext.size());

            if (onProgress && !onProgress(i + 1, entries.size(), entry.RelativePackagePath))
            {
                outError = "Cancelled.";
                out.close();
                std::error_code removeError;
                std::filesystem::remove(outputPath, removeError);
                return false;
            }
        }

        out.close();
        if (!out)
        {
            outError = "Failed writing package file: " + outputPath.string();
            return false;
        }
        return true;
    }

    bool ExtractPackage(const std::wstring& pakPath,
                         const std::wstring& outputDirectory,
                         const Key& key,
                         std::string& outError,
                         const ProgressCallback& onProgress)
    {
        if (!EnsureSodiumInitialized())
        {
            outError = "libsodium failed to initialize.";
            return false;
        }

        std::ifstream in(pakPath, std::ios::binary);
        if (!in)
        {
            outError = "Could not open package file: " + std::filesystem::path(pakPath).string();
            return false;
        }

        FileHeader fileHeader{};
        if (!ReadRaw(in, &fileHeader, sizeof(fileHeader)) ||
            std::memcmp(fileHeader.Magic, kMagic, sizeof(kMagic)) != 0)
        {
            outError = "Not a valid .ppak file: " + std::filesystem::path(pakPath).string();
            return false;
        }
        if (fileHeader.Version != kFormatVersion)
        {
            outError = "Unsupported .ppak version.";
            return false;
        }

        std::error_code makeDirError;
        std::filesystem::create_directories(outputDirectory, makeDirError);

        std::string relativePath;
        std::vector<unsigned char> ciphertext;
        std::vector<char> compressed;
        std::vector<char> plaintext;

        for (std::uint32_t i = 0; i < fileHeader.EntryCount; ++i)
        {
            EntryHeader entryHeader{};
            if (!ReadRaw(in, &entryHeader, sizeof(entryHeader)))
            {
                outError = "Truncated .ppak file (entry header).";
                return false;
            }

            relativePath.resize(entryHeader.PathLength);
            if (entryHeader.PathLength > 0 && !ReadRaw(in, relativePath.data(), relativePath.size()))
            {
                outError = "Truncated .ppak file (entry path).";
                return false;
            }

            plaintext.resize(static_cast<size_t>(entryHeader.PlaintextSize));

            if (entryHeader.CiphertextSize > 0)
            {
                ciphertext.resize(static_cast<size_t>(entryHeader.CiphertextSize));
                if (!ReadRaw(in, ciphertext.data(), ciphertext.size()))
                {
                    outError = "Truncated .ppak file (entry payload): " + relativePath;
                    return false;
                }

                compressed.resize(static_cast<size_t>(entryHeader.CompressedSize));
                if (crypto_secretbox_open_easy(reinterpret_cast<unsigned char*>(compressed.data()),
                                                ciphertext.data(), ciphertext.size(),
                                                entryHeader.Nonce, key.data()) != 0)
                {
                    outError = "Decryption failed for '" + relativePath +
                        "' - wrong key, or the package is corrupt/tampered.";
                    return false;
                }

                const int decompressedBytes = LZ4_decompress_safe(
                    compressed.data(), plaintext.data(),
                    static_cast<int>(compressed.size()), static_cast<int>(plaintext.size()));
                if (decompressedBytes < 0 || static_cast<std::uint64_t>(decompressedBytes) != entryHeader.PlaintextSize)
                {
                    outError = "Decompression failed for: " + relativePath;
                    return false;
                }
            }

            std::filesystem::path destination = std::filesystem::path(outputDirectory) / std::filesystem::path(relativePath);
            std::filesystem::create_directories(destination.parent_path(), makeDirError);

            std::ofstream file(destination, std::ios::binary | std::ios::trunc);
            if (!file)
            {
                outError = "Could not create extracted file: " + destination.string();
                return false;
            }
            if (!plaintext.empty())
                WriteRaw(file, plaintext.data(), plaintext.size());
            file.close();
            if (!file)
            {
                outError = "Failed writing extracted file: " + destination.string();
                return false;
            }

            if (onProgress && !onProgress(i + 1, fileHeader.EntryCount, relativePath))
            {
                outError = "Cancelled.";
                return false;
            }
        }

        return true;
    }

    struct PackageArchive::Impl
    {
        struct IndexedEntry
        {
            EntryHeader Header{};
            std::uint64_t PayloadOffset = 0;
        };

        HANDLE File = INVALID_HANDLE_VALUE;
        std::vector<std::string> Paths;
        std::vector<IndexedEntry> Entries;

        // Positional read: no shared file pointer, so concurrent Read() calls cannot
        // interleave each other's seeks.
        bool ReadAt(std::uint64_t offset, void* data, std::uint64_t size) const
        {
            auto* cursor = static_cast<unsigned char*>(data);
            while (size > 0)
            {
                const DWORD chunk = static_cast<DWORD>((std::min<std::uint64_t>)(size, 64ull * 1024 * 1024));
                OVERLAPPED overlapped{};
                overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
                overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32);
                DWORD bytesRead = 0;
                if (!::ReadFile(File, cursor, chunk, &bytesRead, &overlapped) || bytesRead != chunk)
                    return false;
                cursor += chunk;
                offset += chunk;
                size -= chunk;
            }
            return true;
        }
    };

    PackageArchive::PackageArchive() : mImpl(new Impl) {}

    PackageArchive::~PackageArchive()
    {
        if (mImpl->File != INVALID_HANDLE_VALUE)
            ::CloseHandle(mImpl->File);
        delete mImpl;
    }

    bool PackageArchive::Open(const std::wstring& pakPath, std::string& outError)
    {
        if (!EnsureSodiumInitialized())
        {
            outError = "libsodium failed to initialize.";
            return false;
        }

        mImpl->File = ::CreateFileW(pakPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (mImpl->File == INVALID_HANDLE_VALUE)
        {
            outError = "Could not open package file: " + std::filesystem::path(pakPath).string();
            return false;
        }

        LARGE_INTEGER fileSize{};
        ::GetFileSizeEx(mImpl->File, &fileSize);
        const std::uint64_t totalSize = static_cast<std::uint64_t>(fileSize.QuadPart);

        FileHeader fileHeader{};
        if (!mImpl->ReadAt(0, &fileHeader, sizeof(fileHeader)) ||
            std::memcmp(fileHeader.Magic, kMagic, sizeof(kMagic)) != 0)
        {
            outError = "Not a valid .ppak file: " + std::filesystem::path(pakPath).string();
            return false;
        }
        if (fileHeader.Version != kFormatVersion)
        {
            outError = "Unsupported .ppak version.";
            return false;
        }

        std::uint64_t offset = sizeof(fileHeader);
        mImpl->Paths.reserve(fileHeader.EntryCount);
        mImpl->Entries.reserve(fileHeader.EntryCount);
        for (std::uint32_t i = 0; i < fileHeader.EntryCount; ++i)
        {
            Impl::IndexedEntry entry;
            if (!mImpl->ReadAt(offset, &entry.Header, sizeof(entry.Header)))
            {
                outError = "Truncated .ppak file (entry header).";
                return false;
            }
            offset += sizeof(entry.Header);

            std::string path(entry.Header.PathLength, '\0');
            if (!path.empty() && !mImpl->ReadAt(offset, path.data(), path.size()))
            {
                outError = "Truncated .ppak file (entry path).";
                return false;
            }
            offset += path.size();

            entry.PayloadOffset = offset;
            offset += entry.Header.CiphertextSize;
            if (offset > totalSize)
            {
                outError = "Truncated .ppak file (entry payload): " + path;
                return false;
            }

            mImpl->Paths.push_back(std::move(path));
            mImpl->Entries.push_back(entry);
        }
        return true;
    }

    const std::vector<std::string>& PackageArchive::Paths() const
    {
        return mImpl->Paths;
    }

    std::uint64_t PackageArchive::PlaintextSize(size_t index) const
    {
        return index < mImpl->Entries.size() ? mImpl->Entries[index].Header.PlaintextSize : 0;
    }

    bool PackageArchive::Read(size_t index, const Key& key, void* buffer, std::uint64_t size, std::string& outError) const
    {
        if (index >= mImpl->Entries.size())
        {
            outError = "No such package entry.";
            return false;
        }

        const Impl::IndexedEntry& entry = mImpl->Entries[index];
        if (size != entry.Header.PlaintextSize)
        {
            outError = "Buffer size does not match the entry: " + mImpl->Paths[index];
            return false;
        }
        if (entry.Header.CiphertextSize == 0)
            return true;

        std::vector<unsigned char> ciphertext(static_cast<size_t>(entry.Header.CiphertextSize));
        if (!mImpl->ReadAt(entry.PayloadOffset, ciphertext.data(), ciphertext.size()))
        {
            outError = "Could not read package entry: " + mImpl->Paths[index];
            return false;
        }

        std::vector<char> compressed(static_cast<size_t>(entry.Header.CompressedSize));
        if (crypto_secretbox_open_easy(reinterpret_cast<unsigned char*>(compressed.data()),
                                        ciphertext.data(), ciphertext.size(),
                                        entry.Header.Nonce, key.data()) != 0)
        {
            outError = "Decryption failed for '" + mImpl->Paths[index] +
                "' - wrong key, or the package is corrupt/tampered.";
            return false;
        }

        const int decompressedBytes = LZ4_decompress_safe(
            compressed.data(), static_cast<char*>(buffer),
            static_cast<int>(compressed.size()), static_cast<int>(size));
        if (decompressedBytes < 0 || static_cast<std::uint64_t>(decompressedBytes) != size)
        {
            outError = "Decompression failed for: " + mImpl->Paths[index];
            return false;
        }
        return true;
    }
}
