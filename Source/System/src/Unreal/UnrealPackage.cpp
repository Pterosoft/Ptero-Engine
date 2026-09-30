#include "UnrealPackage.h"

#include "UnrealOodle.h"

#include <algorithm>
#include <fstream>

#include "lz4.h"

namespace Ptero::Unreal
{
    namespace
    {
        constexpr std::uint32_t kPackageFileTag = 0x9E2A83C1u;
        constexpr std::uint64_t kTrailerHeaderTag = 0xD1C43B2E80A5F697ull;
        constexpr std::uint64_t kTrailerFooterTag = 0x29BFCA045138DE76ull;
        constexpr std::uint32_t kTrailerFooterSize = 20;
        constexpr std::uint32_t kTrailerEntrySize = 49;

        // EUnrealEngineObjectUE4Version / UE5Version values the layout branches on.
        constexpr std::int32_t VER_UE4_ADDED_PACKAGE_SUMMARY_LOCALIZATION_ID = 516;
        constexpr std::int32_t VER_UE4_NON_OUTER_PACKAGE_IMPORT = 520;
        constexpr std::int32_t VER_UE4_TemplateIndex_IN_COOKED_EXPORTS = 508;
        constexpr std::int32_t VER_UE4_64BIT_EXPORTMAP_SERIALSIZES = 511;
        constexpr std::int32_t VER_UE4_LOAD_FOR_EDITOR_GAME = 365;
        constexpr std::int32_t VER_UE4_COOKED_ASSETS_IN_EDITOR_SUPPORT = 485;
        constexpr std::int32_t VER_UE4_PRELOAD_DEPENDENCIES_IN_COOKED_EXPORTS = 507;
        constexpr std::int32_t VER_UE4_ADD_STRING_ASSET_REFERENCES_MAP = 384;
        constexpr std::int32_t VER_UE4_ADDED_SEARCHABLE_NAMES = 510;
        constexpr std::int32_t VER_UE4_SERIALIZE_TEXT_IN_PACKAGES = 459;
        constexpr std::int32_t VER_UE4_ENGINE_VERSION_OBJECT = 336;
        constexpr std::int32_t VER_UE4_PACKAGE_SUMMARY_HAS_COMPATIBLE_ENGINE_VERSION = 444;
        constexpr std::int32_t VER_UE4_WORLD_LEVEL_INFO = 224;
        constexpr std::int32_t VER_UE4_CHANGED_CHUNKID_TO_BE_AN_ARRAY_OF_CHUNKIDS = 278;
        constexpr std::int32_t VER_UE4_NAME_HASHES_SERIALIZED = 504;

        constexpr std::int32_t UE5_NAMES_REFERENCED_FROM_EXPORT_DATA = 1001;
        constexpr std::int32_t UE5_PAYLOAD_TOC = 1002;
        constexpr std::int32_t UE5_OPTIONAL_RESOURCES = 1003;
        constexpr std::int32_t UE5_REMOVE_OBJECT_EXPORT_PACKAGE_GUID = 1005;
        constexpr std::int32_t UE5_TRACK_OBJECT_EXPORT_IS_INHERITED = 1006;
        constexpr std::int32_t UE5_ADD_SOFTOBJECTPATH_LIST = 1008;
        constexpr std::int32_t UE5_DATA_RESOURCES = 1009;
        constexpr std::int32_t UE5_SCRIPT_SERIALIZATION_OFFSET = 1010;
        constexpr std::int32_t UE5_PROPERTY_TAG_EXTENSION = 1011;
        constexpr std::int32_t UE5_PROPERTY_TAG_COMPLETE_TYPE_NAME = 1012;
        constexpr std::int32_t UE5_METADATA_SERIALIZATION_OFFSET = 1014;
        constexpr std::int32_t UE5_VERSE_CELLS = 1015;
        constexpr std::int32_t UE5_PACKAGE_SAVED_HASH = 1016;
        constexpr std::int32_t UE5_IMPORT_TYPE_HIERARCHIES = 1018;

        std::uint32_t ReadBigEndian32(const std::uint8_t* bytes)
        {
            return (std::uint32_t(bytes[0]) << 24) | (std::uint32_t(bytes[1]) << 16) | (std::uint32_t(bytes[2]) << 8) | std::uint32_t(bytes[3]);
        }

        std::uint64_t ReadBigEndian64(const std::uint8_t* bytes)
        {
            return (std::uint64_t(ReadBigEndian32(bytes)) << 32) | ReadBigEndian32(bytes + 4);
        }
    }

    // ---- ByteReader --------------------------------------------------------------------

    void ByteReader::ReadBytes(void* destination, const std::size_t count)
    {
        if (m_Failed || count > Remaining())
        {
            m_Failed = true;
            std::memset(destination, 0, count);
            return;
        }

        std::memcpy(destination, m_Data + m_Position, count);
        m_Position += count;
    }

    void ByteReader::Skip(const std::int64_t count)
    {
        if (count < 0 || static_cast<std::uint64_t>(count) > Remaining())
        {
            m_Failed = true;
            return;
        }
        m_Position += static_cast<std::size_t>(count);
    }

    void ByteReader::Seek(const std::size_t position)
    {
        if (position > m_Size)
        {
            m_Failed = true;
            return;
        }
        m_Position = position;
    }

