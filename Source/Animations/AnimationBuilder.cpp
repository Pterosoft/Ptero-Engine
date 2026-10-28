#include "pch.h"

#include "AnimationImport.h"
#include "AnimationInternal.h"

#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/local_to_model_job.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/quaternion.h>
#include <ozz/base/maths/transform.h>
#include <ozz/base/span.h>

#include <acl/compression/compress.h>
#include <acl/compression/track_array.h>
#include <acl/compression/transform_error_metrics.h>
#include <acl/core/ansi_allocator.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace AnimationDetail;

namespace
{
    ozz::math::Transform ToOzz(const AnimationTransform& t)
    {
        ozz::math::Transform out;
        out.translation = ozz::math::Float3(t.Translation[0], t.Translation[1], t.Translation[2]);
        out.rotation = ozz::math::Normalize(ozz::math::Quaternion(t.Rotation[0], t.Rotation[1], t.Rotation[2], t.Rotation[3]));
        out.scale = ozz::math::Float3(t.Scale[0], t.Scale[1], t.Scale[2]);
        return out;
    }

    bool IsFiniteTransform(const AnimationTransform& t)
    {
        for (float v : t.Translation) if (!std::isfinite(v)) return false;
        for (float v : t.Rotation) if (!std::isfinite(v)) return false;
        for (float v : t.Scale) if (!std::isfinite(v)) return false;
        return true;
    }

    void AddChildren(const AnimationRawJoint* joints, int jointCount, int parent,
                     ozz::animation::offline::RawSkeleton::Joint::Children& out)
    {
        for (int i = 0; i < jointCount; ++i)
        {
            if (joints[i].Parent != parent)
            {
                continue;
            }
            out.emplace_back();
            ozz::animation::offline::RawSkeleton::Joint& joint = out.back();
            joint.name = joints[i].Name;
            joint.transform = ToOzz(joints[i].RestPose);
            AddChildren(joints, jointCount, i, joint.children);
        }
    }

    // The change from `reference` to `value`, in the form ozz's BlendingJob adds back on
    // (see ozz's AdditiveAnimationBuilder).
    AnimationTransform MakeDelta(const AnimationTransform& reference, const AnimationTransform& value)
    {
        AnimationTransform delta;
        for (int i = 0; i < 3; ++i)
        {
            delta.Translation[i] = value.Translation[i] - reference.Translation[i];
            delta.Scale[i] = reference.Scale[i] != 0.0f ? value.Scale[i] / reference.Scale[i] : 1.0f;
        }
        const ozz::math::Quaternion ref(reference.Rotation[0], reference.Rotation[1], reference.Rotation[2], reference.Rotation[3]);
        const ozz::math::Quaternion val(value.Rotation[0], value.Rotation[1], value.Rotation[2], value.Rotation[3]);
        const ozz::math::Quaternion d = ozz::math::Normalize(ozz::math::Conjugate(ozz::math::Normalize(ref)) * ozz::math::Normalize(val));
        delta.Rotation[0] = d.x;
        delta.Rotation[1] = d.y;
        delta.Rotation[2] = d.z;
        delta.Rotation[3] = d.w;
        return delta;
    }
}

