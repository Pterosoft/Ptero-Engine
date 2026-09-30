#include "UnrealMaterial.h"

#include <deque>
#include <set>

namespace Ptero::Unreal
{
    namespace
    {
        // Material inputs worth tracing, as named on UMaterialEditorOnlyData.
        constexpr const char* kTracedInputs[] = {
            "BaseColor", "Normal", "Roughness", "Metallic", "AmbientOcclusion",
            "EmissiveColor", "Opacity", "OpacityMask", "Displacement", "Specular",
        };

        struct ExpressionLink
        {
            std::int32_t Expression = 0;
            int Channel = -1;
        };

        int SingleChannel(const bool r, const bool g, const bool b, const bool a)
        {
            const int count = int(r) + int(g) + int(b) + int(a);
            if (count != 1)
            {
                return -1;
            }
            return r ? 0 : g ? 1 : b ? 2 : 3;
        }

        bool IsExpressionInputStruct(const std::string& structName)
        {
            return structName == "ExpressionInput" ||
                (structName.size() > 13 && structName.compare(structName.size() - 13, 13, "MaterialInput") == 0);
        }

        // FExpressionInput's native layout: int32 Expression, int32 OutputIndex,
        // FName InputName, int32 Mask, MaskR, MaskG, MaskB, MaskA (material inputs append
        // their constant, which is not needed here).
        ExpressionLink ReadBinaryExpressionInput(ByteReader& reader, const std::size_t offset)
        {
            reader.Seek(offset);
            ExpressionLink link;
            link.Expression = reader.Read<std::int32_t>();
            reader.Skip(4 + 8);
            const bool masked = reader.Read<std::int32_t>() != 0;
            const bool r = reader.Read<std::int32_t>() != 0;
            const bool g = reader.Read<std::int32_t>() != 0;
            const bool b = reader.Read<std::int32_t>() != 0;
            const bool a = reader.Read<std::int32_t>() != 0;
            link.Channel = masked ? SingleChannel(r, g, b, a) : -1;
            return link;
        }

        ExpressionLink ReadExpressionInput(ByteReader& reader, const PropertyTag& tag, const Package& package)
        {
            if (tag.HasBinaryOrNativeSerialize())
            {
                return ReadBinaryExpressionInput(reader, tag.ValueOffset);
            }

            // Older packages serialize the struct as tagged properties.
            ExpressionLink link;
            const std::vector<PropertyTag> fields = PropertyStruct(reader, tag, package);
            if (const PropertyTag* expression = FindProperty(fields, "Expression")) link.Expression = PropertyObject(reader, *expression);
            auto flag = [&](const char* name)
            {
                const PropertyTag* field = FindProperty(fields, name);
                return field != nullptr && PropertyInt(reader, *field) != 0;
            };
            if (flag("Mask"))
            {
                link.Channel = SingleChannel(flag("MaskR"), flag("MaskG"), flag("MaskB"), flag("MaskA"));
            }
            return link;
        }

        // Every expression input found anywhere in a property list (including inside
        // FunctionInputs arrays and nested structs).
        void CollectLinks(ByteReader& reader, const Package& package, const std::vector<PropertyTag>& tags, std::vector<ExpressionLink>& links, const int depth)
        {
            for (const PropertyTag& tag : tags)
            {
                if (tag.TypeName() == "StructProperty")
                {
                    if (IsExpressionInputStruct(tag.InnerTypeName()))
                    {
                        links.push_back(ReadExpressionInput(reader, tag, package));
                    }
                    else if (!tag.HasBinaryOrNativeSerialize() && depth < 4)
                    {
                        CollectLinks(reader, package, PropertyStruct(reader, tag, package), links, depth + 1);
                    }
                }
                else if (tag.TypeName() == "ArrayProperty" && tag.InnerTypeName() == "StructProperty" && depth < 4)
                {
                    const std::int32_t count = PropertyArrayBegin(reader, tag);
                    for (std::int32_t index = 0; index < count && index < 256 && reader.Ok(); ++index)
                    {
                        const std::vector<PropertyTag> element = ReadTaggedProperties(reader, package);
                        const std::size_t next = reader.Tell();
                        CollectLinks(reader, package, element, links, depth + 1);
                        reader.Seek(next);
                    }
                }
            }
        }

        bool IsTextureExpression(const std::string& className)
        {
            return className.find("TextureSample") != std::string::npos ||
                className == "MaterialExpressionTextureObjectParameter" ||
                className == "MaterialExpressionTextureObject" ||
                className == "MaterialExpressionRuntimeVirtualTextureSampleParameter";
        }

        struct TextureExpression
        {
            std::string ParameterName;
            std::string TexturePath;
        };