    std::string ByteReader::ReadFString()
    {
        const std::int32_t length = Read<std::int32_t>();
        if (length == 0 || m_Failed)
        {
            return {};
        }

        if (length > 0)
        {
            if (static_cast<std::size_t>(length) > Remaining())
            {
                m_Failed = true;
                return {};
            }
            std::string value(reinterpret_cast<const char*>(Current()), static_cast<std::size_t>(length));
            m_Position += static_cast<std::size_t>(length);
            if (!value.empty() && value.back() == '\0')
            {
                value.pop_back();
            }
            return value;
        }

        // Negative length: UTF-16 characters. Asset names are ASCII in practice; anything
        // outside that range is replaced rather than transcoded.
        const std::size_t characterCount = static_cast<std::size_t>(-static_cast<std::int64_t>(length));
        if (characterCount * 2 > Remaining())
        {
            m_Failed = true;
            return {};
        }
        std::string value;
        value.reserve(characterCount);
        for (std::size_t index = 0; index < characterCount; ++index)
        {
            const std::uint16_t character = Read<std::uint16_t>();
            if (character == 0)
            {
                continue;
            }
            value.push_back(character < 0x80 ? static_cast<char>(character) : '_');
        }
        return value;
    }

    // ---- Package -----------------------------------------------------------------------

    bool Package::Load(const std::filesystem::path& path, std::string& error, const bool headerOnly)
    {
        m_Path = path;

        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
        {
            error = "cannot open file";
            return false;
        }
        const std::streamsize size = stream.tellg();
        if (size < 64)
        {
            error = "file is too small to be a package";
            return false;
        }
        // The summary and object tables sit at the start of the file; 4 MB covers them for
        // any realistic asset, and a truncated read is caught by the bounds checks below.
        const std::streamsize readSize = headerOnly ? (std::min<std::streamsize>)(size, 4 << 20) : size;
        m_Bytes.resize(static_cast<std::size_t>(readSize));
        stream.seekg(0);
        stream.read(reinterpret_cast<char*>(m_Bytes.data()), readSize);
        if (!stream)
        {
            error = "read failed";
            return false;
        }

        ByteReader reader(m_Bytes.data(), m_Bytes.size());
        if (!ParseSummary(reader, error))
        {
            return false;
        }

        // Name map.
        reader.Seek(static_cast<std::size_t>(m_NameOffset));
        m_Names.reserve(static_cast<std::size_t>((std::max)(m_NameCount, 0)));
        for (std::int32_t index = 0; index < m_NameCount && reader.Ok(); ++index)
        {
            m_Names.push_back(reader.ReadFString());
            if (m_FileVersionUE4 >= VER_UE4_NAME_HASHES_SERIALIZED)
            {
                reader.Skip(4);
            }
        }

        // Import map.
        reader.Seek(static_cast<std::size_t>(m_ImportOffset));
        for (std::int32_t index = 0; index < m_ImportCount && reader.Ok(); ++index)
        {
            ObjectImport entry;
            entry.ClassPackage = ReadNameString(reader);
            entry.ClassName = ReadNameString(reader);
            entry.OuterIndex = reader.Read<std::int32_t>();
            entry.ObjectName = ReadNameString(reader);
            if (m_FileVersionUE4 >= VER_UE4_NON_OUTER_PACKAGE_IMPORT && !FilterEditorOnly())
            {
                entry.PackageName = ReadNameString(reader);
            }
            if (m_FileVersionUE5 >= UE5_OPTIONAL_RESOURCES)
            {
                reader.Skip(4);
            }
            m_Imports.push_back(std::move(entry));
        }

        // Export map.
        reader.Seek(static_cast<std::size_t>(m_ExportOffset));
        for (std::int32_t index = 0; index < m_ExportCount && reader.Ok(); ++index)
        {
            ObjectExport entry;
            entry.ClassIndex = reader.Read<std::int32_t>();
            entry.SuperIndex = reader.Read<std::int32_t>();
            if (m_FileVersionUE4 >= VER_UE4_TemplateIndex_IN_COOKED_EXPORTS)
            {
                entry.TemplateIndex = reader.Read<std::int32_t>();
            }
            entry.OuterIndex = reader.Read<std::int32_t>();
            entry.ObjectName = ReadNameString(reader);
            entry.ObjectFlags = reader.Read<std::uint32_t>();
            if (m_FileVersionUE4 >= VER_UE4_64BIT_EXPORTMAP_SERIALSIZES)
            {
                entry.SerialSize = reader.Read<std::int64_t>();
                entry.SerialOffset = reader.Read<std::int64_t>();
            }
            else
            {
                entry.SerialSize = reader.Read<std::int32_t>();
                entry.SerialOffset = reader.Read<std::int32_t>();
            }
            reader.Skip(4 * 3);   // bForcedExport, bNotForClient, bNotForServer
            if (m_FileVersionUE5 < UE5_REMOVE_OBJECT_EXPORT_PACKAGE_GUID)
            {
                reader.Skip(16);
            }
            if (m_FileVersionUE5 >= UE5_TRACK_OBJECT_EXPORT_IS_INHERITED)
            {
                reader.Skip(4);
            }
            reader.Skip(4);       // PackageFlags
            if (m_FileVersionUE4 >= VER_UE4_LOAD_FOR_EDITOR_GAME)
            {
                reader.Skip(4);
            }
            if (m_FileVersionUE4 >= VER_UE4_COOKED_ASSETS_IN_EDITOR_SUPPORT)
            {
                reader.Skip(4);
            }
            if (m_FileVersionUE5 >= UE5_OPTIONAL_RESOURCES)
            {
                reader.Skip(4);
            }
            if (m_FileVersionUE4 >= VER_UE4_PRELOAD_DEPENDENCIES_IN_COOKED_EXPORTS)
            {
                reader.Skip(4 * 5);
            }
            if (m_FileVersionUE5 >= UE5_SCRIPT_SERIALIZATION_OFFSET)
            {
                entry.ScriptStart = reader.Read<std::int64_t>();
                entry.ScriptEnd = reader.Read<std::int64_t>();
            }
            m_Exports.push_back(std::move(entry));
        }

        if (!reader.Ok())
        {
            error = "package tables are truncated or use an unsupported layout";
            return false;
        }

        if (headerOnly)
        {
            return true;
        }

        for (const ObjectExport& exportEntry : m_Exports)
        {
            if (exportEntry.SerialOffset < 0 || exportEntry.SerialSize < 0 ||
                static_cast<std::uint64_t>(exportEntry.SerialOffset + exportEntry.SerialSize) > m_Bytes.size())
            {
                error = "export '" + exportEntry.ObjectName + "' lies outside the file";
                return false;
            }
        }

        return ParseTrailer(error);
    }

