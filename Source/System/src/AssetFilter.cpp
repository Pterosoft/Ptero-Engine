#include "System/AssetFilter.h"

#include <algorithm>
#include <cwctype>

namespace Packaging
{
    namespace
    {
        std::wstring ToLower(std::wstring value)
        {
            std::transform(value.begin(), value.end(), value.begin(),
                            [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            return value;
        }

        bool HasExtension(const std::wstring& fileName, const wchar_t* extensionLower)
        {
            const std::wstring lowerName = ToLower(fileName);
            const std::wstring extension(extensionLower);
            if (lowerName.size() < extension.size())
                return false;
            return lowerName.compare(lowerName.size() - extension.size(), extension.size(), extension) == 0;
        }
    }

    bool ShouldIncludeAsset(const std::wstring& topLevelFolderName, const std::wstring& fileName)
    {
        const std::wstring folderLower = ToLower(topLevelFolderName);

        if (folderLower == L"textures")
            return HasExtension(fileName, L".dds");

        // Mesh folders keep their materials (.json) and cooked textures (.dds) beside the
        // cooked mesh - Geometry/MET_Table/M_MET_Table.json points at
        // Geometry/MET_Table/Textures/*.dds - so all three ship. Source art (.fbx, .png,
        // .tga, .spp, ...) stays behind.
        if (folderLower == L"geometry")
            return HasExtension(fileName, L".ptero") || HasExtension(fileName, L".dds") || HasExtension(fileName, L".json");

        // UI, and every other included folder, ship unfiltered.
        return true;
    }
}
