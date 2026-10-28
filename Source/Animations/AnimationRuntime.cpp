#include "pch.h"

#include "AnimationInternal.h"

#include <ozz/animation/runtime/blending_job.h>
#include <ozz/animation/runtime/ik_aim_job.h>
#include <ozz/animation/runtime/ik_two_bone_job.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/simd_quaternion.h>
#include <ozz/base/span.h>

#include <acl/decompression/decompress.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>

namespace AnimationDetail
{
    void SetError(char* error, std::size_t errorSize, const char* format, ...)
    {
        if (error == nullptr || errorSize == 0)
        {
            return;
        }

        va_list args;
        va_start(args, format);
        std::vsnprintf(error, errorSize, format, args);
        va_end(args);
    }

    std::uint32_t HashHierarchy(const ozz::animation::Skeleton& skeleton)
    {
        // FNV-1a over every name (with its terminator) and parent index.
        std::uint32_t hash = 2166136261u;
        auto mix = [&hash](const void* data, std::size_t size)
        {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            for (std::size_t i = 0; i < size; ++i)
            {
                hash ^= bytes[i];
                hash *= 16777619u;
            }
        };

        const int count = skeleton.num_joints();
        mix(&count, sizeof(count));
        for (int i = 0; i < count; ++i)
        {
            const char* name = skeleton.joint_names()[i];
            mix(name, std::strlen(name) + 1);
            const std::int16_t parent = skeleton.joint_parents()[i];
            mix(&parent, sizeof(parent));
        }
        return hash;
    }

    static std::filesystem::path PathFromUtf8(const char* utf8Path)
    {
        return std::filesystem::path(reinterpret_cast<const char8_t*>(utf8Path));
    }

    bool ReadWholeFile(const char* utf8Path, std::vector<std::uint8_t>& out, char* error, std::size_t errorSize)
    {
        out.clear();
        if (utf8Path == nullptr || utf8Path[0] == '\0')
        {
            SetError(error, errorSize, "No file name given.");
            return false;
        }

        std::ifstream file(PathFromUtf8(utf8Path), std::ios::binary | std::ios::ate);
        if (!file)
        {
            SetError(error, errorSize, "Cannot open '%s'.", utf8Path);
            return false;
        }

        const std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        out.resize(static_cast<std::size_t>(size));
        if (size > 0 && !file.read(reinterpret_cast<char*>(out.data()), size))
        {
            SetError(error, errorSize, "Cannot read '%s'.", utf8Path);
            out.clear();
            return false;
        }
        return true;
    }

    bool WriteWholeFile(const char* utf8Path, const std::vector<std::uint8_t>& data, char* error, std::size_t errorSize)
    {
        const std::filesystem::path path = PathFromUtf8(utf8Path);
        std::error_code ec;
        if (path.has_parent_path())
        {
            std::filesystem::create_directories(path.parent_path(), ec);
        }

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file || !file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size())))
        {
            SetError(error, errorSize, "Cannot write '%s'.", utf8Path);
            return false;
        }
        return true;
    }

    void ReadSoaLane(const ozz::math::SoaTransform& soa, int lane, AnimationTransform& out)
    {
        out.Translation[0] = Lane(soa.translation.x, lane);
        out.Translation[1] = Lane(soa.translation.y, lane);
        out.Translation[2] = Lane(soa.translation.z, lane);
        out.Rotation[0] = Lane(soa.rotation.x, lane);
        out.Rotation[1] = Lane(soa.rotation.y, lane);
        out.Rotation[2] = Lane(soa.rotation.z, lane);
        out.Rotation[3] = Lane(soa.rotation.w, lane);
        out.Scale[0] = Lane(soa.scale.x, lane);
        out.Scale[1] = Lane(soa.scale.y, lane);
        out.Scale[2] = Lane(soa.scale.z, lane);
    }

    void WriteSoaLane(ozz::math::SoaTransform& soa, int lane, const AnimationTransform& in)
    {
        Lane(soa.translation.x, lane) = in.Translation[0];
        Lane(soa.translation.y, lane) = in.Translation[1];
        Lane(soa.translation.z, lane) = in.Translation[2];
        Lane(soa.rotation.x, lane) = in.Rotation[0];
        Lane(soa.rotation.y, lane) = in.Rotation[1];
        Lane(soa.rotation.z, lane) = in.Rotation[2];
        Lane(soa.rotation.w, lane) = in.Rotation[3];
        Lane(soa.scale.x, lane) = in.Scale[0];
        Lane(soa.scale.y, lane) = in.Scale[1];
        Lane(soa.scale.z, lane) = in.Scale[2];
    }
}

