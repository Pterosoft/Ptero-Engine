#include "UnrealStaticMesh.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

namespace Ptero::Unreal
{
    namespace
    {
        // FMeshDescription attribute value types, in the order of the AttributeTypes tuple.
        enum class AttributeType : std::int32_t
        {
            Vector4f = 0,
            Vector3f,
            Vector2f,
            Float,
            Int32,
            Bool,
            Name,
            Transform,
        };

        constexpr std::int32_t kUE5MainMeshDescriptionNewFormat = 4;
        constexpr std::int32_t kFortniteMainTriangleLabels = 257;
        constexpr std::size_t kTransformSize = 80;   // FQuat + 2 x FVector, doubles

        struct AttributeChannel
        {
            std::uint32_t Extent = 1;
            std::int32_t ElementSize = 0;
            const std::uint8_t* Data = nullptr;   // points into the payload
            std::int32_t Count = 0;               // number of values (elements * extent)
        };

        struct Attribute
        {
            AttributeType Type = AttributeType::Float;
            std::uint32_t Extent = 1;
            std::vector<AttributeChannel> Channels;
            std::vector<std::vector<std::string>> Names;   // per channel, for Name attributes
        };

        struct ElementContainer
        {
            std::vector<bool> Allocated;
            std::int32_t NumElements = 0;
            std::map<std::string, Attribute> Attributes;

            const Attribute* Find(const std::string& name) const
            {
                const auto found = Attributes.find(name);
                return found != Attributes.end() ? &found->second : nullptr;
            }
        };

        // Each element type (Vertices, VertexInstances, UVs, ...) holds one container per
        // channel; only UVs uses more than one.
        using MeshDescription = std::map<std::string, std::vector<ElementContainer>>;

        // The payload serializes FNames as strings (it is written through a name-as-string
        // proxy archive).
        std::string ReadNameString(ByteReader& reader)
        {
            return reader.ReadFString();
        }

        bool ReadNameArray(ByteReader& reader, const bool compact, std::vector<std::string>& names)
        {
            if (!compact)
            {
                const std::int32_t count = reader.Read<std::int32_t>();
                if (count < 0 || static_cast<std::size_t>(count) > reader.Remaining())
                {
                    return false;
                }
                names.reserve(static_cast<std::size_t>(count));
                for (std::int32_t index = 0; index < count && reader.Ok(); ++index)
                {
                    names.push_back(ReadNameString(reader));
                }
                return reader.Ok();
            }

            const std::uint32_t count = reader.Read<std::uint32_t>();
            if (count == 0)
            {
                return reader.Ok();
            }
            const std::uint32_t uniqueCount = reader.Read<std::uint32_t>();
            if (uniqueCount > count || uniqueCount > reader.Remaining())
            {
                return false;
            }
            std::vector<std::string> unique;
            unique.reserve(uniqueCount);
            for (std::uint32_t index = 0; index < uniqueCount && reader.Ok(); ++index)
            {
                unique.push_back(ReadNameString(reader));
            }
            if (uniqueCount == 1)
            {
                names.assign(count, unique.front());
                return reader.Ok();
            }
            if (uniqueCount == count)
            {
                names = std::move(unique);
                return reader.Ok();
            }

            const std::uint8_t indexSize = reader.Read<std::uint8_t>();
            names.resize(count);
            for (std::uint32_t index = 0; index < count && reader.Ok(); ++index)
            {
                std::uint32_t nameIndex = 0;
                if (indexSize == 0) nameIndex = reader.Read<std::uint8_t>();
                else if (indexSize == 1) nameIndex = reader.Read<std::uint16_t>();
                else nameIndex = reader.Read<std::uint32_t>();
                if (nameIndex < unique.size())
                {
                    names[index] = unique[nameIndex];
                }
            }
            return reader.Ok();
        }

        std::size_t AttributeValueSize(const AttributeType type)
        {
            switch (type)
            {
            case AttributeType::Vector4f: return 16;
            case AttributeType::Vector3f: return 12;
            case AttributeType::Vector2f: return 8;
            case AttributeType::Float: return 4;
            case AttributeType::Int32: return 4;
            case AttributeType::Bool: return 4;    // FArchive serializes bool as uint32
            default: return 0;
            }
        }

