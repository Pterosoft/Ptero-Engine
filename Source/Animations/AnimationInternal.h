#pragma once

// AnimationInternal.h - private to Animations.dll: the asset file layouts and the Impl
// structs behind AnimationRuntime.h, shared by the runtime and the builders.

#include "AnimationRuntime.h"

#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/maths/simd_math.h>
#include <ozz/base/maths/soa_transform.h>
#include <ozz/base/memory/unique_ptr.h>

#include <acl/core/compressed_tracks.h>

#include <cstdint>
#include <string>
#include <vector>

namespace AnimationDetail
{
    // .pskel: SkeletonFileHeader, ArchiveSize bytes of ozz skeleton archive, then
    // JointCount inverse bind matrices (16 floats each, XMFLOAT4X4 layout).
    constexpr char kSkeletonMagic[4] = { 'P', 'S', 'K', 'L' };
    constexpr std::uint32_t kSkeletonVersion = 1;

    // .panim: ClipFileHeader, NameLength bytes of UTF-8 name, then DataSize bytes of ACL
    // compressed_tracks. One track per skeleton joint, in skeleton joint order.
    constexpr char kClipMagic[4] = { 'P', 'A', 'N', 'M' };
    constexpr std::uint32_t kClipVersion = 1;

    enum ClipFlags : std::uint32_t
    {
        ClipFlagAdditive = 1u << 0,
    };

#pragma pack(push, 1)
    struct SkeletonFileHeader
    {
        char Magic[4];
        std::uint32_t Version;
        std::uint32_t JointCount;
        std::uint32_t HierarchyHash;
        std::uint64_t ArchiveSize;
    };

    struct ClipFileHeader
    {
        char Magic[4];
        std::uint32_t Version;
        std::uint32_t TrackCount;
        std::uint32_t SkeletonHash;
        std::uint32_t Flags;
        float Duration;
        float SampleRate;
        std::uint32_t NameLength;
        std::uint64_t DataSize;
    };
#pragma pack(pop)

    void SetError(char* error, std::size_t errorSize, const char* format, ...);

    std::uint32_t HashHierarchy(const ozz::animation::Skeleton& skeleton);

    bool ReadWholeFile(const char* utf8Path, std::vector<std::uint8_t>& out, char* error, std::size_t errorSize);
    bool WriteWholeFile(const char* utf8Path, const std::vector<std::uint8_t>& data, char* error, std::size_t errorSize);

    template <typename T>
    void Append(std::vector<std::uint8_t>& out, const T& value)
    {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        out.insert(out.end(), bytes, bytes + sizeof(T));
    }

    inline void AppendBytes(std::vector<std::uint8_t>& out, const void* data, std::size_t size)
    {
        const auto* bytes = static_cast<const std::uint8_t*>(data);
        out.insert(out.end(), bytes, bytes + size);
    }

    // One lane of a SoA register. ozz keeps four joints per SoaTransform; joint j lives in
    // element j / 4, lane j % 4.
    inline float& Lane(ozz::math::SimdFloat4& v, int lane) { return reinterpret_cast<float*>(&v)[lane]; }
    inline float Lane(const ozz::math::SimdFloat4& v, int lane) { return reinterpret_cast<const float*>(&v)[lane]; }

    void ReadSoaLane(const ozz::math::SoaTransform& soa, int lane, AnimationTransform& out);
    void WriteSoaLane(ozz::math::SoaTransform& soa, int lane, const AnimationTransform& in);
}

struct AnimationSkeleton::Impl
{
    ozz::animation::Skeleton Skeleton;
    std::vector<ozz::math::Float4x4> InverseBind;
    std::uint32_t Hash = 0;
};

struct AnimationClip::Impl
{
    ~Impl();

    void* Buffer = nullptr; // 16-byte aligned copy of the ACL data, owns Tracks
    const acl::compressed_tracks* Tracks = nullptr;
    std::string Name;
    float Duration = 0.0f;
    float SampleRate = 0.0f;
    std::uint32_t TrackCount = 0;
    std::uint32_t SkeletonHash = 0;
    bool Additive = false;
};