using namespace AnimationDetail;

namespace
{
    using DecompressionContext = acl::decompression_context<acl::default_transform_decompression_settings>;

    // Writes ACL's decompressed tracks straight into ozz's SoA layout.
    struct SoaTrackWriter final : public acl::track_writer
    {
        ozz::math::SoaTransform* Output = nullptr;

        void RTM_SIMD_CALL write_rotation(std::uint32_t track, rtm::quatf_arg0 rotation)
        {
            ozz::math::SoaTransform& soa = Output[track / 4];
            const int lane = static_cast<int>(track & 3);
            Lane(soa.rotation.x, lane) = rtm::quat_get_x(rotation);
            Lane(soa.rotation.y, lane) = rtm::quat_get_y(rotation);
            Lane(soa.rotation.z, lane) = rtm::quat_get_z(rotation);
            Lane(soa.rotation.w, lane) = rtm::quat_get_w(rotation);
        }

        void RTM_SIMD_CALL write_translation(std::uint32_t track, rtm::vector4f_arg0 translation)
        {
            ozz::math::SoaTransform& soa = Output[track / 4];
            const int lane = static_cast<int>(track & 3);
            Lane(soa.translation.x, lane) = rtm::vector_get_x(translation);
            Lane(soa.translation.y, lane) = rtm::vector_get_y(translation);
            Lane(soa.translation.z, lane) = rtm::vector_get_z(translation);
        }

        void RTM_SIMD_CALL write_scale(std::uint32_t track, rtm::vector4f_arg0 scale)
        {
            ozz::math::SoaTransform& soa = Output[track / 4];
            const int lane = static_cast<int>(track & 3);
            Lane(soa.scale.x, lane) = rtm::vector_get_x(scale);
            Lane(soa.scale.y, lane) = rtm::vector_get_y(scale);
            Lane(soa.scale.z, lane) = rtm::vector_get_z(scale);
        }
    };

    // Writes one track into an AnimationTransform (AnimationClip::SampleJoint).
    struct SingleTrackWriter final : public acl::track_writer
    {
        AnimationTransform* Output = nullptr;

        void RTM_SIMD_CALL write_rotation(std::uint32_t, rtm::quatf_arg0 rotation)
        {
            Output->Rotation[0] = rtm::quat_get_x(rotation);
            Output->Rotation[1] = rtm::quat_get_y(rotation);
            Output->Rotation[2] = rtm::quat_get_z(rotation);
            Output->Rotation[3] = rtm::quat_get_w(rotation);
        }

        void RTM_SIMD_CALL write_translation(std::uint32_t, rtm::vector4f_arg0 translation)
        {
            Output->Translation[0] = rtm::vector_get_x(translation);
            Output->Translation[1] = rtm::vector_get_y(translation);
            Output->Translation[2] = rtm::vector_get_z(translation);
        }

        void RTM_SIMD_CALL write_scale(std::uint32_t, rtm::vector4f_arg0 scale)
        {
            Output->Scale[0] = rtm::vector_get_x(scale);
            Output->Scale[1] = rtm::vector_get_y(scale);
            Output->Scale[2] = rtm::vector_get_z(scale);
        }
    };

    float ResolveClipTime(float time, float duration, bool loop)
    {
        if (duration <= 0.0f || !std::isfinite(time))
        {
            return 0.0f;
        }
        if (loop)
        {
            time = std::fmod(time, duration);
            return time < 0.0f ? time + duration : time;
        }
        return std::clamp(time, 0.0f, duration);
    }

    template <typename T>
    ozz::span<const T> ConstSpan(const std::vector<T>& v)
    {
        return ozz::span<const T>(v.data(), v.size());
    }

    template <typename T>
    ozz::span<T> MutableSpan(std::vector<T>& v)
    {
        return ozz::span<T>(v.data(), v.size());
    }