    bool Package::ParseSummary(ByteReader& reader, std::string& error)
    {
        const std::uint32_t tag = reader.Read<std::uint32_t>();
        if (tag != kPackageFileTag)
        {
            error = "not an Unreal package (bad file tag)";
            return false;
        }

        m_LegacyFileVersion = reader.Read<std::int32_t>();
        if (m_LegacyFileVersion >= 0)
        {
            error = "UE3 packages are not supported";
            return false;
        }
        if (m_LegacyFileVersion < -9)
        {
            error = "package was saved by a newer Unreal Engine than this importer understands (legacy version " +
                std::to_string(m_LegacyFileVersion) + ")";
            return false;
        }

        if (m_LegacyFileVersion != -4)
        {
            reader.Skip(4);   // LegacyUE3Version
        }
        m_FileVersionUE4 = reader.Read<std::int32_t>();
        if (m_LegacyFileVersion <= -8)
        {
            m_FileVersionUE5 = reader.Read<std::int32_t>();
        }
        reader.Skip(4);       // FileVersionLicenseeUE

        if (m_FileVersionUE4 == 0 && m_FileVersionUE5 == 0)
        {
            error = "package is unversioned (cooked); only editor packages can be imported";
            return false;
        }

        if (m_FileVersionUE5 >= UE5_PACKAGE_SAVED_HASH)
        {
            reader.Skip(20);  // SavedHash
            reader.Skip(4);   // TotalHeaderSize
        }

        if (m_LegacyFileVersion <= -2)
        {
            // ECustomVersionSerializationFormat::Optimized: {FGuid, int32} pairs.
            const std::int32_t count = reader.Read<std::int32_t>();
            for (std::int32_t index = 0; index < count && reader.Ok(); ++index)
            {
                const Guid key = reader.ReadGuid();
                m_CustomVersions[key] = reader.Read<std::int32_t>();
            }
        }

        if (m_FileVersionUE5 < UE5_PACKAGE_SAVED_HASH)
        {
            reader.Skip(4);   // TotalHeaderSize
        }

        m_PackageName = reader.ReadFString();
        m_PackageFlags = reader.Read<std::uint32_t>();
        m_NameCount = reader.Read<std::int32_t>();
        m_NameOffset = reader.Read<std::int32_t>();

        if (m_FileVersionUE5 >= UE5_ADD_SOFTOBJECTPATH_LIST)
        {
            reader.Skip(8);
        }
        if (!FilterEditorOnly() && m_FileVersionUE4 >= VER_UE4_ADDED_PACKAGE_SUMMARY_LOCALIZATION_ID)
        {
            reader.ReadFString();
        }
        if (m_FileVersionUE4 >= VER_UE4_SERIALIZE_TEXT_IN_PACKAGES)
        {
            reader.Skip(8);
        }

        m_ExportCount = reader.Read<std::int32_t>();
        m_ExportOffset = reader.Read<std::int32_t>();
        m_ImportCount = reader.Read<std::int32_t>();
        m_ImportOffset = reader.Read<std::int32_t>();

        if (m_FileVersionUE5 >= UE5_VERSE_CELLS)
        {
            reader.Skip(16);
        }
        if (m_FileVersionUE5 >= UE5_METADATA_SERIALIZATION_OFFSET)
        {
            reader.Skip(4);
        }
        reader.Skip(4);       // DependsOffset
        if (m_FileVersionUE4 >= VER_UE4_ADD_STRING_ASSET_REFERENCES_MAP)
        {
            reader.Skip(8);
        }
        if (m_FileVersionUE4 >= VER_UE4_ADDED_SEARCHABLE_NAMES)
        {
            reader.Skip(4);
        }
        reader.Skip(4);       // ThumbnailTableOffset
        if (m_FileVersionUE5 >= UE5_IMPORT_TYPE_HIERARCHIES)
        {
            reader.Skip(8);
        }
        if (m_FileVersionUE5 < UE5_PACKAGE_SAVED_HASH)
        {
            reader.Skip(16);  // Guid
        }
        if (!FilterEditorOnly())
        {
            reader.Skip(16);  // PersistentGuid
        }

        const std::int32_t generationCount = reader.Read<std::int32_t>();
        if (generationCount < 0 || generationCount > 100000)
        {
            error = "corrupt generation table";
            return false;
        }
        reader.Skip(static_cast<std::int64_t>(generationCount) * 8);

        auto skipEngineVersion = [&reader]()
        {
            reader.Skip(2 + 2 + 2 + 4);
            reader.ReadFString();
        };
        if (m_FileVersionUE4 >= VER_UE4_ENGINE_VERSION_OBJECT)
        {
            skipEngineVersion();
        }
        else
        {
            reader.Skip(4);
        }
        if (m_FileVersionUE4 >= VER_UE4_PACKAGE_SUMMARY_HAS_COMPATIBLE_ENGINE_VERSION)
        {
            skipEngineVersion();
        }

        reader.Skip(4);       // CompressionFlags
        const std::int32_t compressedChunkCount = reader.Read<std::int32_t>();
        if (compressedChunkCount != 0)
        {
            error = "package uses legacy whole-file compression, which is not supported";
            return false;
        }
        reader.Skip(4);       // PackageSource

        const std::int32_t additionalPackages = reader.Read<std::int32_t>();
        for (std::int32_t index = 0; index < additionalPackages && reader.Ok(); ++index)
        {
            reader.ReadFString();
        }
        if (m_LegacyFileVersion > -7)
        {
            reader.Skip(4);   // NumTextureAllocations
        }

        reader.Skip(4);       // AssetRegistryDataOffset
        m_BulkDataStartOffset = reader.Read<std::int64_t>();
        if (m_FileVersionUE4 >= VER_UE4_WORLD_LEVEL_INFO)
        {
            reader.Skip(4);
        }
        if (m_FileVersionUE4 >= VER_UE4_CHANGED_CHUNKID_TO_BE_AN_ARRAY_OF_CHUNKIDS)
        {
            const std::int32_t chunkCount = reader.Read<std::int32_t>();
            reader.Skip(static_cast<std::int64_t>((std::max)(chunkCount, 0)) * 4);
        }
        else
        {
            reader.Skip(4);
        }
        if (m_FileVersionUE4 >= VER_UE4_PRELOAD_DEPENDENCIES_IN_COOKED_EXPORTS)
        {
            reader.Skip(8);
        }
        if (m_FileVersionUE5 >= UE5_NAMES_REFERENCED_FROM_EXPORT_DATA)
        {
            reader.Skip(4);
        }
        if (m_FileVersionUE5 >= UE5_PAYLOAD_TOC)
        {
            m_PayloadTocOffset = reader.Read<std::int64_t>();
        }
        if (m_FileVersionUE5 >= UE5_DATA_RESOURCES)
        {
            reader.Skip(4);
        }

        if (!reader.Ok())
        {
            error = "package summary is truncated";
            return false;
        }

        const std::size_t fileSize = m_Bytes.size();
        auto inRange = [fileSize](std::int64_t offset) { return offset >= 0 && static_cast<std::uint64_t>(offset) <= fileSize; };
        if (!inRange(m_NameOffset) || !inRange(m_ImportOffset) || !inRange(m_ExportOffset) ||
            m_NameCount < 0 || m_ImportCount < 0 || m_ExportCount < 0)
        {
            error = "package summary offsets are invalid";
            return false;
        }
        return true;
    }

