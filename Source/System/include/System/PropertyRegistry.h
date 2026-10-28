#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// The engine's property reflection: one named, typed handle for every value a tool may
// read or drive, shared by Ptero Sequencer (property tracks) and the Node Graph (Get/Set
// Entity Property).
//
// Neither system knows a single component. A property is registered by the module that
// owns the data - Renderer_DX12 registers the entity properties because it owns Entity -
// with accessors that take the owning object as an opaque pointer. Adding a property to
// both editors is therefore one Register call next to the data, and a future weather
// system registering "Weather.RainIntensity" shows up in the Sequencer's Add Track menu and
// in the graph without either core being edited.
//
// Properties are grouped into domains, one per kind of owner object:
//   "Entity"       owner is Entity*           (Renderer_DX12/EntityProperties.cpp)
//   "Environment"  owner is the host's environment context (engine-wide settings)
// A domain's accessors may only ever be handed an owner of that domain's type; whoever
// resolves the owner (a CinematicHost, a NodeGraphHost) is what guarantees it.
//
// The registry is Qt-free and engine-free so the cinematic runtime and its tests can use
// it with owners of their own.

enum class PropertyType
{
    Bool,
    Int,
    Float,
    Vector2,
    Vector3,
    Vector4,
    // Linear RGBA. Animates as four channels; the editor offers a colour picker.
    Color,
    // Linear RGB, for colours with no alpha to animate - a light's, the fog's.
    ColorRGB,
    // Location (metres), rotation (Euler degrees, the engine's X then Y then Z order) and
    // scale - nine channels. Degrees because that is what an animator keys and reads on a
    // curve; the entity accessor converts to the radians the component stores.
    Transform
};

const char* PropertyTypeId(PropertyType type);
const char* PropertyTypeName(PropertyType type);
// Falls back to Float for an unknown id, which a newer build's file could carry.
PropertyType PropertyTypeFromId(const std::string& id, bool* known = nullptr);

// How many scalar channels a value of this type animates through: 1, 2, 3, 4 or 9.
int PropertyChannelCount(PropertyType type);
// "X", "Y", "Z", "W" / "R", "G", "B", "A" / "Location.X" ... "Scale.Z". Empty for scalar types.
const char* PropertyChannelName(PropertyType type, int channel);

// A value of any PropertyType. Every type is a run of doubles, so channel i of a track is
// simply Components[i] - the mapping between the curves the Sequencer edits and the value
// a property receives has no per-type code at all.
struct PropertyValue
{
    static constexpr int kMaxComponents = 9;

    PropertyType Type = PropertyType::Float;
    double Components[kMaxComponents]{};

    static PropertyValue MakeBool(bool value);
    static PropertyValue MakeInt(int value);
    static PropertyValue MakeFloat(double value);
    static PropertyValue MakeVector(PropertyType type, double x, double y, double z = 0.0, double w = 0.0);
    static PropertyValue MakeColor(double r, double g, double b, double a = 1.0);
    // A transform at the origin with no rotation and unit scale.
    static PropertyValue MakeTransform();

    bool AsBool() const { return Components[0] >= 0.5; }
    int AsInt() const;
    double AsFloat() const { return Components[0]; }

    // Component-wise comparison, for Auto Key's "did the user change this" test.
    bool NearlyEquals(const PropertyValue& other, double tolerance = 1e-4) const;

    // "1.5", "true", "(1, 2, 3)" - for logs and tooltips, not for saving.
    std::string ToDisplayString() const;
};

enum PropertyFlags : unsigned
{
    PropertyFlag_None = 0,
    // Offered by the Sequencer's Add Track menu.
    PropertyFlag_Animatable = 1u << 0,
    // Reachable from graph Get/Set property nodes.
    PropertyFlag_Scriptable = 1u << 1,
    // Get only. Never animated, never set by a graph.
    PropertyFlag_ReadOnly = 1u << 2,

    PropertyFlag_Default = PropertyFlag_Animatable | PropertyFlag_Scriptable
};

struct PropertyDescriptor
{
    // Which kind of owner this property lives on, e.g. "Entity".
    std::string Domain;
    // Stable identity inside the domain, written into sequences and graphs:
    // "PointLight.Intensity". Renaming one breaks every file that refers to it, so a
    // rename keeps the old path in Aliases.
    std::string Path;
    // The component or subsystem it belongs to, for menus: "Point Light".
    std::string Group;
    // What the editors show: "Intensity".
    std::string DisplayName;
    std::string Tooltip;
    // Other names Find answers to - older paths, and the Node Graph's original property
    // names ("Light Intensity"), so documents written before the registry still resolve.
    std::vector<std::string> Aliases;

    PropertyType Type = PropertyType::Float;
    // Editor slider range; ignored when equal.
    double Minimum = 0.0;
    double Maximum = 0.0;
    unsigned Flags = PropertyFlag_Default;

    // True when this owner currently carries the property - an entity without a light has
    // no light intensity. Null means always.
    std::function<bool(const void* owner)> Has;
    std::function<bool(const void* owner, PropertyValue& value)> Get;
    // Receives a value of `Type`. Returns false when the owner lacks the property.
    std::function<bool(void* owner, const PropertyValue& value)> Set;

    bool IsAvailableOn(const void* owner) const { return owner != nullptr && (!Has || Has(owner)); }
};

namespace PropertyRegistry
{
    // Adds a property, or replaces the one already registered under the same domain and
    // path - so a module that registers again (a reload) never duplicates its rows.
    // Registration is expected at startup on the main thread; lookups are not locked.
    void Register(PropertyDescriptor descriptor);

    // By path first, then by alias. Null when neither matches. Case-sensitive: paths are
    // identifiers, not display text. The pointer stays valid for the process lifetime.
    const PropertyDescriptor* Find(std::string_view domain, std::string_view pathOrAlias);

    // Every property of a domain, in registration order (which is what menus show).
    std::vector<const PropertyDescriptor*> List(std::string_view domain);

    // Bumped by every Register, so callers caching a descriptor pointer by path know when
    // to look it up again.
    std::size_t Revision();
}