    // Post-multiplies one joint's local rotation by `correction`, as ozz's IK samples do.
    void MultiplyLocalRotation(std::vector<ozz::math::SoaTransform>& locals, int joint,
                               const ozz::math::SimdQuaternion& correction)
    {
        ozz::math::SoaTransform& soa = locals[joint / 4];
        ozz::math::SimdQuaternion aos[4];
        ozz::math::Transpose4x4(&soa.rotation.x, &aos->xyzw);
        aos[joint & 3] = aos[joint & 3] * correction;
        ozz::math::Transpose4x4(&aos->xyzw, &soa.rotation.x);
    }
}

// ---- AnimationSkeleton ------------------------------------------------------------------

AnimationSkeleton::AnimationSkeleton() = default;

AnimationSkeleton::~AnimationSkeleton()
{
    Reset();
}

void AnimationSkeleton::Reset()
{
    delete mImpl;
    mImpl = nullptr;
}

bool AnimationSkeleton::IsLoaded() const
{
    return mImpl != nullptr;
}

bool AnimationSkeleton::LoadFromFile(const char* path, char* error, std::size_t errorSize)
{
    Reset();
    std::vector<std::uint8_t> data;
    if (!ReadWholeFile(path, data, error, errorSize))
    {
        return false;
    }
    return LoadFromMemory(data.data(), data.size(), error, errorSize);
}

bool AnimationSkeleton::LoadFromMemory(const void* data, std::size_t size, char* error, std::size_t errorSize)
{
    Reset();

    SkeletonFileHeader header{};
    if (data == nullptr || size < sizeof(header))
    {
        SetError(error, errorSize, "Not a skeleton file (too small).");
        return false;
    }
    std::memcpy(&header, data, sizeof(header));
    if (std::memcmp(header.Magic, kSkeletonMagic, 4) != 0)
    {
        SetError(error, errorSize, "Not a skeleton file.");
        return false;
    }
    if (header.Version != kSkeletonVersion)
    {
        SetError(error, errorSize, "Skeleton file version %u is not supported (expected %u); re-import it.",
                 header.Version, kSkeletonVersion);
        return false;
    }

    const std::uint64_t bindBytes = static_cast<std::uint64_t>(header.JointCount) * 16u * sizeof(float);
    if (header.JointCount == 0 || header.ArchiveSize > size ||
        sizeof(header) + header.ArchiveSize + bindBytes > size)
    {
        SetError(error, errorSize, "Skeleton file is truncated.");
        return false;
    }

    const auto* bytes = static_cast<const std::uint8_t*>(data);
    auto impl = std::make_unique<Impl>();

    ozz::io::MemoryStream stream;
    stream.Write(bytes + sizeof(header), static_cast<std::size_t>(header.ArchiveSize));
    stream.Seek(0, ozz::io::Stream::kSet);
    ozz::io::IArchive archive(&stream);
    if (!archive.TestTag<ozz::animation::Skeleton>())
    {
        SetError(error, errorSize, "Skeleton file holds no ozz skeleton.");
        return false;
    }
    archive >> impl->Skeleton;

    if (impl->Skeleton.num_joints() != static_cast<int>(header.JointCount))
    {
        SetError(error, errorSize, "Skeleton file is corrupt (joint count %d, header says %u).",
                 impl->Skeleton.num_joints(), header.JointCount);
        return false;
    }

    impl->InverseBind.resize(header.JointCount);
    const std::uint8_t* bind = bytes + sizeof(header) + header.ArchiveSize;
    for (std::uint32_t i = 0; i < header.JointCount; ++i)
    {
        std::memcpy(&impl->InverseBind[i], bind + i * 16u * sizeof(float), 16u * sizeof(float));
    }

    impl->Hash = HashHierarchy(impl->Skeleton);
    mImpl = impl.release();
    return true;
}

int AnimationSkeleton::GetJointCount() const
{
    return mImpl ? mImpl->Skeleton.num_joints() : 0;
}

const char* AnimationSkeleton::GetJointName(int joint) const
{
    if (joint < 0 || joint >= GetJointCount())
    {
        return "";
    }
    return mImpl->Skeleton.joint_names()[joint];
}

int AnimationSkeleton::GetJointParent(int joint) const
{
    if (joint < 0 || joint >= GetJointCount())
    {
        return -1;
    }
    return mImpl->Skeleton.joint_parents()[joint];
}

int AnimationSkeleton::FindJoint(const char* name) const
{
    if (name == nullptr)
    {
        return -1;
    }
    for (int i = 0; i < GetJointCount(); ++i)
    {
        if (std::strcmp(mImpl->Skeleton.joint_names()[i], name) == 0)
        {
            return i;
        }
    }
    return -1;
}