        bool ReadMeshDescription(ByteReader& reader, const bool compactNames, MeshDescription& description, std::string& error)
        {
            const std::int32_t elementTypeCount = reader.Read<std::int32_t>();
            if (elementTypeCount <= 0 || elementTypeCount > 64)
            {
                error = "mesh description has an unexpected layout";
                return false;
            }

            for (std::int32_t typeIndex = 0; typeIndex < elementTypeCount && reader.Ok(); ++typeIndex)
            {
                const std::string elementName = ReadNameString(reader);
                const std::int32_t channelCount = reader.Read<std::int32_t>();
                if (channelCount < 0 || channelCount > 64)
                {
                    error = "mesh description element '" + elementName + "' is corrupt";
                    return false;
                }

                std::vector<ElementContainer>& containers = description[elementName];
                for (std::int32_t channel = 0; channel < channelCount && reader.Ok(); ++channel)
                {
                    ElementContainer container;

                    // TBitArray of allocated element indices.
                    const std::int32_t bitCount = reader.Read<std::int32_t>();
                    if (bitCount < 0 || static_cast<std::size_t>(bitCount) / 8 > reader.Remaining())
                    {
                        error = "mesh description bit array is corrupt";
                        return false;
                    }
                    container.Allocated.resize(static_cast<std::size_t>(bitCount));
                    const std::int32_t wordCount = (bitCount + 31) / 32;
                    for (std::int32_t word = 0; word < wordCount; ++word)
                    {
                        const std::uint32_t bits = reader.Read<std::uint32_t>();
                        for (int bit = 0; bit < 32; ++bit)
                        {
                            const std::int32_t index = word * 32 + bit;
                            if (index < bitCount)
                            {
                                container.Allocated[static_cast<std::size_t>(index)] = ((bits >> bit) & 1u) != 0;
                            }
                        }
                    }

                    reader.Skip(4);   // NumHoles
                    container.NumElements = reader.Read<std::int32_t>();
                    const std::int32_t attributeCount = reader.Read<std::int32_t>();
                    if (attributeCount < 0 || attributeCount > 256)
                    {
                        error = "mesh description attribute table is corrupt";
                        return false;
                    }

                    for (std::int32_t attributeIndex = 0; attributeIndex < attributeCount && reader.Ok(); ++attributeIndex)
                    {
                        const std::string attributeName = ReadNameString(reader);
                        Attribute attribute;
                        attribute.Type = static_cast<AttributeType>(reader.Read<std::int32_t>());
                        attribute.Extent = reader.Read<std::uint32_t>();
                        reader.Skip(4);   // NumElements
                        const std::int32_t attributeChannels = reader.Read<std::int32_t>();
                        if (attributeChannels < 0 || attributeChannels > 64)
                        {
                            error = "mesh attribute '" + attributeName + "' is corrupt";
                            return false;
                        }

                        for (std::int32_t index = 0; index < attributeChannels && reader.Ok(); ++index)
                        {
                            AttributeChannel values;
                            values.Extent = reader.Read<std::uint32_t>();
                            if (attribute.Type == AttributeType::Name)
                            {
                                std::vector<std::string> names;
                                if (!ReadNameArray(reader, compactNames, names))
                                {
                                    error = "mesh attribute '" + attributeName + "' has a corrupt name table";
                                    return false;
                                }
                                attribute.Names.push_back(std::move(names));
                            }
                            else if (attribute.Type == AttributeType::Transform)
                            {
                                const std::int32_t count = reader.Read<std::int32_t>();
                                reader.Skip(static_cast<std::int64_t>((std::max)(count, 0)) * kTransformSize);
                            }
                            else
                            {
                                // TArray::BulkSerialize: element size, count, raw data.
                                values.ElementSize = reader.Read<std::int32_t>();
                                values.Count = reader.Read<std::int32_t>();
                                if (values.ElementSize <= 0 || values.Count < 0 ||
                                    static_cast<std::uint64_t>(values.ElementSize) * static_cast<std::uint64_t>(values.Count) > reader.Remaining())
                                {
                                    error = "mesh attribute '" + attributeName + "' is truncated";
                                    return false;
                                }
                                values.Data = reader.Current();
                                reader.Skip(static_cast<std::int64_t>(values.ElementSize) * values.Count);
                            }
                            attribute.Channels.push_back(values);
                        }

                        // Default value, then flags.
                        if (attribute.Type == AttributeType::Name)
                        {
                            ReadNameString(reader);
                        }
                        else if (attribute.Type == AttributeType::Transform)
                        {
                            reader.Skip(kTransformSize);
                        }
                        else
                        {
                            const std::size_t valueSize = AttributeValueSize(attribute.Type);
                            if (valueSize == 0)
                            {
                                error = "mesh attribute '" + attributeName + "' has an unknown type";
                                return false;
                            }
                            reader.Skip(static_cast<std::int64_t>(valueSize));
                        }
                        reader.Skip(4);   // EMeshAttributeFlags

                        container.Attributes.emplace(attributeName, std::move(attribute));
                    }

                    containers.push_back(std::move(container));
                }
            }

            if (!reader.Ok())
            {
                error = "mesh description is truncated";
                return false;
            }
            return true;
        }