        TextureExpression ReadTextureExpression(const Package& package, const ObjectExport& expression)
        {
            TextureExpression result;
            ByteReader reader = package.ExportPropertyReader(expression);
            const std::vector<PropertyTag> tags = ReadTaggedProperties(reader, package);
            if (const PropertyTag* tag = FindProperty(tags, "ParameterName")) result.ParameterName = PropertyName(reader, *tag, package);
            if (const PropertyTag* tag = FindProperty(tags, "Texture")) result.TexturePath = package.ObjectPath(PropertyObject(reader, *tag));
            return result;
        }

        // Walks the expression graph upstream from a material input until it reaches a
        // texture sample. Channel masks met on the way (pin masks, ComponentMask nodes) say
        // which channel of that texture the input ends up reading.
        bool TraceToTexture(const Package& package, const ExpressionLink& start, std::int32_t& textureExpression, int& channel)
        {
            std::deque<ExpressionLink> queue{ start };
            std::set<std::int32_t> visited;
            while (!queue.empty() && visited.size() < 128)
            {
                const ExpressionLink current = queue.front();
                queue.pop_front();
                if (current.Expression <= 0 || static_cast<std::size_t>(current.Expression) > package.Exports().size() ||
                    !visited.insert(current.Expression).second)
                {
                    continue;
                }

                const ObjectExport& expression = package.Exports()[static_cast<std::size_t>(current.Expression - 1)];
                const std::string className = package.ExportClassName(expression);
                if (IsTextureExpression(className))
                {
                    textureExpression = current.Expression;
                    channel = current.Channel;
                    return true;
                }

                ByteReader reader = package.ExportPropertyReader(expression);
                const std::vector<PropertyTag> tags = ReadTaggedProperties(reader, package);

                int pending = current.Channel;
                if (className == "MaterialExpressionComponentMask")
                {
                    auto flag = [&](const char* name) { const PropertyTag* tag = FindProperty(tags, name); return tag != nullptr && tag->BoolValue; };
                    const int masked = SingleChannel(flag("R"), flag("G"), flag("B"), flag("A"));
                    if (masked >= 0)
                    {
                        pending = masked;
                    }
                }

                std::vector<ExpressionLink> links;
                CollectLinks(reader, package, tags, links, 0);
                for (ExpressionLink link : links)
                {
                    if (link.Channel < 0)
                    {
                        link.Channel = pending;
                    }
                    queue.push_back(link);
                }
            }
            return false;
        }

        struct MaterialLayer
        {
            bool IsInstance = false;
            std::string ParentPath;
            std::map<std::string, std::string> TextureParameters;   // name -> texture path
            std::map<std::string, float> Scalars;
            std::map<std::string, std::array<float, 4>> Vectors;
            bool HasBlendMode = false;
            std::string BlendMode;
            bool HasTwoSided = false;
            bool TwoSided = false;

            // Root material only: input -> traced texture expression data.
            struct InputTexture
            {
                std::string ParameterName;
                std::string DefaultTexture;
                int Channel = -1;
            };
            std::map<std::string, InputTexture> InputTextures;
        };

        std::string ParameterInfoName(ByteReader& reader, const std::vector<PropertyTag>& element, const Package& package)
        {
            if (const PropertyTag* info = FindProperty(element, "ParameterInfo"))
            {
                const std::vector<PropertyTag> fields = PropertyStruct(reader, *info, package);
                if (const PropertyTag* name = FindProperty(fields, "Name"))
                {
                    return PropertyName(reader, *name, package);
                }
            }
            if (const PropertyTag* name = FindProperty(element, "ParameterName"))
            {
                return PropertyName(reader, *name, package);
            }
            return {};
        }

        std::array<float, 4> ReadLinearColor(ByteReader& reader, const PropertyTag& tag)
        {
            reader.Seek(tag.ValueOffset);
            std::array<float, 4> value{ 0, 0, 0, 1 };
            for (float& channel : value)
            {
                channel = reader.Read<float>();
            }
            return value;
        }

