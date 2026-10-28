#pragma once

// Reader for Unreal Engine 5 editor packages (.uasset / .umap as saved by the editor, not
// cooked). Written from the public file layout; it understands just enough of the format to
// pull texture source art and static mesh source geometry out of Fab / Marketplace content:
// the package summary, name/import/export tables, tagged properties and the package trailer
// that holds the (usually Oodle compressed) bulk payloads.

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace Ptero::Unreal
{
    struct Guid
    {
        std::uint32_t A = 0, B = 0, C = 0, D = 0;

        bool operator<(const Guid& other) const
        {
            return std::memcmp(this, &other, sizeof(Guid)) < 0;
        }
        bool operator==(const Guid& other) const
        {
            return std::memcmp(this, &other, sizeof(Guid)) == 0;
        }
    };

    using IoHash = std::array<std::uint8_t, 20>;

    // Little-endian cursor over a byte range. Every read is bounds checked; after the first
    // out-of-range read the reader is marked failed and returns zeros from then on, so parsing
    // code can check Ok() once at the end of a block instead of after every field.
    class ByteReader
    {
    public:
        ByteReader() = default;
        ByteReader(const std::uint8_t* data, std::size_t size) : m_Data(data), m_Size(size) {}

        template <typename T>
        T Read()
        {
            T value{};
            ReadBytes(&value, sizeof(T));
            return value;
        }

        void ReadBytes(void* destination, std::size_t count);
        void Skip(std::int64_t count);
        void Seek(std::size_t position);

        bool ReadBool32() { return Read<std::uint32_t>() != 0; }
        std::string ReadFString();
        Guid ReadGuid() { return Read<Guid>(); }
        IoHash ReadIoHash() { return Read<IoHash>(); }

        std::size_t Tell() const { return m_Position; }
        std::size_t Size() const { return m_Size; }
        std::size_t Remaining() const { return m_Position <= m_Size ? m_Size - m_Position : 0; }
        const std::uint8_t* Data() const { return m_Data; }
        const std::uint8_t* Current() const { return m_Data + m_Position; }
        bool Ok() const { return !m_Failed; }
        void Fail() { m_Failed = true; }

    private:
        const std::uint8_t* m_Data = nullptr;
        std::size_t m_Size = 0;
        std::size_t m_Position = 0;
        bool m_Failed = false;
    };

    struct NameRef
    {
        std::int32_t Index = 0;
        std::int32_t Number = 0;
    };

    struct ObjectImport
    {
        std::string ClassPackage;
        std::string ClassName;
        std::int32_t OuterIndex = 0;
        std::string ObjectName;
        std::string PackageName;
    };

    struct ObjectExport
    {
        std::int32_t ClassIndex = 0;
        std::int32_t SuperIndex = 0;
        std::int32_t TemplateIndex = 0;
        std::int32_t OuterIndex = 0;
        std::string ObjectName;
        std::uint32_t ObjectFlags = 0;
        std::int64_t SerialSize = 0;
        std::int64_t SerialOffset = 0;
        // Tagged-property range relative to SerialOffset; -1 when the package predates it.
        std::int64_t ScriptStart = -1;
        std::int64_t ScriptEnd = -1;
    };

    struct PayloadEntry
    {
        IoHash Id{};
        std::int64_t OffsetInFile = 0;
        std::uint64_t CompressedSize = 0;
        std::uint64_t RawSize = 0;
        std::uint16_t Flags = 0;
        std::uint16_t FilterFlags = 0;
        std::uint8_t AccessMode = 0;   // 0 local, 1 referenced, 2 virtualized
    };

    // Custom version GUIDs the reader branches on.
    namespace CustomVersions
    {
        inline constexpr Guid ReleaseObject{ 0x9C54D522, 0xA8264FBE, 0x94210746, 0x61B482D0 };
        inline constexpr Guid UE5MainStream{ 0x697DD581, 0xE64f41AB, 0xAA4A51EC, 0xBEB7B628 };
        inline constexpr Guid EditorObject{ 0xE4B068ED, 0xF49442E9, 0xA231DA0B, 0x2E46BB41 };
        inline constexpr Guid FortniteMain{ 0x601D1886, 0xAC644F84, 0xAA16D3DE, 0x0DEAC7D6 };
    }

    class Package
    {
    public:
        // headerOnly reads just the tables at the start of the file (enough to list exports
        // and imports); payloads are unavailable then.
        bool Load(const std::filesystem::path& path, std::string& error, bool headerOnly = false);

        // The main asset: the top-level export named like the package.
        const ObjectExport* MainExport() const;

        const std::filesystem::path& Path() const { return m_Path; }
        const std::string& PackageName() const { return m_PackageName; }
        std::int32_t FileVersionUE4() const { return m_FileVersionUE4; }
        std::int32_t FileVersionUE5() const { return m_FileVersionUE5; }
        bool FilterEditorOnly() const { return (m_PackageFlags & 0x80000000u) != 0; }
        std::int32_t CustomVersion(const Guid& key) const;
        const std::map<Guid, std::int32_t>& CustomVersionMap() const { return m_CustomVersions; }

        const std::vector<std::string>& Names() const { return m_Names; }
        const std::vector<ObjectImport>& Imports() const { return m_Imports; }
        const std::vector<ObjectExport>& Exports() const { return m_Exports; }
        const std::vector<PayloadEntry>& Payloads() const { return m_Payloads; }

        // "Name" or "Name_N-1" for a numbered FName, as the engine prints it.
        std::string NameToString(const NameRef& name) const;
        NameRef ReadName(ByteReader& reader) const;
        std::string ReadNameString(ByteReader& reader) const { return NameToString(ReadName(reader)); }

        // FPackageIndex helpers: > 0 is export (index-1), < 0 is import (-index-1), 0 is null.
        std::string ObjectName(std::int32_t packageIndex) const;
        std::string ClassName(std::int32_t packageIndex) const;
        // Full object path of an import, e.g. "/Game/Textures/T_Rock_N.T_Rock_N".
        std::string ObjectPath(std::int32_t packageIndex) const;
        // Package (long package name) an import lives in, e.g. "/Game/Textures/T_Rock_N".
        std::string ImportPackageName(std::int32_t packageIndex) const;

        std::string ExportClassName(const ObjectExport& exportEntry) const { return ClassName(exportEntry.ClassIndex); }
        const ObjectExport* FindExportByClass(const std::string& className) const;
        std::vector<const ObjectExport*> FindExportsByClass(const std::string& className) const;

        // Reader limited to one export's serialized bytes.
        ByteReader ExportReader(const ObjectExport& exportEntry) const;
        // Reader over the tagged properties only (skips the leading serialization-control byte).
        ByteReader ExportPropertyReader(const ObjectExport& exportEntry) const;
        // Offset (relative to the export start) where native serialization begins.
        std::size_t ExportNativeOffset(const ObjectExport& exportEntry) const;

        const PayloadEntry* FindPayload(const IoHash& id) const;
        bool ReadPayload(const IoHash& id, std::vector<std::uint8_t>& out, std::string& error) const;

        const std::vector<std::uint8_t>& Bytes() const { return m_Bytes; }

    private:
        bool ParseSummary(ByteReader& reader, std::string& error);
        bool ParseTrailer(std::string& error);

        std::filesystem::path m_Path;
        std::vector<std::uint8_t> m_Bytes;

        std::int32_t m_LegacyFileVersion = 0;
        std::int32_t m_FileVersionUE4 = 0;
        std::int32_t m_FileVersionUE5 = 0;
        std::map<Guid, std::int32_t> m_CustomVersions;
        std::string m_PackageName;
        std::uint32_t m_PackageFlags = 0;
        std::int32_t m_NameCount = 0, m_NameOffset = 0;
        std::int32_t m_ExportCount = 0, m_ExportOffset = 0;
        std::int32_t m_ImportCount = 0, m_ImportOffset = 0;
        std::int64_t m_BulkDataStartOffset = 0;
        std::int64_t m_PayloadTocOffset = -1;

        std::vector<std::string> m_Names;
        std::vector<ObjectImport> m_Imports;
        std::vector<ObjectExport> m_Exports;

        std::int64_t m_TrailerOffset = -1;
        std::uint32_t m_TrailerHeaderLength = 0;
        std::vector<PayloadEntry> m_Payloads;
    };

    // Decodes an FCompressedBuffer (header + blocks; Oodle, LZ4 or stored).
    bool DecodeCompressedBuffer(const std::uint8_t* data, std::size_t size, std::vector<std::uint8_t>& out, std::string& error);

    // Where an export's FEditorBulkData payload lives.
    struct EditorBulkDataRef
    {
        std::uint32_t Flags = 0;
        IoHash PayloadId{};
        std::int64_t PayloadSize = 0;
        // Absolute file offset of the compressed buffer when the payload is stored inline
        // (packages saved before the payload trailer); -1 when it lives in the trailer.
        std::int64_t InlineOffset = -1;
    };

    // Finds the first editor bulk data serialized in an export's native section.
    bool FindEditorBulkData(const Package& package, const ObjectExport& exportEntry, EditorBulkDataRef& out);
    bool ReadEditorBulkData(const Package& package, const EditorBulkDataRef& bulkData, std::vector<std::uint8_t>& out, std::string& error);

    // ---- Tagged properties -------------------------------------------------------------

    // One node of a property's complete type name, flattened in pre-order: e.g.
    // StructProperty(LinearColor(/Script/CoreUObject)) or ArrayProperty(ObjectProperty(...)).
    struct TypeNode
    {
        std::string Name;
        std::int32_t InnerCount = 0;
    };

    struct PropertyTag
    {
        std::string Name;
        std::vector<TypeNode> Type;     // Type[0].Name is e.g. "IntProperty"
        std::int32_t Size = 0;
        std::int32_t ArrayIndex = 0;
        std::uint8_t Flags = 0;
        bool BoolValue = false;
        std::size_t ValueOffset = 0;    // absolute offset into the reader's data

        const std::string& TypeName() const;
        // First type parameter, e.g. the struct name of a StructProperty or the inner type of
        // an ArrayProperty; empty when the type has none.
        std::string InnerTypeName() const;
        // Type parameters of the first parameter (e.g. an array's struct element name).
        std::string InnerInnerTypeName() const;
        bool HasBinaryOrNativeSerialize() const { return (Flags & 0x08) != 0; }
    };

    // Reads tags until "None", leaving each value unread (ValueOffset/Size record where it is).
    std::vector<PropertyTag> ReadTaggedProperties(ByteReader& reader, const Package& package);

    const PropertyTag* FindProperty(const std::vector<PropertyTag>& tags, const std::string& name, std::int32_t arrayIndex = 0);

    // Value readers; each seeks to the tag's value first.
    std::int64_t PropertyInt(ByteReader& reader, const PropertyTag& tag);
    float PropertyFloat(ByteReader& reader, const PropertyTag& tag);
    bool PropertyBool(ByteReader& reader, const PropertyTag& tag);
    // EnumProperty / enum ByteProperty -> "ESomething::Value" or "Value"; plain ByteProperty -> number.
    std::string PropertyEnum(ByteReader& reader, const PropertyTag& tag, const Package& package);
    std::string PropertyName(ByteReader& reader, const PropertyTag& tag, const Package& package);
    std::string PropertyString(ByteReader& reader, const PropertyTag& tag);
    std::int32_t PropertyObject(ByteReader& reader, const PropertyTag& tag);
    // Nested tagged struct (non-native).
    std::vector<PropertyTag> PropertyStruct(ByteReader& reader, const PropertyTag& tag, const Package& package);
    // ArrayProperty: returns element count and positions the reader on the first element.
    std::int32_t PropertyArrayBegin(ByteReader& reader, const PropertyTag& tag);

    // Strips an "EEnum::" prefix.
    std::string EnumValueName(const std::string& value);
}