        template <typename T>
        T AttributeValue(const Attribute* attribute, const std::size_t valueIndex, const T& fallback, const std::size_t channel = 0)
        {
            if (attribute == nullptr || channel >= attribute->Channels.size())
            {
                return fallback;
            }
            const AttributeChannel& values = attribute->Channels[channel];
            if (values.Data == nullptr || valueIndex >= static_cast<std::size_t>(values.Count) || values.ElementSize < static_cast<std::int32_t>(sizeof(T)))
            {
                return fallback;
            }
            T value{};
            std::memcpy(&value, values.Data + valueIndex * static_cast<std::size_t>(values.ElementSize), sizeof(T));
            return value;
        }

        struct Float3 { float X, Y, Z; };
        struct Float2 { float X, Y; };

        // UE: centimetres, Z up, left-handed with +X forward. Ptero: metres, Z up,
        // left-handed, models facing -Y. The two differ by a half turn about Z, so the
        // conversion keeps handedness; verified against the FBX importer's output for the
        // same source files.
        DirectX::XMFLOAT3 ConvertPosition(const Float3& p)
        {
            return DirectX::XMFLOAT3(-p.X * 0.01f, -p.Y * 0.01f, p.Z * 0.01f);
        }

        DirectX::XMFLOAT3 ConvertDirection(const Float3& n)
        {
            return DirectX::XMFLOAT3(-n.X, -n.Y, n.Z);
        }

        struct VertexKey
        {
            Vertex Value;
            bool operator==(const VertexKey& other) const { return std::memcmp(&Value, &other.Value, sizeof(Vertex)) == 0; }
        };

        struct VertexKeyHash
        {
            std::size_t operator()(const VertexKey& key) const noexcept
            {
                const unsigned char* bytes = reinterpret_cast<const unsigned char*>(&key.Value);
                std::size_t hash = 1469598103934665603ull;
                for (std::size_t index = 0; index < sizeof(Vertex); ++index)
                {
                    hash ^= bytes[index];
                    hash *= 1099511628211ull;
                }
                return hash;
            }
        };

        struct MaterialSlotInfo
        {
            std::string SlotName;
            std::string ImportedSlotName;
            std::int32_t MaterialIndex = 0;
        };