bool AnimationSkeleton::GetRestPose(int joint, AnimationTransform& out) const
{
    if (joint < 0 || joint >= GetJointCount())
    {
        return false;
    }
    ReadSoaLane(mImpl->Skeleton.joint_rest_poses()[joint / 4], joint & 3, out);
    return true;
}

const float* AnimationSkeleton::GetInverseBindMatrices() const
{
    return mImpl ? reinterpret_cast<const float*>(mImpl->InverseBind.data()) : nullptr;
}

std::uint32_t AnimationSkeleton::GetHierarchyHash() const
{
    return mImpl ? mImpl->Hash : 0;
}

// ---- AnimationClip ----------------------------------------------------------------------

AnimationClip::Impl::~Impl()
{
    _aligned_free(Buffer);
}

AnimationClip::AnimationClip() = default;

AnimationClip::~AnimationClip()
{
    Reset();
}

void AnimationClip::Reset()
{
    delete mImpl;
    mImpl = nullptr;
}

bool AnimationClip::IsLoaded() const
{
    return mImpl != nullptr;
}

bool AnimationClip::LoadFromFile(const char* path, char* error, std::size_t errorSize)
{
    Reset();
    std::vector<std::uint8_t> data;
    if (!ReadWholeFile(path, data, error, errorSize))
    {
        return false;
    }
    return LoadFromMemory(data.data(), data.size(), error, errorSize);
}

bool AnimationClip::LoadFromMemory(const void* data, std::size_t size, char* error, std::size_t errorSize)
{
    Reset();

    ClipFileHeader header{};
    if (data == nullptr || size < sizeof(header))
    {
        SetError(error, errorSize, "Not an animation clip file (too small).");
        return false;
    }
    std::memcpy(&header, data, sizeof(header));
    if (std::memcmp(header.Magic, kClipMagic, 4) != 0)
    {
        SetError(error, errorSize, "Not an animation clip file.");
        return false;
    }
    if (header.Version != kClipVersion)
    {
        SetError(error, errorSize, "Animation clip version %u is not supported (expected %u); re-import it.",
                 header.Version, kClipVersion);
        return false;
    }
    if (header.DataSize == 0 || header.NameLength > size || header.DataSize > size ||
        sizeof(header) + header.NameLength + header.DataSize > size)
    {
        SetError(error, errorSize, "Animation clip file is truncated.");
        return false;
    }

    const auto* bytes = static_cast<const std::uint8_t*>(data);
    auto impl = std::make_unique<Impl>();
    impl->Name.assign(reinterpret_cast<const char*>(bytes + sizeof(header)), header.NameLength);

    // ACL reads its data in place and wants it 16-byte aligned; the file buffer is not.
    impl->Buffer = _aligned_malloc(static_cast<std::size_t>(header.DataSize), 16);
    if (impl->Buffer == nullptr)
    {
        SetError(error, errorSize, "Out of memory loading animation clip.");
        return false;
    }
    std::memcpy(impl->Buffer, bytes + sizeof(header) + header.NameLength, static_cast<std::size_t>(header.DataSize));

    acl::error_result aclError;
    impl->Tracks = acl::make_compressed_tracks(impl->Buffer, &aclError);
    if (impl->Tracks == nullptr || aclError.any())
    {
        SetError(error, errorSize, "Animation clip data is corrupt (%s).", aclError.any() ? aclError.c_str() : "unknown");
        return false;
    }
    if (impl->Tracks->get_track_type() != acl::track_type8::qvvf ||
        impl->Tracks->get_num_tracks() != header.TrackCount)
    {
        SetError(error, errorSize, "Animation clip data does not match its header.");
        return false;
    }

    impl->Duration = header.Duration;
    impl->SampleRate = header.SampleRate;
    impl->TrackCount = header.TrackCount;
    impl->SkeletonHash = header.SkeletonHash;
    impl->Additive = (header.Flags & ClipFlagAdditive) != 0;
    mImpl = impl.release();
    return true;
}

const char* AnimationClip::GetName() const
{
    return mImpl ? mImpl->Name.c_str() : "";
}

float AnimationClip::GetDuration() const
{
    return mImpl ? mImpl->Duration : 0.0f;
}