    bool Package::ParseTrailer(std::string& error)
    {
        // Packages saved before the payload trailer existed store bulk data inline; those
        // are handled by the caller through the legacy bulk data path, so no trailer is fine.
        if (m_PayloadTocOffset < 0 || static_cast<std::uint64_t>(m_PayloadTocOffset) >= m_Bytes.size())
        {
            return true;
        }

        ByteReader reader(m_Bytes.data(), m_Bytes.size());
        reader.Seek(static_cast<std::size_t>(m_PayloadTocOffset));
        const std::uint64_t headerTag = reader.Read<std::uint64_t>();
        if (headerTag != kTrailerHeaderTag)
        {
            return true;
        }
        const std::int32_t version = reader.Read<std::int32_t>();
        m_TrailerHeaderLength = reader.Read<std::uint32_t>();
        reader.Skip(8);   // PayloadsDataLength
        (void)version;

        const std::int32_t payloadCount = reader.Read<std::int32_t>();
        for (std::int32_t index = 0; index < payloadCount && reader.Ok(); ++index)
        {
            const std::size_t entryStart = reader.Tell();
            PayloadEntry entry;
            entry.Id = reader.ReadIoHash();
            entry.OffsetInFile = reader.Read<std::int64_t>();
            entry.CompressedSize = reader.Read<std::uint64_t>();
            entry.RawSize = reader.Read<std::uint64_t>();
            entry.Flags = reader.Read<std::uint16_t>();
            entry.FilterFlags = reader.Read<std::uint16_t>();
            entry.AccessMode = reader.Read<std::uint8_t>();
            reader.Seek(entryStart + kTrailerEntrySize);
            m_Payloads.push_back(entry);
        }

        if (!reader.Ok())
        {
            error = "package trailer is truncated";
            return false;
        }

        // Validate the footer so a misparse is reported instead of producing garbage payloads.
        if (m_Bytes.size() >= kTrailerFooterSize)
        {
            ByteReader footer(m_Bytes.data(), m_Bytes.size());
            footer.Seek(m_Bytes.size() - kTrailerFooterSize);
            if (footer.Read<std::uint64_t>() != kTrailerFooterTag)
            {
                error = "package trailer footer is missing";
                return false;
            }
        }

        m_TrailerOffset = m_PayloadTocOffset;
        return true;
    }

    std::int32_t Package::CustomVersion(const Guid& key) const
    {
        const auto found = m_CustomVersions.find(key);
        return found != m_CustomVersions.end() ? found->second : -1;
    }