        // Reads UStaticMesh's StaticMaterials, SourceModels[0] bulk data object and the LOD 0
        // entries of SectionInfoMap.
        void ReadStaticMeshProperties(
            const Package& package,
            const ObjectExport& meshExport,
            std::vector<MaterialSlotInfo>& slots,
            std::int32_t& lod0BulkDataObject,
            std::map<std::uint32_t, std::int32_t>& sectionMaterials)
        {
            ByteReader reader = package.ExportPropertyReader(meshExport);
            const std::vector<PropertyTag> tags = ReadTaggedProperties(reader, package);

            if (const PropertyTag* materials = FindProperty(tags, "StaticMaterials"))
            {
                const std::int32_t count = PropertyArrayBegin(reader, *materials);
                for (std::int32_t index = 0; index < count && reader.Ok(); ++index)
                {
                    const std::vector<PropertyTag> element = ReadTaggedProperties(reader, package);
                    const std::size_t next = reader.Tell();
                    MaterialSlotInfo slot;
                    if (const PropertyTag* tag = FindProperty(element, "MaterialInterface")) slot.MaterialIndex = PropertyObject(reader, *tag);
                    if (const PropertyTag* tag = FindProperty(element, "MaterialSlotName")) slot.SlotName = PropertyName(reader, *tag, package);
                    if (const PropertyTag* tag = FindProperty(element, "ImportedMaterialSlotName")) slot.ImportedSlotName = PropertyName(reader, *tag, package);
                    slots.push_back(std::move(slot));
                    reader.Seek(next);
                }
            }

            if (const PropertyTag* sourceModels = FindProperty(tags, "SourceModels"))
            {
                const std::int32_t count = PropertyArrayBegin(reader, *sourceModels);
                if (count > 0)
                {
                    const std::vector<PropertyTag> lod0 = ReadTaggedProperties(reader, package);
                    if (const PropertyTag* tag = FindProperty(lod0, "StaticMeshDescriptionBulkData"))
                    {
                        lod0BulkDataObject = PropertyObject(reader, *tag);
                    }
                }
            }

            if (const PropertyTag* sectionInfo = FindProperty(tags, "SectionInfoMap"))
            {
                const std::vector<PropertyTag> infoMap = PropertyStruct(reader, *sectionInfo, package);
                if (const PropertyTag* map = FindProperty(infoMap, "Map"))
                {
                    reader.Seek(map->ValueOffset);
                    const std::int32_t removed = reader.Read<std::int32_t>();
                    reader.Skip(static_cast<std::int64_t>((std::max)(removed, 0)) * 4);
                    const std::int32_t count = reader.Read<std::int32_t>();
                    for (std::int32_t index = 0; index < count && reader.Ok(); ++index)
                    {
                        const std::uint32_t key = reader.Read<std::uint32_t>();
                        const std::vector<PropertyTag> info = ReadTaggedProperties(reader, package);
                        const std::size_t next = reader.Tell();
                        std::int32_t materialIndex = 0;   // unsaved means the default, 0
                        if (const PropertyTag* tag = FindProperty(info, "MaterialIndex"))
                        {
                            materialIndex = static_cast<std::int32_t>(PropertyInt(reader, *tag));
                        }
                        if ((key >> 16) == 0)
                        {
                            sectionMaterials[key & 0xFFFFu] = materialIndex;
                        }
                        reader.Seek(next);
                    }
                }
            }
        }

        bool DecodeMeshDescriptionPayload(const Package& package, const std::vector<std::uint8_t>& payload, MeshDescription& description, std::string& error)
        {
            ByteReader reader(payload.data(), payload.size());
            const bool compactNames = package.CustomVersion(CustomVersions::FortniteMain) >= kFortniteMainTriangleLabels;
            return ReadMeshDescription(reader, compactNames, description, error);
        }
    }