float AnimationClip::GetSampleRate() const
{
    return mImpl ? mImpl->SampleRate : 0.0f;
}

int AnimationClip::GetTrackCount() const
{
    return mImpl ? static_cast<int>(mImpl->TrackCount) : 0;
}

std::uint32_t AnimationClip::GetSkeletonHash() const
{
    return mImpl ? mImpl->SkeletonHash : 0;
}

bool AnimationClip::IsAdditive() const
{
    return mImpl && mImpl->Additive;
}

bool AnimationClip::IsCompatibleWith(const AnimationSkeleton& skeleton) const
{
    return mImpl && skeleton.IsLoaded() &&
           static_cast<int>(mImpl->TrackCount) == skeleton.GetJointCount() &&
           mImpl->SkeletonHash == skeleton.GetHierarchyHash();
}

bool AnimationClip::SampleJoint(int joint, float time, AnimationTransform& out) const
{
    if (!mImpl || joint < 0 || joint >= static_cast<int>(mImpl->TrackCount))
    {
        return false;
    }

    DecompressionContext context;
    if (!context.initialize(*mImpl->Tracks))
    {
        return false;
    }
    context.seek(ResolveClipTime(time, mImpl->Duration, false), acl::sample_rounding_policy::none);

    out = AnimationTransform{};
    SingleTrackWriter writer;
    writer.Output = &out;
    context.decompress_track(static_cast<std::uint32_t>(joint), writer);
    return true;
}

// ---- AnimationPose ----------------------------------------------------------------------

namespace
{
    // Scratch for one layer: its sampled locals, its joint mask in SoA form, and the
    // decompression context, which stays bound while the same clip keeps this slot.
    struct LayerScratch
    {
        std::vector<ozz::math::SoaTransform> Locals;
        std::vector<ozz::math::SimdFloat4> JointWeights;
        DecompressionContext Context;
    };
}

struct AnimationPose::Impl
{
    const AnimationSkeleton::Impl* Skeleton = nullptr;
    std::vector<ozz::math::SoaTransform> Locals;
    std::vector<ozz::math::Float4x4> Models;
    std::vector<std::unique_ptr<LayerScratch>> Layers;
    std::vector<ozz::animation::BlendingJob::Layer> Blend;
    std::vector<ozz::animation::BlendingJob::Layer> Additive;

    int JointCount() const { return Skeleton->Skeleton.num_joints(); }

    void LocalToModel(int from = ozz::animation::Skeleton::kNoParent)
    {
        ozz::animation::LocalToModelJob job;
        job.skeleton = &Skeleton->Skeleton;
        job.from = from;
        job.input = ConstSpan(Locals);
        job.output = MutableSpan(Models);
        job.Run();
    }
};

AnimationPose::AnimationPose() = default;

AnimationPose::~AnimationPose()
{
    Reset();
}

void AnimationPose::Reset()
{
    delete mImpl;
    mImpl = nullptr;
}

bool AnimationPose::Initialize(const AnimationSkeleton& skeleton)
{
    Reset();
    if (!skeleton.IsLoaded())
    {
        return false;
    }

    mImpl = new Impl();
    mImpl->Skeleton = skeleton.GetImpl();
    mImpl->Locals.resize(mImpl->Skeleton->Skeleton.num_soa_joints());
    mImpl->Models.resize(mImpl->Skeleton->Skeleton.num_joints());
    SetToRestPose();
    return true;
}

bool AnimationPose::IsInitialized() const
{
    return mImpl != nullptr;
}

int AnimationPose::GetJointCount() const
{
    return mImpl ? mImpl->JointCount() : 0;
}

void AnimationPose::SetToRestPose()
{
    if (!mImpl)
    {
        return;
    }
    const auto rest = mImpl->Skeleton->Skeleton.joint_rest_poses();
    std::copy(rest.begin(), rest.end(), mImpl->Locals.begin());
    mImpl->LocalToModel();
}