    std::string Package::NameToString(const NameRef& name) const
    {
        if (name.Index < 0 || static_cast<std::size_t>(name.Index) >= m_Names.size())
        {
            return {};
        }
        if (name.Number == 0)
        {
            return m_Names[static_cast<std::size_t>(name.Index)];
        }
        return m_Names[static_cast<std::size_t>(name.Index)] + "_" + std::to_string(name.Number - 1);
    }

    NameRef Package::ReadName(ByteReader& reader) const
    {
        NameRef name;
        name.Index = reader.Read<std::int32_t>();
        name.Number = reader.Read<std::int32_t>();
        return name;
    }

    std::string Package::ObjectName(const std::int32_t packageIndex) const
    {
        if (packageIndex > 0 && static_cast<std::size_t>(packageIndex) <= m_Exports.size())
        {
            return m_Exports[static_cast<std::size_t>(packageIndex - 1)].ObjectName;
        }
        if (packageIndex < 0 && static_cast<std::size_t>(-packageIndex) <= m_Imports.size())
        {
            return m_Imports[static_cast<std::size_t>(-packageIndex - 1)].ObjectName;
        }
        return {};
    }

    std::string Package::ClassName(const std::int32_t packageIndex) const
    {
        if (packageIndex < 0 && static_cast<std::size_t>(-packageIndex) <= m_Imports.size())
        {
            // The class of an export is itself an import of class "Class"; its name is the
            // class name we want.
            return m_Imports[static_cast<std::size_t>(-packageIndex - 1)].ObjectName;
        }
        if (packageIndex > 0)
        {
            return ObjectName(packageIndex);
        }
        return {};
    }

    std::string Package::ObjectPath(const std::int32_t packageIndex) const
    {
        if (packageIndex == 0)
        {
            return {};
        }

        std::vector<std::string> parts;
        std::int32_t current = packageIndex;
        for (int guard = 0; current != 0 && guard < 64; ++guard)
        {
            if (current < 0)
            {
                const std::size_t importIndex = static_cast<std::size_t>(-current - 1);
                if (importIndex >= m_Imports.size())
                {
                    break;
                }
                parts.push_back(m_Imports[importIndex].ObjectName);
                current = m_Imports[importIndex].OuterIndex;
            }
            else
            {
                const std::size_t exportIndex = static_cast<std::size_t>(current - 1);
                if (exportIndex >= m_Exports.size())
                {
                    break;
                }
                parts.push_back(m_Exports[exportIndex].ObjectName);
                current = m_Exports[exportIndex].OuterIndex;
                if (current == 0)
                {
                    parts.push_back(m_PackageName);
                }
            }
        }

        std::string path;
        for (auto part = parts.rbegin(); part != parts.rend(); ++part)
        {
            if (path.empty())
            {
                path = *part;
            }
            else
            {
                path += (path.find('.') == std::string::npos ? "." : ":") + *part;
            }
        }
        return path;
    }

    std::string Package::ImportPackageName(const std::int32_t packageIndex) const
    {
        std::int32_t current = packageIndex;
        for (int guard = 0; current < 0 && guard < 64; ++guard)
        {
            const std::size_t importIndex = static_cast<std::size_t>(-current - 1);
            if (importIndex >= m_Imports.size())
            {
                return {};
            }
            if (m_Imports[importIndex].OuterIndex == 0)
            {
                return m_Imports[importIndex].ObjectName;
            }
            current = m_Imports[importIndex].OuterIndex;
        }
        return current > 0 ? m_PackageName : std::string{};
    }

    const ObjectExport* Package::MainExport() const
    {
        const std::string assetName = m_PackageName.substr(m_PackageName.rfind('/') + 1);
        for (const ObjectExport& exportEntry : m_Exports)
        {
            if (exportEntry.OuterIndex == 0 && exportEntry.ObjectName == assetName)
            {
                return &exportEntry;
            }
        }
        for (const ObjectExport& exportEntry : m_Exports)
        {
            if (exportEntry.OuterIndex == 0 && ExportClassName(exportEntry) != "MetaData")
            {
                return &exportEntry;
            }
        }
        return nullptr;
    }

    const ObjectExport* Package::FindExportByClass(const std::string& className) const
    {
        for (const ObjectExport& exportEntry : m_Exports)
        {
            if (ExportClassName(exportEntry) == className)
            {
                return &exportEntry;
            }
        }
        return nullptr;
    }

    std::vector<const ObjectExport*> Package::FindExportsByClass(const std::string& className) const
    {
        std::vector<const ObjectExport*> result;
        for (const ObjectExport& exportEntry : m_Exports)
        {
            if (ExportClassName(exportEntry) == className)
            {
                result.push_back(&exportEntry);
            }
        }
        return result;
    }

    ByteReader Package::ExportReader(const ObjectExport& exportEntry) const
    {
        return ByteReader(m_Bytes.data() + exportEntry.SerialOffset, static_cast<std::size_t>(exportEntry.SerialSize));
    }

    ByteReader Package::ExportPropertyReader(const ObjectExport& exportEntry) const
    {
        ByteReader reader = ExportReader(exportEntry);
        if (exportEntry.ScriptStart >= 0)
        {
            reader.Seek(static_cast<std::size_t>(exportEntry.ScriptStart));
        }
        if (m_FileVersionUE5 >= UE5_PROPERTY_TAG_EXTENSION)
        {
            // EClassSerializationControlExtension; bit 1 carries an extra overridable-operation byte.
            const std::uint8_t control = reader.Read<std::uint8_t>();
            if ((control & 0x02) != 0)
            {
                reader.Skip(1);
            }
        }
        return reader;
    }