        void ReadInstanceLayer(const Package& package, const ObjectExport& instance, MaterialLayer& layer)
        {
            layer.IsInstance = true;
            ByteReader reader = package.ExportPropertyReader(instance);
            const std::vector<PropertyTag> tags = ReadTaggedProperties(reader, package);

            if (const PropertyTag* parent = FindProperty(tags, "Parent"))
            {
                layer.ParentPath = package.ObjectPath(PropertyObject(reader, *parent));
            }

            auto forEachElement = [&](const char* arrayName, auto&& visit)
            {
                const PropertyTag* array = FindProperty(tags, arrayName);
                if (array == nullptr) return;
                const std::int32_t count = PropertyArrayBegin(reader, *array);
                for (std::int32_t index = 0; index < count && index < 1024 && reader.Ok(); ++index)
                {
                    const std::vector<PropertyTag> element = ReadTaggedProperties(reader, package);
                    const std::size_t next = reader.Tell();
                    visit(element);
                    reader.Seek(next);
                }
            };

            forEachElement("TextureParameterValues", [&](const std::vector<PropertyTag>& element)
            {
                const std::string name = ParameterInfoName(reader, element, package);
                if (const PropertyTag* value = FindProperty(element, "ParameterValue"); value != nullptr && !name.empty())
                {
                    layer.TextureParameters[name] = package.ObjectPath(PropertyObject(reader, *value));
                }
            });
            forEachElement("ScalarParameterValues", [&](const std::vector<PropertyTag>& element)
            {
                const std::string name = ParameterInfoName(reader, element, package);
                if (const PropertyTag* value = FindProperty(element, "ParameterValue"); value != nullptr && !name.empty())
                {
                    layer.Scalars[name] = PropertyFloat(reader, *value);
                }
            });
            forEachElement("VectorParameterValues", [&](const std::vector<PropertyTag>& element)
            {
                const std::string name = ParameterInfoName(reader, element, package);
                if (const PropertyTag* value = FindProperty(element, "ParameterValue"); value != nullptr && !name.empty())
                {
                    layer.Vectors[name] = ReadLinearColor(reader, *value);
                }
            });

            if (const PropertyTag* overrides = FindProperty(tags, "BasePropertyOverrides"))
            {
                const std::vector<PropertyTag> fields = PropertyStruct(reader, *overrides, package);
                const PropertyTag* overrideBlend = FindProperty(fields, "bOverride_BlendMode");
                if (overrideBlend != nullptr && overrideBlend->BoolValue)
                {
                    layer.HasBlendMode = true;
                    const PropertyTag* blend = FindProperty(fields, "BlendMode");
                    layer.BlendMode = blend != nullptr ? EnumValueName(PropertyEnum(reader, *blend, package)) : "BLEND_Opaque";
                }
                const PropertyTag* overrideTwoSided = FindProperty(fields, "bOverride_TwoSided");
                if (overrideTwoSided != nullptr && overrideTwoSided->BoolValue)
                {
                    layer.HasTwoSided = true;
                    const PropertyTag* twoSided = FindProperty(fields, "TwoSided");
                    layer.TwoSided = twoSided != nullptr && twoSided->BoolValue;
                }
            }
        }

        void ReadMaterialLayer(const Package& package, const ObjectExport& material, MaterialLayer& layer)
        {
            {
                ByteReader reader = package.ExportPropertyReader(material);
                const std::vector<PropertyTag> tags = ReadTaggedProperties(reader, package);
                layer.HasBlendMode = true;
                layer.BlendMode = "BLEND_Opaque";
                if (const PropertyTag* blend = FindProperty(tags, "BlendMode"))
                {
                    layer.BlendMode = EnumValueName(PropertyEnum(reader, *blend, package));
                }
                layer.HasTwoSided = true;
                if (const PropertyTag* twoSided = FindProperty(tags, "TwoSided"))
                {
                    layer.TwoSided = twoSided->BoolValue;
                }
            }

            // Parameter defaults live on the expressions.
            for (const ObjectExport& expression : package.Exports())
            {
                const std::string className = package.ExportClassName(expression);
                if (className.rfind("MaterialExpression", 0) != 0)
                {
                    continue;
                }

                ByteReader reader = package.ExportPropertyReader(expression);
                const std::vector<PropertyTag> tags = ReadTaggedProperties(reader, package);
                const PropertyTag* parameterName = FindProperty(tags, "ParameterName");
                if (parameterName == nullptr)
                {
                    continue;
                }
                const std::string name = PropertyName(reader, *parameterName, package);
                const PropertyTag* value = FindProperty(tags, "DefaultValue");

                if (IsTextureExpression(className))
                {
                    if (const PropertyTag* texture = FindProperty(tags, "Texture"))
                    {
                        layer.TextureParameters[name] = package.ObjectPath(PropertyObject(reader, *texture));
                    }
                }
                else if (className == "MaterialExpressionScalarParameter")
                {
                    layer.Scalars[name] = value != nullptr ? PropertyFloat(reader, *value) : 0.0f;
                }
                else if (className == "MaterialExpressionVectorParameter" && value != nullptr)
                {
                    layer.Vectors[name] = ReadLinearColor(reader, *value);
                }
            }

            // Material inputs, which UE 5 keeps on the editor-only data object (older
            // releases kept them on the material itself).
            std::vector<const ObjectExport*> inputOwners = package.FindExportsByClass("MaterialEditorOnlyData");
            inputOwners.push_back(&material);
            for (const ObjectExport* owner : inputOwners)
            {
                ByteReader reader = package.ExportPropertyReader(*owner);
                const std::vector<PropertyTag> tags = ReadTaggedProperties(reader, package);
                for (const char* input : kTracedInputs)
                {
                    const PropertyTag* tag = FindProperty(tags, input);
                    if (tag == nullptr || tag->TypeName() != "StructProperty" || layer.InputTextures.count(input) != 0)
                    {
                        continue;
                    }

                    const ExpressionLink link = ReadExpressionInput(reader, *tag, package);
                    std::int32_t textureExpression = 0;
                    int channel = -1;
                    if (link.Expression > 0 && TraceToTexture(package, link, textureExpression, channel))
                    {
                        const TextureExpression texture = ReadTextureExpression(package, package.Exports()[static_cast<std::size_t>(textureExpression - 1)]);
                        layer.InputTextures[input] = { texture.ParameterName, texture.TexturePath, channel };
                    }
                }
            }
        }
    }