bool AnimationBuildSkeleton(const AnimationRawJoint* joints, int jointCount, const char* outputPath,
                            char* error, std::size_t errorSize)
{
    if (joints == nullptr || jointCount <= 0)
    {
        SetError(error, errorSize, "A skeleton needs at least one joint.");
        return false;
    }
    if (jointCount > ozz::animation::Skeleton::kMaxJoints)
    {
        SetError(error, errorSize, "%d joints is more than the %d a skeleton supports.",
                 jointCount, static_cast<int>(ozz::animation::Skeleton::kMaxJoints));
        return false;
    }

    std::unordered_map<std::string, int> rawIndexByName;
    for (int i = 0; i < jointCount; ++i)
    {
        const AnimationRawJoint& joint = joints[i];
        if (joint.Name == nullptr || joint.Name[0] == '\0')
        {
            SetError(error, errorSize, "Joint %d has no name.", i);
            return false;
        }
        if (!rawIndexByName.emplace(joint.Name, i).second)
        {
            SetError(error, errorSize, "Two joints are called '%s'; joint names must be unique.", joint.Name);
            return false;
        }
        if (joint.Parent < -1 || joint.Parent >= jointCount || joint.Parent == i)
        {
            SetError(error, errorSize, "Joint '%s' has an invalid parent index %d.", joint.Name, joint.Parent);
            return false;
        }
        if (!IsFiniteTransform(joint.RestPose))
        {
            SetError(error, errorSize, "Joint '%s' has a non-finite rest pose.", joint.Name);
            return false;
        }

        // Walking up must reach a root within jointCount steps, else there is a cycle.
        int steps = 0;
        for (int p = joint.Parent; p >= 0; p = joints[p].Parent)
        {
            if (++steps > jointCount)
            {
                SetError(error, errorSize, "Joint '%s' is part of a parent cycle.", joint.Name);
                return false;
            }
        }
    }

    ozz::animation::offline::RawSkeleton raw;
    AddChildren(joints, jointCount, -1, raw.roots);
    if (!raw.Validate())
    {
        SetError(error, errorSize, "The joint hierarchy is not valid.");
        return false;
    }

    const ozz::animation::offline::SkeletonBuilder builder;
    ozz::unique_ptr<ozz::animation::Skeleton> skeleton = builder(raw);
    if (!skeleton || skeleton->num_joints() != jointCount)
    {
        SetError(error, errorSize, "ozz could not build the skeleton.");
        return false;
    }

    // ozz orders joints depth first; the inverse binds have to follow its order.
    std::vector<ozz::math::Float4x4> restModels(static_cast<std::size_t>(jointCount));
    {
        ozz::animation::LocalToModelJob job;
        job.skeleton = skeleton.get();
        job.input = skeleton->joint_rest_poses();
        job.output = ozz::span<ozz::math::Float4x4>(restModels.data(), restModels.size());
        if (!job.Run())
        {
            SetError(error, errorSize, "Could not evaluate the rest pose.");
            return false;
        }
    }

    std::vector<ozz::math::Float4x4> inverseBind(static_cast<std::size_t>(jointCount));
    for (int i = 0; i < jointCount; ++i)
    {
        const AnimationRawJoint& rawJoint = joints[rawIndexByName.at(skeleton->joint_names()[i])];
        if (rawJoint.HasInverseBind)
        {
            std::memcpy(&inverseBind[i], rawJoint.InverseBind, sizeof(rawJoint.InverseBind));
        }
        else
        {
            inverseBind[i] = ozz::math::Invert(restModels[i]);
        }
    }

    ozz::io::MemoryStream stream;
    {
        ozz::io::OArchive archive(&stream);
        archive << *skeleton;
    }
    std::vector<std::uint8_t> archiveBytes(stream.Size());
    stream.Seek(0, ozz::io::Stream::kSet);
    stream.Read(archiveBytes.data(), archiveBytes.size());

    SkeletonFileHeader header{};
    std::memcpy(header.Magic, kSkeletonMagic, 4);
    header.Version = kSkeletonVersion;
    header.JointCount = static_cast<std::uint32_t>(jointCount);
    header.HierarchyHash = HashHierarchy(*skeleton);
    header.ArchiveSize = archiveBytes.size();

    std::vector<std::uint8_t> file;
    file.reserve(sizeof(header) + archiveBytes.size() + inverseBind.size() * 64);
    Append(file, header);
    AppendBytes(file, archiveBytes.data(), archiveBytes.size());
    for (const ozz::math::Float4x4& m : inverseBind)
    {
        AppendBytes(file, &m, 16 * sizeof(float));
    }
    return WriteWholeFile(outputPath, file, error, errorSize);
}