    std::size_t Package::ExportNativeOffset(const ObjectExport& exportEntry) const
    {
        if (exportEntry.ScriptEnd >= 0)
        {
            return static_cast<std::size_t>(exportEntry.ScriptEnd);
        }
        ByteReader reader = ExportPropertyReader(exportEntry);
        ReadTaggedProperties(reader, *this);
        return reader.Tell();
    }

    const PayloadEntry* Package::FindPayload(const IoHash& id) const
    {
        for (const PayloadEntry& entry : m_Payloads)
        {
            if (entry.Id == id)
            {
                return &entry;
            }
        }
        return nullptr;
    }

    bool Package::ReadPayload(const IoHash& id, std::vector<std::uint8_t>& out, std::string& error) const
    {
        const PayloadEntry* entry = FindPayload(id);
        if (entry == nullptr)
        {
            error = "payload not found in package trailer";
            return false;
        }
        if (entry->AccessMode != 0)
        {
            error = entry->AccessMode == 2
                ? "payload is virtualized (stored in a remote DDC); open the asset in Unreal and use 'Rehydrate' first"
                : "payload is stored in another file, which is not supported";
            return false;
        }

        // Local payload offsets are relative to the end of the trailer header.
        const std::uint64_t start = static_cast<std::uint64_t>(m_TrailerOffset) + m_TrailerHeaderLength + static_cast<std::uint64_t>(entry->OffsetInFile);
        if (start + entry->CompressedSize > m_Bytes.size())
        {
            error = "payload lies outside the file";
            return false;
        }

        if (!DecodeCompressedBuffer(m_Bytes.data() + start, static_cast<std::size_t>(entry->CompressedSize), out, error))
        {
            return false;
        }
        if (out.size() != entry->RawSize)
        {
            error = "payload decoded to an unexpected size";
            return false;
        }
        return true;
    }

    // ---- FCompressedBuffer -------------------------------------------------------------

    bool DecodeCompressedBuffer(const std::uint8_t* data, const std::size_t size, std::vector<std::uint8_t>& out, std::string& error)
    {
        constexpr std::size_t kHeaderSize = 64;
        if (size < kHeaderSize || ReadBigEndian32(data) != 0xB7756362u)
        {
            error = "payload is not a compressed buffer";
            return false;
        }

        const std::uint8_t method = data[8];
        const std::uint8_t blockSizeExponent = data[11];
        const std::uint32_t blockCount = ReadBigEndian32(data + 12);
        const std::uint64_t totalRawSize = ReadBigEndian64(data + 16);
        const std::uint64_t totalCompressedSize = ReadBigEndian64(data + 24);

        if (totalCompressedSize > size || totalRawSize > (1ull << 34))
        {
            error = "compressed buffer header is inconsistent";
            return false;
        }

        out.resize(static_cast<std::size_t>(totalRawSize));

        if (method == 0)
        {
            if (kHeaderSize + totalRawSize > size)
            {
                error = "stored buffer is truncated";
                return false;
            }
            std::memcpy(out.data(), data + kHeaderSize, static_cast<std::size_t>(totalRawSize));
            return true;
        }

        if (method != 3 && method != 4)
        {
            error = "unknown compression method " + std::to_string(method);
            return false;
        }

        if (method == 3 && !Oodle::IsAvailable())
        {
            error = Oodle::UnavailableReason();
            return false;
        }

        const std::uint64_t blockSize = 1ull << blockSizeExponent;
        const std::uint8_t* blockSizes = data + kHeaderSize;
        std::size_t readOffset = kHeaderSize + static_cast<std::size_t>(blockCount) * 4;
        std::uint64_t writeOffset = 0;

        for (std::uint32_t blockIndex = 0; blockIndex < blockCount; ++blockIndex)
        {
            const std::uint32_t compressedBlockSize = ReadBigEndian32(blockSizes + blockIndex * 4);
            const std::uint64_t rawBlockSize = (std::min)(blockSize, totalRawSize - writeOffset);
            if (readOffset + compressedBlockSize > size || writeOffset + rawBlockSize > totalRawSize)
            {
                error = "compressed block lies outside the buffer";
                return false;
            }

            const std::uint8_t* source = data + readOffset;
            std::uint8_t* destination = out.data() + writeOffset;

            if (compressedBlockSize >= rawBlockSize)
            {
                // Incompressible blocks are stored raw.
                std::memcpy(destination, source, static_cast<std::size_t>(rawBlockSize));
            }
            else if (method == 3)
            {
                if (!Oodle::Decompress(source, compressedBlockSize, destination, static_cast<std::size_t>(rawBlockSize)))
                {
                    error = "Oodle failed to decompress block " + std::to_string(blockIndex);
                    return false;
                }
            }
            else
            {
                const int decoded = LZ4_decompress_safe(
                    reinterpret_cast<const char*>(source), reinterpret_cast<char*>(destination),
                    static_cast<int>(compressedBlockSize), static_cast<int>(rawBlockSize));
                if (decoded != static_cast<int>(rawBlockSize))
                {
                    error = "LZ4 failed to decompress block " + std::to_string(blockIndex);
                    return false;
                }
            }

            readOffset += compressedBlockSize;
            writeOffset += rawBlockSize;
        }

        if (writeOffset != totalRawSize)
        {
            error = "compressed buffer is missing blocks";
            return false;
        }
        return true;
    }

