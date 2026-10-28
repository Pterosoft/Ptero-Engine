#include "System/PropertyRegistry.h"

#include <cmath>
#include <cstdio>
#include <memory>

namespace
{
    struct TypeInfo
    {
        PropertyType Type;
        const char* Id;
        const char* Name;
        int Channels;
    };

    constexpr TypeInfo kTypes[] = {
        { PropertyType::Bool, "bool", "Boolean", 1 },
        { PropertyType::Int, "int", "Integer", 1 },
        { PropertyType::Float, "float", "Float", 1 },
        { PropertyType::Vector2, "vector2", "Vector2", 2 },
        { PropertyType::Vector3, "vector3", "Vector3", 3 },
        { PropertyType::Vector4, "vector4", "Vector4", 4 },
        { PropertyType::Color, "color", "Color", 4 },
        { PropertyType::ColorRGB, "colorrgb", "Color (RGB)", 3 },
        { PropertyType::Transform, "transform", "Transform", 9 },
    };

    const TypeInfo& Info(PropertyType type)
    {
        for (const TypeInfo& info : kTypes)
        {
            if (info.Type == type)
                return info;
        }
        return kTypes[2];
    }

    // Descriptors are held by pointer so that a pointer handed out by Find survives later
    // registrations growing the list.
    struct Registry
    {
        std::vector<std::unique_ptr<PropertyDescriptor>> Descriptors;
        std::size_t Revision = 0;
    };

    Registry& Instance()
    {
        // Function-local so registration from another translation unit's static
        // initialiser cannot run before the registry exists.
        static Registry registry;
        return registry;
    }
}

const char* PropertyTypeId(PropertyType type)
{
    return Info(type).Id;
}

const char* PropertyTypeName(PropertyType type)
{
    return Info(type).Name;
}

PropertyType PropertyTypeFromId(const std::string& id, bool* known)
{
    for (const TypeInfo& info : kTypes)
    {
        if (id == info.Id)
        {
            if (known != nullptr)
                *known = true;
            return info.Type;
        }
    }

    if (known != nullptr)
        *known = false;
    return PropertyType::Float;
}

int PropertyChannelCount(PropertyType type)
{
    return Info(type).Channels;
}

const char* PropertyChannelName(PropertyType type, int channel)
{
    static const char* kVector[] = { "X", "Y", "Z", "W" };
    static const char* kColor[] = { "R", "G", "B", "A" };
    static const char* kTransform[] = { "Location.X", "Location.Y", "Location.Z",
                                        "Rotation.X", "Rotation.Y", "Rotation.Z",
                                        "Scale.X",    "Scale.Y",    "Scale.Z" };

    if (channel < 0 || channel >= PropertyChannelCount(type))
        return "";

    switch (type)
    {
    case PropertyType::Vector2:
    case PropertyType::Vector3:
    case PropertyType::Vector4:
        return kVector[channel];
    case PropertyType::Color:
    case PropertyType::ColorRGB:
        return kColor[channel];
    case PropertyType::Transform:
        return kTransform[channel];
    default:
        return "";
    }
}

PropertyValue PropertyValue::MakeBool(bool value)
{
    PropertyValue result;
    result.Type = PropertyType::Bool;
    result.Components[0] = value ? 1.0 : 0.0;
    return result;
}

PropertyValue PropertyValue::MakeInt(int value)
{
    PropertyValue result;
    result.Type = PropertyType::Int;
    result.Components[0] = static_cast<double>(value);
    return result;
}

PropertyValue PropertyValue::MakeFloat(double value)
{
    PropertyValue result;
    result.Type = PropertyType::Float;
    result.Components[0] = value;
    return result;
}

PropertyValue PropertyValue::MakeVector(PropertyType type, double x, double y, double z, double w)
{
    PropertyValue result;
    result.Type = type;
    result.Components[0] = x;
    result.Components[1] = y;
    result.Components[2] = z;
    result.Components[3] = w;
    return result;
}

PropertyValue PropertyValue::MakeColor(double r, double g, double b, double a)
{
    return MakeVector(PropertyType::Color, r, g, b, a);
}

PropertyValue PropertyValue::MakeTransform()
{
    PropertyValue result;
    result.Type = PropertyType::Transform;
    result.Components[6] = 1.0;
    result.Components[7] = 1.0;
    result.Components[8] = 1.0;
    return result;
}

int PropertyValue::AsInt() const
{
    return static_cast<int>(std::lround(Components[0]));
}

bool PropertyValue::NearlyEquals(const PropertyValue& other, double tolerance) const
{
    if (Type != other.Type)
        return false;

    const int count = PropertyChannelCount(Type);
    for (int i = 0; i < count; ++i)
    {
        if (std::fabs(Components[i] - other.Components[i]) > tolerance)
            return false;
    }
    return true;
}

std::string PropertyValue::ToDisplayString() const
{
    char buffer[64];
    switch (Type)
    {
    case PropertyType::Bool:
        return AsBool() ? "true" : "false";
    case PropertyType::Int:
        std::snprintf(buffer, sizeof(buffer), "%d", AsInt());
        return buffer;
    case PropertyType::Float:
        std::snprintf(buffer, sizeof(buffer), "%.4g", Components[0]);
        return buffer;
    default:
        break;
    }

    std::string text = "(";
    const int count = PropertyChannelCount(Type);
    for (int i = 0; i < count; ++i)
    {
        std::snprintf(buffer, sizeof(buffer), i == 0 ? "%.4g" : ", %.4g", Components[i]);
        text += buffer;
    }
    text += ")";
    return text;
}

namespace PropertyRegistry
{
    void Register(PropertyDescriptor descriptor)
    {
        Registry& registry = Instance();
        ++registry.Revision;

        for (std::unique_ptr<PropertyDescriptor>& existing : registry.Descriptors)
        {
            if (existing->Domain == descriptor.Domain && existing->Path == descriptor.Path)
            {
                // Assigned in place, so pointers already handed out stay valid.
                *existing = std::move(descriptor);
                return;
            }
        }

        registry.Descriptors.push_back(std::make_unique<PropertyDescriptor>(std::move(descriptor)));
    }

    const PropertyDescriptor* Find(std::string_view domain, std::string_view pathOrAlias)
    {
        const Registry& registry = Instance();
        for (const std::unique_ptr<PropertyDescriptor>& descriptor : registry.Descriptors)
        {
            if (descriptor->Domain == domain && descriptor->Path == pathOrAlias)
                return descriptor.get();
        }

        for (const std::unique_ptr<PropertyDescriptor>& descriptor : registry.Descriptors)
        {
            if (descriptor->Domain != domain)
                continue;
            for (const std::string& alias : descriptor->Aliases)
            {
                if (alias == pathOrAlias)
                    return descriptor.get();
            }
        }

        return nullptr;
    }

    std::vector<const PropertyDescriptor*> List(std::string_view domain)
    {
        std::vector<const PropertyDescriptor*> result;
        for (const std::unique_ptr<PropertyDescriptor>& descriptor : Instance().Descriptors)
        {
            if (descriptor->Domain == domain)
                result.push_back(descriptor.get());
        }
        return result;
    }

    std::size_t Revision()
    {
        return Instance().Revision;
    }
}