bool AnimationPose::Evaluate(const AnimationLayer* layers, int layerCount)
{
    if (!mImpl)
    {
        return false;
    }

    const AnimationSkeleton::Impl& skeleton = *mImpl->Skeleton;
    const auto rest = skeleton.Skeleton.joint_rest_poses();
    const int jointCount = mImpl->JointCount();
    const std::size_t soaCount = mImpl->Locals.size();

    mImpl->Blend.clear();
    mImpl->Additive.clear();

    int used = 0;
    for (int i = 0; layers != nullptr && i < layerCount; ++i)
    {
        const AnimationLayer& layer = layers[i];
        const AnimationClip::Impl* clip = layer.Clip ? layer.Clip->GetImpl() : nullptr;
        if (clip == nullptr || !(layer.Weight > 0.0f) ||
            static_cast<int>(clip->TrackCount) != jointCount || clip->SkeletonHash != skeleton.Hash)
        {
            continue;
        }

        if (used == static_cast<int>(mImpl->Layers.size()))
        {
            mImpl->Layers.push_back(std::make_unique<LayerScratch>());
        }
        LayerScratch& scratch = *mImpl->Layers[used++];

        // Start from the rest pose: ACL writes every joint, but the padding lanes of the
        // last SoA element have to hold something sane for the blend.
        scratch.Locals.assign(rest.begin(), rest.end());
        if (!scratch.Context.is_bound_to(*clip->Tracks))
        {
            scratch.Context.initialize(*clip->Tracks);
        }
        scratch.Context.seek(ResolveClipTime(layer.Time, clip->Duration, layer.Loop), acl::sample_rounding_policy::none);

        SoaTrackWriter writer;
        writer.Output = scratch.Locals.data();
        scratch.Context.decompress_tracks(writer);

        ozz::animation::BlendingJob::Layer blendLayer;
        blendLayer.weight = layer.Weight;
        blendLayer.transform = ConstSpan(scratch.Locals);
        if (layer.JointWeights != nullptr)
        {
            scratch.JointWeights.assign(soaCount, ozz::math::simd_float4::zero());
            for (int joint = 0; joint < jointCount; ++joint)
            {
                Lane(scratch.JointWeights[joint / 4], joint & 3) = std::clamp(layer.JointWeights[joint], 0.0f, 1.0f);
            }
            blendLayer.joint_weights = ConstSpan(scratch.JointWeights);
        }

        (clip->Additive ? mImpl->Additive : mImpl->Blend).push_back(blendLayer);
    }

    ozz::animation::BlendingJob job;
    job.layers = ConstSpan(mImpl->Blend);
    job.additive_layers = ConstSpan(mImpl->Additive);
    job.rest_pose = rest;
    job.output = MutableSpan(mImpl->Locals);
    if (!job.Run())
    {
        std::copy(rest.begin(), rest.end(), mImpl->Locals.begin());
    }

    mImpl->LocalToModel();
    return true;
}

bool AnimationPose::GetLocalTransform(int joint, AnimationTransform& out) const
{
    if (!mImpl || joint < 0 || joint >= mImpl->JointCount())
    {
        return false;
    }
    ReadSoaLane(mImpl->Locals[joint / 4], joint & 3, out);
    return true;
}

bool AnimationPose::SetLocalTransform(int joint, const AnimationTransform& transform)
{
    if (!mImpl || joint < 0 || joint >= mImpl->JointCount())
    {
        return false;
    }
    WriteSoaLane(mImpl->Locals[joint / 4], joint & 3, transform);
    return true;
}

void AnimationPose::UpdateModelSpace()
{
    if (mImpl)
    {
        mImpl->LocalToModel();
    }
}

bool AnimationPose::SolveTwoBoneIK(const AnimationTwoBoneIK& ik, bool* reached)
{
    if (!mImpl)
    {
        return false;
    }
    const int count = mImpl->JointCount();
    if (ik.StartJoint < 0 || ik.StartJoint >= count || ik.MidJoint < 0 || ik.MidJoint >= count ||
        ik.EndJoint < 0 || ik.EndJoint >= count)
    {
        return false;
    }

    using namespace ozz::math;
    ozz::animation::IKTwoBoneJob job;
    job.target = simd_float4::Load3PtrU(ik.Target);
    job.pole_vector = NormalizeSafe3(simd_float4::Load3PtrU(ik.PoleVector), simd_float4::z_axis());
    job.mid_axis = NormalizeSafe3(simd_float4::Load3PtrU(ik.MidAxis), simd_float4::z_axis());
    job.twist_angle = ik.TwistAngle;
    job.soften = std::clamp(ik.Soften, 0.0f, 1.0f);
    job.weight = std::clamp(ik.Weight, 0.0f, 1.0f);
    job.start_joint = &mImpl->Models[ik.StartJoint];
    job.mid_joint = &mImpl->Models[ik.MidJoint];
    job.end_joint = &mImpl->Models[ik.EndJoint];

    SimdQuaternion startCorrection;
    SimdQuaternion midCorrection;
    bool targetReached = false;
    job.start_joint_correction = &startCorrection;
    job.mid_joint_correction = &midCorrection;
    job.reached = &targetReached;
    if (!job.Run())
    {
        return false;
    }

    MultiplyLocalRotation(mImpl->Locals, ik.StartJoint, startCorrection);
    MultiplyLocalRotation(mImpl->Locals, ik.MidJoint, midCorrection);
    mImpl->LocalToModel(ik.StartJoint);

    if (reached)
    {
        *reached = targetReached;
    }
    return true;
}