    // ---- FEditorBulkData ---------------------------------------------------------------

    bool FindEditorBulkData(const Package& package, const ObjectExport& exportEntry, EditorBulkDataRef& out)
    {
        constexpr std::uint32_t kIsVirtualized = 1u << 0;
        constexpr std::uint32_t kStoredInPackageTrailer = 1u << 9;
        constexpr std::uint32_t kKnownFlagsMask = (1u << 12) - 1;

        const std::uint8_t* exportData = package.Bytes().data() + exportEntry.SerialOffset;
        const std::size_t exportSize = static_cast<std::size_t>(exportEntry.SerialSize);
        const std::size_t nativeStart = (std::min)(package.ExportNativeOffset(exportEntry), exportSize);

        // Layout: uint32 Flags | FGuid BulkDataId | FIoHash PayloadContentId | int64 PayloadSize
        //         [| int64 OffsetInFile when stored inline].
        // Rather than replaying every class's native Serialize() to reach it, look for the
        // payload hash itself: trailer payload ids are known, and a 20-byte hash cannot occur
        // by accident.
        for (const PayloadEntry& payload : package.Payloads())
        {
            for (std::size_t position = nativeStart + 20; position + 28 <= exportSize; ++position)
            {
                if (std::memcmp(exportData + position, payload.Id.data(), payload.Id.size()) != 0)
                {
                    continue;
                }

                std::memcpy(&out.Flags, exportData + position - 20, 4);
                out.PayloadId = payload.Id;
                std::memcpy(&out.PayloadSize, exportData + position + 20, 8);
                out.InlineOffset = -1;
                return true;
            }
        }

        // Packages without a trailer (early UE 5.0) keep the payload inline. Scan for a
        // header whose offset points at a compressed-buffer magic.
        const std::vector<std::uint8_t>& file = package.Bytes();
        for (std::size_t position = nativeStart; position + 56 <= exportSize; ++position)
        {
            std::uint32_t flags = 0;
            std::memcpy(&flags, exportData + position, 4);
            if ((flags & ~kKnownFlagsMask) != 0 || (flags & (kIsVirtualized | kStoredInPackageTrailer)) != 0)
            {
                continue;
            }

            std::int64_t payloadSize = 0;
            std::int64_t offset = 0;
            std::memcpy(&payloadSize, exportData + position + 40, 8);
            std::memcpy(&offset, exportData + position + 48, 8);
            if (payloadSize <= 0 || offset <= 0 || static_cast<std::uint64_t>(offset) + 64 > file.size())
            {
                continue;
            }

            const std::uint8_t* candidate = file.data() + offset;
            if (candidate[0] == 0xB7 && candidate[1] == 0x75 && candidate[2] == 0x63 && candidate[3] == 0x62)
            {
                out.Flags = flags;
                std::memcpy(out.PayloadId.data(), exportData + position + 20, 20);
                out.PayloadSize = payloadSize;
                out.InlineOffset = offset;
                return true;
            }
        }

        return false;
    }

    bool ReadEditorBulkData(const Package& package, const EditorBulkDataRef& bulkData, std::vector<std::uint8_t>& out, std::string& error)
    {
        if (bulkData.InlineOffset < 0)
        {
            return package.ReadPayload(bulkData.PayloadId, out, error);
        }

        const std::vector<std::uint8_t>& file = package.Bytes();
        const std::size_t offset = static_cast<std::size_t>(bulkData.InlineOffset);
        if (!DecodeCompressedBuffer(file.data() + offset, file.size() - offset, out, error))
        {
            return false;
        }
        if (static_cast<std::int64_t>(out.size()) != bulkData.PayloadSize)
        {
            error = "inline payload decoded to an unexpected size";
            return false;
        }
        return true;
    }

    // ---- Tagged properties -------------------------------------------------------------

    const std::string& PropertyTag::TypeName() const
    {
        static const std::string empty;
        return Type.empty() ? empty : Type.front().Name;
    }

    std::string PropertyTag::InnerTypeName() const
    {
        return Type.size() > 1 && Type.front().InnerCount > 0 ? Type[1].Name : std::string{};
    }

    std::string PropertyTag::InnerInnerTypeName() const
    {
        return Type.size() > 2 && Type[1].InnerCount > 0 ? Type[2].Name : std::string{};
    }

    namespace
    {
        void ReadTypeName(ByteReader& reader, const Package& package, std::vector<TypeNode>& nodes)
        {
            // Pre-order: name, inner count, then that many child nodes.
            std::int32_t remaining = 1;
            for (int guard = 0; remaining > 0 && guard < 256 && reader.Ok(); ++guard)
            {
                TypeNode node;
                node.Name = package.ReadNameString(reader);
                node.InnerCount = reader.Read<std::int32_t>();
                remaining += node.InnerCount - 1;
                nodes.push_back(std::move(node));
            }
        }
    }