    bool ReadStaticMesh(const Package& package, StaticMeshData& mesh, std::string& error)
    {
        const ObjectExport* meshExport = package.FindExportByClass("StaticMesh");
        if (meshExport == nullptr)
        {
            error = package.FindExportByClass("SkeletalMesh") != nullptr
                ? "skeletal meshes are not supported yet; only static meshes can be imported"
                : "the package does not contain a StaticMesh";
            return false;
        }
        mesh.Name = meshExport->ObjectName;

        if (package.CustomVersion(CustomVersions::UE5MainStream) < kUE5MainMeshDescriptionNewFormat)
        {
            error = "the mesh was saved by Unreal Engine 4; open and resave it in Unreal Engine 5 first";
            return false;
        }

        std::vector<MaterialSlotInfo> slots;
        std::int32_t lod0BulkDataObject = 0;
        std::map<std::uint32_t, std::int32_t> sectionMaterials;
        ReadStaticMeshProperties(package, *meshExport, slots, lod0BulkDataObject, sectionMaterials);

        // LOD 0 source geometry: the StaticMeshDescriptionBulkData object SourceModels[0]
        // points at. Fall back to the first payload that parses as a mesh description (older
        // 5.x releases kept it inside the StaticMesh export itself).
        // The parsed description points into the payload, which must outlive it.
        std::vector<std::uint8_t> payload;
        MeshDescription description;
        bool decoded = false;
        std::string decodeError;
        auto tryExport = [&](const ObjectExport& candidate)
        {
            EditorBulkDataRef bulkData;
            std::vector<std::uint8_t> candidatePayload;
            if (!FindEditorBulkData(package, candidate, bulkData) || !ReadEditorBulkData(package, bulkData, candidatePayload, decodeError))
            {
                return false;
            }
            MeshDescription parsed;
            if (!DecodeMeshDescriptionPayload(package, candidatePayload, parsed, decodeError) || parsed.find("Triangles") == parsed.end())
            {
                return false;
            }
            // Moving the vector keeps its heap block, so the parsed pointers stay valid.
            payload = std::move(candidatePayload);
            description = std::move(parsed);
            return true;
        };

        if (lod0BulkDataObject > 0 && static_cast<std::size_t>(lod0BulkDataObject) <= package.Exports().size())
        {
            decoded = tryExport(package.Exports()[static_cast<std::size_t>(lod0BulkDataObject - 1)]);
        }
        if (!decoded)
        {
            decoded = tryExport(*meshExport);
        }
        if (!decoded)
        {
            for (const ObjectExport* candidate : package.FindExportsByClass("StaticMeshDescriptionBulkData"))
            {
                if (tryExport(*candidate))
                {
                    decoded = true;
                    break;
                }
            }
        }
        if (!decoded)
        {
            error = decodeError.empty() ? "could not find the mesh's source geometry" : decodeError;
            return false;
        }

        auto container = [&description](const char* name) -> const ElementContainer*
        {
            const auto found = description.find(name);
            return (found != description.end() && !found->second.empty()) ? &found->second.front() : nullptr;
        };

        const ElementContainer* vertices = container("Vertices");
        const ElementContainer* instances = container("VertexInstances");
        const ElementContainer* triangles = container("Triangles");
        const ElementContainer* groups = container("PolygonGroups");
        if (vertices == nullptr || instances == nullptr || triangles == nullptr)
        {
            error = "mesh description lacks vertices, vertex instances or triangles";
            return false;
        }

        const Attribute* positions = vertices->Find("Position ");
        if (positions == nullptr) positions = vertices->Find("Position");
        const Attribute* instanceVertex = instances->Find("VertexIndex");
        const Attribute* normals = instances->Find("Normal");
        const Attribute* uvs = instances->Find("TextureCoordinate");
        const Attribute* triangleInstances = triangles->Find("VertexInstanceIndex");
        const Attribute* triangleGroups = triangles->Find("PolygonGroupIndex");
        if (positions == nullptr || instanceVertex == nullptr || triangleInstances == nullptr)
        {
            error = "mesh description lacks positions or triangle connectivity";
            return false;
        }

        // Polygon group -> material slot. A saved SectionInfoMap (sections are the
        // non-empty polygon groups in ID order) is what the editor renders with; otherwise
        // UE matches the group's imported slot name against StaticMaterials.
        std::vector<std::string> groupSlotNames;
        if (groups != nullptr)
        {
            if (const Attribute* names = groups->Find("ImportedMaterialSlotName"); names != nullptr && !names->Names.empty())
            {
                groupSlotNames = names->Names.front();
            }
        }

        const std::size_t triangleCount = triangles->Allocated.size();
        std::vector<std::int32_t> triangleGroup(triangleCount, 0);
        std::int32_t maxGroup = 0;
        for (std::size_t triangle = 0; triangle < triangleCount; ++triangle)
        {
            triangleGroup[triangle] = (std::max)(0, AttributeValue<std::int32_t>(triangleGroups, triangle, 0));
            maxGroup = (std::max)(maxGroup, triangleGroup[triangle]);
        }

        std::vector<bool> groupUsed(static_cast<std::size_t>(maxGroup) + 1, false);
        for (std::size_t triangle = 0; triangle < triangleCount; ++triangle)
        {
            if (triangles->Allocated[triangle])
            {
                groupUsed[static_cast<std::size_t>(triangleGroup[triangle])] = true;
            }
        }

        std::vector<std::int32_t> groupToSlot(groupUsed.size(), 0);
        std::uint32_t sectionIndex = 0;
        for (std::size_t group = 0; group < groupUsed.size(); ++group)
        {
            std::int32_t slot = -1;
            if (group < groupSlotNames.size())
            {
                for (std::size_t index = 0; index < slots.size() && slot < 0; ++index)
                {
                    if (!groupSlotNames[group].empty() && slots[index].ImportedSlotName == groupSlotNames[group]) slot = static_cast<std::int32_t>(index);
                }
                for (std::size_t index = 0; index < slots.size() && slot < 0; ++index)
                {
                    if (!groupSlotNames[group].empty() && slots[index].SlotName == groupSlotNames[group]) slot = static_cast<std::int32_t>(index);
                }
            }
            if (slot < 0)
            {
                slot = static_cast<std::int32_t>(group);
            }

            if (groupUsed[group])
            {
                if (const auto section = sectionMaterials.find(sectionIndex); section != sectionMaterials.end())
                {
                    slot = section->second;
                }
                ++sectionIndex;
            }

            groupToSlot[group] = slots.empty() ? 0 : (std::clamp)(slot, 0, static_cast<std::int32_t>(slots.size()) - 1);
        }

        // Normals are recomputed when the source has none (all zero).
        bool hasNormals = false;
        if (normals != nullptr)
        {
            for (std::size_t instance = 0; instance < instances->Allocated.size() && !hasNormals; ++instance)
            {
                const Float3 n = AttributeValue<Float3>(normals, instance, Float3{ 0, 0, 0 });
                hasNormals = (n.X * n.X + n.Y * n.Y + n.Z * n.Z) > 0.01f;
            }
        }

        std::vector<Float3> smoothNormals;
        if (!hasNormals)
        {
            smoothNormals.assign(vertices->Allocated.size(), Float3{ 0, 0, 0 });
            for (std::size_t triangle = 0; triangle < triangleCount; ++triangle)
            {
                if (!triangles->Allocated[triangle]) continue;
                std::int32_t corner[3];
                Float3 p[3];
                for (int k = 0; k < 3; ++k)
                {
                    const std::int32_t instance = AttributeValue<std::int32_t>(triangleInstances, triangle * 3 + k, -1);
                    corner[k] = AttributeValue<std::int32_t>(instanceVertex, static_cast<std::size_t>((std::max)(instance, 0)), -1);
                    p[k] = AttributeValue<Float3>(positions, static_cast<std::size_t>((std::max)(corner[k], 0)), Float3{ 0, 0, 0 });
                }
                const Float3 e1{ p[1].X - p[0].X, p[1].Y - p[0].Y, p[1].Z - p[0].Z };
                const Float3 e2{ p[2].X - p[0].X, p[2].Y - p[0].Y, p[2].Z - p[0].Z };
                // UE's front faces wind clockwise in its left-handed frame.
                const Float3 face{ e2.Y * e1.Z - e2.Z * e1.Y, e2.Z * e1.X - e2.X * e1.Z, e2.X * e1.Y - e2.Y * e1.X };
                for (int k = 0; k < 3; ++k)
                {
                    if (corner[k] >= 0 && static_cast<std::size_t>(corner[k]) < smoothNormals.size())
                    {
                        Float3& n = smoothNormals[static_cast<std::size_t>(corner[k])];
                        n.X += face.X; n.Y += face.Y; n.Z += face.Z;
                    }
                }
            }
        }

        // Emit per slot so each slot's triangles form one contiguous index range.
        std::map<std::int32_t, std::vector<std::size_t>> trianglesBySlot;
        for (std::size_t triangle = 0; triangle < triangleCount; ++triangle)
        {
            if (triangles->Allocated[triangle])
            {
                trianglesBySlot[groupToSlot[static_cast<std::size_t>(triangleGroup[triangle])]].push_back(triangle);
            }
        }

        std::unordered_map<VertexKey, std::uint32_t, VertexKeyHash> vertexLookup;
        vertexLookup.reserve(instances->Allocated.size());
        for (const auto& [slot, slotTriangles] : trianglesBySlot)
        {
            PteroSubMeshEntry subMesh{};
            subMesh.materialId = static_cast<std::uint32_t>(slot);
            subMesh.indexStart = static_cast<std::uint32_t>(mesh.Indices.size());

            for (const std::size_t triangle : slotTriangles)
            {
                std::uint32_t corners[3] = {};
                bool valid = true;
                for (int k = 0; k < 3 && valid; ++k)
                {
                    const std::int32_t instance = AttributeValue<std::int32_t>(triangleInstances, triangle * 3 + k, -1);
                    const std::int32_t vertexIndex = instance >= 0
                        ? AttributeValue<std::int32_t>(instanceVertex, static_cast<std::size_t>(instance), -1)
                        : -1;
                    if (vertexIndex < 0)
                    {
                        valid = false;
                        break;
                    }

                    Vertex vertex{};
                    vertex.Position = ConvertPosition(AttributeValue<Float3>(positions, static_cast<std::size_t>(vertexIndex), Float3{ 0, 0, 0 }));

                    Float3 normal = hasNormals
                        ? AttributeValue<Float3>(normals, static_cast<std::size_t>(instance), Float3{ 0, 0, 1 })
                        : (static_cast<std::size_t>(vertexIndex) < smoothNormals.size() ? smoothNormals[static_cast<std::size_t>(vertexIndex)] : Float3{ 0, 0, 1 });
                    const float length = std::sqrt(normal.X * normal.X + normal.Y * normal.Y + normal.Z * normal.Z);
                    normal = length > 1e-12f ? Float3{ normal.X / length, normal.Y / length, normal.Z / length } : Float3{ 0, 0, 1 };
                    vertex.Normal = ConvertDirection(normal);

                    // UE and Ptero both put V = 0 at the top of the image.
                    const Float2 uv = AttributeValue<Float2>(uvs, static_cast<std::size_t>(instance), Float2{ 0, 0 });
                    vertex.TexCoord = DirectX::XMFLOAT2(uv.X, uv.Y);
                    // Vertex colours in UE content are usually blend masks rather than tint,
                    // so they are not carried over; the FBX importer writes white as well.
                    vertex.Color = DirectX::XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);

                    const VertexKey key{ vertex };
                    const auto existing = vertexLookup.find(key);
                    if (existing != vertexLookup.end())
                    {
                        corners[k] = existing->second;
                    }
                    else
                    {
                        corners[k] = static_cast<std::uint32_t>(mesh.Vertices.size());
                        mesh.Vertices.push_back(vertex);
                        vertexLookup.emplace(key, corners[k]);
                    }
                }

                if (!valid)
                {
                    continue;
                }

                // The two engines disagree on which winding faces front.
                mesh.Indices.push_back(corners[0]);
                mesh.Indices.push_back(corners[2]);
                mesh.Indices.push_back(corners[1]);
            }

            subMesh.indexCount = static_cast<std::uint32_t>(mesh.Indices.size()) - subMesh.indexStart;
            if (subMesh.indexCount > 0)
            {
                mesh.SubMeshes.push_back(subMesh);
            }
        }

        for (const MaterialSlotInfo& slot : slots)
        {
            MeshMaterialSlot outSlot;
            outSlot.SlotName = slot.SlotName.empty() ? slot.ImportedSlotName : slot.SlotName;
            outSlot.MaterialPath = package.ObjectPath(slot.MaterialIndex);
            mesh.Slots.push_back(std::move(outSlot));
        }
        if (mesh.Slots.empty())
        {
            mesh.Slots.push_back(MeshMaterialSlot{ "Material", {} });
        }

        if (mesh.Indices.empty())
        {
            error = "the mesh has no triangles";
            return false;
        }
        return true;
    }
}