bool AnimationPose::SolveAimIK(const AnimationAimIK& ik, bool* reached)
{
    if (!mImpl || ik.Joint < 0 || ik.Joint >= mImpl->JointCount())
    {
        return false;
    }

    using namespace ozz::math;
    ozz::animation::IKAimJob job;
    job.target = simd_float4::Load3PtrU(ik.Target);
    job.forward = NormalizeSafe3(simd_float4::Load3PtrU(ik.Forward), simd_float4::x_axis());
    job.up = NormalizeSafe3(simd_float4::Load3PtrU(ik.Up), simd_float4::z_axis());
    job.pole_vector = NormalizeSafe3(simd_float4::Load3PtrU(ik.PoleVector), simd_float4::z_axis());
    job.offset = simd_float4::Load3PtrU(ik.Offset);
    job.twist_angle = ik.TwistAngle;
    job.weight = std::clamp(ik.Weight, 0.0f, 1.0f);
    job.joint = &mImpl->Models[ik.Joint];

    SimdQuaternion correction;
    bool targetReached = false;
    job.joint_correction = &correction;
    job.reached = &targetReached;
    if (!job.Run())
    {
        return false;
    }

    MultiplyLocalRotation(mImpl->Locals, ik.Joint, correction);
    mImpl->LocalToModel(ik.Joint);

    if (reached)
    {
        *reached = targetReached;
    }
    return true;
}

const float* AnimationPose::GetModelMatrices() const
{
    return mImpl ? reinterpret_cast<const float*>(mImpl->Models.data()) : nullptr;
}

bool AnimationPose::GetJointModelPosition(int joint, float outPosition[3]) const
{
    if (!mImpl || joint < 0 || joint >= mImpl->JointCount())
    {
        return false;
    }
    const ozz::math::SimdFloat4& translation = mImpl->Models[joint].cols[3];
    outPosition[0] = Lane(translation, 0);
    outPosition[1] = Lane(translation, 1);
    outPosition[2] = Lane(translation, 2);
    return true;
}

int AnimationPose::GetSkinningMatrices(float* outMatrices, int maxJoints) const
{
    if (!mImpl || outMatrices == nullptr || maxJoints <= 0)
    {
        return 0;
    }

    // ozz is column-vector, so its Model * InverseBind is the row-vector
    // InverseBind * Model the header promises; the memory layouts are identical.
    const int count = (std::min)(maxJoints, mImpl->JointCount());
    for (int i = 0; i < count; ++i)
    {
        const ozz::math::Float4x4 skin = mImpl->Models[i] * mImpl->Skeleton->InverseBind[i];
        std::memcpy(outMatrices + i * 16, &skin, 16 * sizeof(float));
    }
    return count;
}

// ---- AnimationController ----------------------------------------------------------------

