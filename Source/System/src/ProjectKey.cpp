#include "System/ProjectKey.h"

#ifndef SODIUM_STATIC
#define SODIUM_STATIC
#endif
#include <sodium.h>

#include <filesystem>
#include <fstream>

namespace Packaging
{
    bool LoadOrCreateProjectKey(const std::wstring& projectRoot, Key& outKey, std::string& outError)
    {
        if (sodium_init() < 0)
        {
            outError = "libsodium failed to initialize.";
            return false;
        }

        const std::filesystem::path keyPath = std::filesystem::path(projectRoot) / L"ProjectSettings" / L"Packaging.key";

        if (std::filesystem::exists(keyPath))
        {
            std::ifstream in(keyPath, std::ios::binary);
            if (!in)
            {
                outError = "Could not open " + keyPath.string();
                return false;
            }
            in.read(reinterpret_cast<char*>(outKey.data()), static_cast<std::streamsize>(outKey.size()));
            if (!in || in.gcount() != static_cast<std::streamsize>(outKey.size()))
            {
                outError = keyPath.string() + " is not a valid packaging key (expected " +
                    std::to_string(outKey.size()) + " bytes).";
                return false;
            }
            return true;
        }

        randombytes_buf(outKey.data(), outKey.size());

        std::error_code makeDirError;
        std::filesystem::create_directories(keyPath.parent_path(), makeDirError);

        std::ofstream out(keyPath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            outError = "Could not create " + keyPath.string();
            return false;
        }
        out.write(reinterpret_cast<const char*>(outKey.data()), static_cast<std::streamsize>(outKey.size()));
        out.close();
        if (!out)
        {
            outError = "Failed writing " + keyPath.string();
            return false;
        }
        return true;
    }
}