    std::vector<PropertyTag> ReadTaggedProperties(ByteReader& reader, const Package& package)
    {
        std::vector<PropertyTag> tags;
        const bool completeTypeName = package.FileVersionUE5() >= UE5_PROPERTY_TAG_COMPLETE_TYPE_NAME;

        for (int guard = 0; guard < 100000 && reader.Ok(); ++guard)
        {
            PropertyTag tag;
            tag.Name = package.ReadNameString(reader);
            if (tag.Name.empty() || tag.Name == "None")
            {
                break;
            }

            if (completeTypeName)
            {
                ReadTypeName(reader, package, tag.Type);
                tag.Size = reader.Read<std::int32_t>();
                tag.Flags = reader.Read<std::uint8_t>();
                if (tag.Flags & 0x01)
                {
                    tag.ArrayIndex = reader.Read<std::int32_t>();
                }
                if (tag.Flags & 0x02)
                {
                    reader.Skip(16);
                }
                if (tag.Flags & 0x04)
                {
                    const std::uint8_t extensions = reader.Read<std::uint8_t>();
                    if (extensions & 0x02)
                    {
                        reader.Skip(1);   // EOverriddenPropertyOperation
                        reader.Skip(1);   // bExperimentalOverridableLogic
                    }
                }
                tag.BoolValue = (tag.Flags & 0x10) != 0;
            }
            else
            {
                // Pre-5.4 layout: type name, size, array index, then per-type extras.
                TypeNode typeNode{ package.ReadNameString(reader), 0 };
                tag.Type.push_back(typeNode);
                tag.Size = reader.Read<std::int32_t>();
                tag.ArrayIndex = reader.Read<std::int32_t>();
                const std::string& type = tag.Type.front().Name;
                if (type == "StructProperty")
                {
                    tag.Type.front().InnerCount = 1;
                    tag.Type.push_back({ package.ReadNameString(reader), 0 });
                    reader.Skip(16);   // StructGuid
                }
                else if (type == "BoolProperty")
                {
                    tag.BoolValue = reader.Read<std::uint8_t>() != 0;
                }
                else if (type == "ByteProperty" || type == "EnumProperty" || type == "ArrayProperty" || type == "SetProperty")
                {
                    tag.Type.front().InnerCount = 1;
                    tag.Type.push_back({ package.ReadNameString(reader), 0 });
                }
                else if (type == "MapProperty")
                {
                    tag.Type.front().InnerCount = 2;
                    tag.Type.push_back({ package.ReadNameString(reader), 0 });
                    tag.Type.push_back({ package.ReadNameString(reader), 0 });
                }
                const std::uint8_t hasGuid = reader.Read<std::uint8_t>();
                if (hasGuid)
                {
                    reader.Skip(16);
                }
                if (package.FileVersionUE5() >= UE5_PROPERTY_TAG_EXTENSION)
                {
                    const std::uint8_t extensions = reader.Read<std::uint8_t>();
                    if (extensions & 0x02)
                    {
                        reader.Skip(2);
                    }
                }
            }

            tag.ValueOffset = reader.Tell();
            if (tag.Size < 0)
            {
                reader.Fail();
                break;
            }
            reader.Skip(tag.Size);
            tags.push_back(std::move(tag));
        }

        return tags;
    }

    const PropertyTag* FindProperty(const std::vector<PropertyTag>& tags, const std::string& name, const std::int32_t arrayIndex)
    {
        for (const PropertyTag& tag : tags)
        {
            if (tag.Name == name && tag.ArrayIndex == arrayIndex)
            {
                return &tag;
            }
        }
        return nullptr;
    }

    std::int64_t PropertyInt(ByteReader& reader, const PropertyTag& tag)
    {
        reader.Seek(tag.ValueOffset);
        switch (tag.Size)
        {
        case 1: return reader.Read<std::int8_t>();
        case 2: return reader.Read<std::int16_t>();
        case 4: return reader.Read<std::int32_t>();
        case 8: return reader.Read<std::int64_t>();
        default: return 0;
        }
    }

    float PropertyFloat(ByteReader& reader, const PropertyTag& tag)
    {
        reader.Seek(tag.ValueOffset);
        if (tag.Size == 8)
        {
            return static_cast<float>(reader.Read<double>());
        }
        return reader.Read<float>();
    }

    bool PropertyBool(ByteReader& reader, const PropertyTag& tag)
    {
        (void)reader;
        return tag.BoolValue;
    }

    std::string PropertyEnum(ByteReader& reader, const PropertyTag& tag, const Package& package)
    {
        reader.Seek(tag.ValueOffset);
        if (tag.Size == 8)
        {
            return package.ReadNameString(reader);
        }
        if (tag.Size == 1)
        {
            return std::to_string(reader.Read<std::uint8_t>());
        }
        return {};
    }

    std::string PropertyName(ByteReader& reader, const PropertyTag& tag, const Package& package)
    {
        reader.Seek(tag.ValueOffset);
        return package.ReadNameString(reader);
    }

    std::string PropertyString(ByteReader& reader, const PropertyTag& tag)
    {
        reader.Seek(tag.ValueOffset);
        return reader.ReadFString();
    }

    std::int32_t PropertyObject(ByteReader& reader, const PropertyTag& tag)
    {
        reader.Seek(tag.ValueOffset);
        return reader.Read<std::int32_t>();
    }

    std::vector<PropertyTag> PropertyStruct(ByteReader& reader, const PropertyTag& tag, const Package& package)
    {
        reader.Seek(tag.ValueOffset);
        return ReadTaggedProperties(reader, package);
    }

    std::int32_t PropertyArrayBegin(ByteReader& reader, const PropertyTag& tag)
    {
        reader.Seek(tag.ValueOffset);
        return reader.Read<std::int32_t>();
    }

    std::string EnumValueName(const std::string& value)
    {
        const std::size_t separator = value.rfind("::");
        return separator == std::string::npos ? value : value.substr(separator + 2);
    }
}