void AnimationController::Play(const AnimationClip* clip, float fadeSeconds, bool loop, float speed, bool restart)
{
    if (clip == nullptr)
    {
        Stop(fadeSeconds);
        return;
    }

    const bool fade = fadeSeconds > 0.0f;
    bool anythingAudible = false;
    for (const Track& track : mTracks)
    {
        anythingAudible |= track.Clip != nullptr && track.Weight > 0.0f;
    }

    if (mCurrent >= 0 && mTracks[mCurrent].Clip == clip)
    {
        Track& current = mTracks[mCurrent];
        current.Loop = loop;
        current.Speed = speed;
        if (restart)
        {
            current.Time = 0.0f;
        }
    }
    else
    {
        // A free slot, else the quietest track that is not the current one.
        int slot = -1;
        for (int i = 0; i < kMaxTracks && slot < 0; ++i)
        {
            if (mTracks[i].Clip == nullptr)
            {
                slot = i;
            }
        }
        if (slot < 0)
        {
            for (int i = 0; i < kMaxTracks; ++i)
            {
                if (i != mCurrent && (slot < 0 || mTracks[i].Weight < mTracks[slot].Weight))
                {
                    slot = i;
                }
            }
        }

        Track& track = mTracks[slot];
        track = Track{};
        track.Clip = clip;
        track.Speed = speed;
        track.Loop = loop;
        // Nothing to cross-fade from: start at full weight rather than fading in from the
        // rest pose.
        track.Weight = (fade && anythingAudible) ? 0.0f : 1.0f;
        mCurrent = slot;
    }

    for (int i = 0; i < kMaxTracks; ++i)
    {
        Track& track = mTracks[i];
        if (track.Clip == nullptr)
        {
            continue;
        }
        if (i == mCurrent)
        {
            track.FadeRate = fade ? 1.0f / fadeSeconds : 0.0f;
            if (!fade)
            {
                track.Weight = 1.0f;
            }
        }
        else if (fade)
        {
            track.FadeRate = -1.0f / fadeSeconds;
        }
        else
        {
            track = Track{};
        }
    }
}

void AnimationController::Stop(float fadeSeconds)
{
    for (Track& track : mTracks)
    {
        if (track.Clip == nullptr)
        {
            continue;
        }
        if (fadeSeconds > 0.0f)
        {
            track.FadeRate = -1.0f / fadeSeconds;
        }
        else
        {
            track = Track{};
        }
    }
    mCurrent = -1;
}

void AnimationController::Update(float deltaSeconds)
{
    for (int i = 0; i < kMaxTracks; ++i)
    {
        Track& track = mTracks[i];
        if (track.Clip == nullptr)
        {
            continue;
        }

        const float duration = track.Clip->GetDuration();
        track.Time += deltaSeconds * track.Speed;
        if (track.Loop && duration > 0.0f)
        {
            // Keep the time small so float precision does not decay over a long session.
            track.Time = ResolveClipTime(track.Time, duration, true);
        }
        else
        {
            track.Time = std::clamp(track.Time, 0.0f, (std::max)(duration, 0.0f));
        }

        track.Weight = std::clamp(track.Weight + track.FadeRate * deltaSeconds, 0.0f, 1.0f);
        if (track.FadeRate > 0.0f && track.Weight >= 1.0f)
        {
            track.FadeRate = 0.0f;
        }
        else if (track.FadeRate < 0.0f && track.Weight <= 0.0f)
        {
            track = Track{};
            if (i == mCurrent)
            {
                mCurrent = -1;
            }
        }
    }
}

int AnimationController::GetLayers(AnimationLayer* out, int maxLayers) const
{
    int count = 0;
    for (const Track& track : mTracks)
    {
        if (count >= maxLayers || out == nullptr)
        {
            break;
        }
        if (track.Clip == nullptr || track.Weight <= 0.0f)
        {
            continue;
        }

        AnimationLayer& layer = out[count++];
        layer = AnimationLayer{};
        layer.Clip = track.Clip;
        layer.Time = track.Time;
        layer.Weight = track.Weight;
        layer.Loop = track.Loop;
    }
    return count;
}

const AnimationClip* AnimationController::GetCurrentClip() const
{
    return mCurrent >= 0 ? mTracks[mCurrent].Clip : nullptr;
}

float AnimationController::GetPlaybackTime() const
{
    return mCurrent >= 0 ? mTracks[mCurrent].Time : 0.0f;
}

void AnimationController::SetPlaybackTime(float seconds)
{
    if (mCurrent >= 0)
    {
        Track& track = mTracks[mCurrent];
        track.Time = ResolveClipTime(seconds, track.Clip->GetDuration(), track.Loop);
    }
}

void AnimationController::SetSpeed(float speed)
{
    if (mCurrent >= 0)
    {
        mTracks[mCurrent].Speed = speed;
    }
}

bool AnimationController::IsFinished() const
{
    if (mCurrent < 0)
    {
        return true;
    }
    const Track& track = mTracks[mCurrent];
    return !track.Loop && track.Time >= track.Clip->GetDuration();
}