bool AnimationBuildClip(const AnimationSkeleton& skeleton, const AnimationRawClip& clip,
                        const AnimationCompressionSettings& settings, const char* outputPath,
                        char* error, std::size_t errorSize)
{
    if (!skeleton.IsLoaded())
    {
        SetError(error, errorSize, "The clip's skeleton is not loaded.");
        return false;
    }
    if (clip.SampleCount < 1 || clip.TrackCount < 0 || !(clip.SampleRate > 0.0f) ||
        (clip.TrackCount > 0 && (clip.Samples == nullptr || clip.TrackJointNames == nullptr)))
    {
        SetError(error, errorSize, "The clip has no samples.");
        return false;
    }

    const ozz::animation::Skeleton& ozzSkeleton = skeleton.GetImpl()->Skeleton;
    const int jointCount = ozzSkeleton.num_joints();

    // Skeleton joint -> clip track; -1 = not animated, holds its rest pose.
    std::vector<int> trackOfJoint(static_cast<std::size_t>(jointCount), -1);
    int matched = 0;
    for (int track = 0; track < clip.TrackCount; ++track)
    {
        const int joint = skeleton.FindJoint(clip.TrackJointNames[track]);
        if (joint >= 0 && trackOfJoint[joint] < 0)
        {
            trackOfJoint[joint] = track;
            ++matched;
        }
    }
    if (clip.TrackCount > 0 && matched == 0)
    {
        SetError(error, errorSize, "None of the clip's tracks names a joint of the skeleton.");
        return false;
    }

    std::vector<AnimationTransform> rest(static_cast<std::size_t>(jointCount));
    for (int joint = 0; joint < jointCount; ++joint)
    {
        skeleton.GetRestPose(joint, rest[joint]);
    }

    auto sampleOf = [&](int joint, int sample) -> AnimationTransform
    {
        const int track = trackOfJoint[joint];
        if (track < 0)
        {
            return rest[joint];
        }
        AnimationTransform t = clip.Samples[static_cast<std::size_t>(sample) * clip.TrackCount + track];
        if (!IsFiniteTransform(t))
        {
            t = rest[joint];
        }
        return t;
    };

    acl::ansi_allocator allocator;
    acl::track_array_qvvf rawTracks(allocator, static_cast<std::uint32_t>(jointCount));
    for (int joint = 0; joint < jointCount; ++joint)
    {
        acl::track_desc_transformf desc;
        desc.output_index = static_cast<std::uint32_t>(joint);
        const int parent = ozzSkeleton.joint_parents()[joint];
        desc.parent_index = parent < 0 ? acl::k_invalid_track_index : static_cast<std::uint32_t>(parent);
        desc.precision = settings.Precision > 0.0f ? settings.Precision : 0.0001f;
        desc.shell_distance = settings.ShellDistance > 0.0f ? settings.ShellDistance : 0.03f;

        acl::track_qvvf track = acl::track_qvvf::make_reserve(desc, allocator,
            static_cast<std::uint32_t>(clip.SampleCount), clip.SampleRate);

        const AnimationTransform reference = sampleOf(joint, 0);
        for (int sample = 0; sample < clip.SampleCount; ++sample)
        {
            AnimationTransform t = sampleOf(joint, sample);
            if (clip.MakeAdditive)
            {
                t = MakeDelta(reference, t);
            }
            const rtm::quatf rotation = rtm::quat_normalize(rtm::quat_set(t.Rotation[0], t.Rotation[1], t.Rotation[2], t.Rotation[3]));
            track[static_cast<std::uint32_t>(sample)] = rtm::qvv_set(rotation,
                rtm::vector_set(t.Translation[0], t.Translation[1], t.Translation[2]),
                rtm::vector_set(t.Scale[0], t.Scale[1], t.Scale[2]));
        }
        rawTracks[static_cast<std::uint32_t>(joint)] = std::move(track);
    }

    acl::compression_settings compression = acl::get_default_compression_settings();
    compression.level = settings.Level < 0
        ? acl::compression_level8::automatic
        : static_cast<acl::compression_level8>(std::clamp(settings.Level, 0, 4));
    acl::qvvf_transform_error_metric errorMetric;
    compression.error_metric = &errorMetric;

    acl::output_stats stats;
    acl::compressed_tracks* compressed = nullptr;
    const acl::error_result result = acl::compress_track_list(allocator, rawTracks, compression, compressed, stats);
    if (result.any() || compressed == nullptr)
    {
        SetError(error, errorSize, "ACL could not compress the clip: %s", result.c_str());
        return false;
    }

    const std::string name = clip.Name ? clip.Name : "";
    ClipFileHeader header{};
    std::memcpy(header.Magic, kClipMagic, 4);
    header.Version = kClipVersion;
    header.TrackCount = static_cast<std::uint32_t>(jointCount);
    header.SkeletonHash = skeleton.GetHierarchyHash();
    header.Flags = clip.MakeAdditive ? ClipFlagAdditive : 0u;
    header.Duration = static_cast<float>(clip.SampleCount - 1) / clip.SampleRate;
    header.SampleRate = clip.SampleRate;
    header.NameLength = static_cast<std::uint32_t>(name.size());
    header.DataSize = compressed->get_size();

    std::vector<std::uint8_t> file;
    file.reserve(sizeof(header) + name.size() + compressed->get_size());
    Append(file, header);
    AppendBytes(file, name.data(), name.size());
    AppendBytes(file, compressed, compressed->get_size());
    allocator.deallocate(compressed, compressed->get_size());

    return WriteWholeFile(outputPath, file, error, errorSize);
}