    bool ResolveMaterial(const std::string& materialObjectPath, const PackageLoader& loadPackage, ResolvedMaterial& material, std::string& error)
    {
        // Leaf first: instance, its parent, ..., the root UMaterial.
        std::vector<MaterialLayer> chain;
        std::string currentPath = materialObjectPath;
        for (int guard = 0; guard < 16 && !currentPath.empty(); ++guard)
        {
            const Package* package = loadPackage(currentPath);
            if (package == nullptr)
            {
                if (chain.empty())
                {
                    error = "material package not found: " + currentPath;
                    return false;
                }
                break;   // missing parent (often /Engine content); use what we have
            }

            const std::string objectName = currentPath.substr(currentPath.rfind('.') + 1);
            const ObjectExport* exportEntry = nullptr;
            for (const ObjectExport& candidate : package->Exports())
            {
                const std::string className = package->ExportClassName(candidate);
                if (candidate.ObjectName == objectName && (className == "MaterialInstanceConstant" || className == "Material"))
                {
                    exportEntry = &candidate;
                    break;
                }
            }
            if (exportEntry == nullptr)
            {
                exportEntry = package->FindExportByClass("MaterialInstanceConstant");
            }
            if (exportEntry == nullptr)
            {
                exportEntry = package->FindExportByClass("Material");
            }
            if (exportEntry == nullptr)
            {
                if (chain.empty())
                {
                    error = "no material found in " + currentPath;
                    return false;
                }
                break;
            }

            if (chain.empty())
            {
                material.Name = exportEntry->ObjectName;
            }

            MaterialLayer layer;
            if (package->ExportClassName(*exportEntry) == "MaterialInstanceConstant")
            {
                ReadInstanceLayer(*package, *exportEntry, layer);
                currentPath = layer.ParentPath;
                chain.push_back(std::move(layer));
            }
            else
            {
                ReadMaterialLayer(*package, *exportEntry, layer);
                chain.push_back(std::move(layer));
                currentPath.clear();
            }
        }

        // Overlay parameter values root -> leaf so the most derived instance wins.
        std::map<std::string, std::string> textureParameters;
        for (auto layer = chain.rbegin(); layer != chain.rend(); ++layer)
        {
            for (const auto& [name, path] : layer->TextureParameters) textureParameters[name] = path;
            for (const auto& [name, value] : layer->Scalars) material.Scalars[name] = value;
            for (const auto& [name, value] : layer->Vectors) material.Vectors[name] = value;
            if (layer->HasBlendMode) material.BlendMode = layer->BlendMode;
            if (layer->HasTwoSided) material.TwoSided = layer->TwoSided;
        }

        const MaterialLayer& root = chain.back();
        std::set<std::string> boundParameters;
        if (!root.IsInstance)
        {
            for (const auto& [input, texture] : root.InputTextures)
            {
                TextureBinding binding;
                binding.ParameterName = texture.ParameterName;
                binding.Channel = texture.Channel;
                const auto overridden = texture.ParameterName.empty() ? textureParameters.end() : textureParameters.find(texture.ParameterName);
                binding.TexturePath = overridden != textureParameters.end() ? overridden->second : texture.DefaultTexture;
                if (!binding.TexturePath.empty())
                {
                    material.Inputs[input] = binding;
                    boundParameters.insert(texture.ParameterName);
                }
            }
        }

        for (const auto& [name, path] : textureParameters)
        {
            if (boundParameters.count(name) == 0 && !path.empty())
            {
                material.LooseTextures.push_back(TextureBinding{ path, -1, name });
            }
        }
        return true;
    }
}
