#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// A 128-bit random identifier (RFC 4122 version 4), for anything an asset has to point
// at across saves: a sequence, a binding inside it, a track, a camera cut.
//
// Entities keep their own 53-bit Entity::Id - the node graph carries those in a double -
// so this is for asset-internal identity, where nothing forces the narrower width and
// two assets authored apart must never collide.
struct Guid
{
    std::uint64_t High = 0;
    std::uint64_t Low = 0;

    static Guid New();

    // Accepts the canonical 8-4-4-4-12 form, with or without braces, any case. Returns
    // the null Guid and sets *ok to false for anything else.
    static Guid Parse(const std::string& text, bool* ok = nullptr);

    // Lower-case 8-4-4-4-12, the form Parse reads back.
    std::string ToString() const;

    bool IsValid() const { return High != 0 || Low != 0; }

    friend bool operator==(const Guid& a, const Guid& b) { return a.High == b.High && a.Low == b.Low; }
    friend bool operator!=(const Guid& a, const Guid& b) { return !(a == b); }
    friend bool operator<(const Guid& a, const Guid& b)
    {
        return a.High != b.High ? a.High < b.High : a.Low < b.Low;
    }
};

template <>
struct std::hash<Guid>
{
    std::size_t operator()(const Guid& guid) const noexcept
    {
        // Already uniformly random; folding the halves is as good as any mixing.
        return static_cast<std::size_t>(guid.High ^ (guid.Low * 0x9e3779b97f4a7c15ull));
    }
};
