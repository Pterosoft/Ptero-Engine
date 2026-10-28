#pragma once

// AnimationImport.h - Animations.dll: building .pskel / .panim assets.
//
// Two ways in:
//   - AnimationImportFbx: a rig and/or its takes straight from an FBX file, converted to
//     the engine's frame exactly as FbxCompiler converts meshes (metres, 3ds Max axes, then
//     X mirrored), so the skeleton fits the mesh cooked from the same file.
//   - AnimationBuildSkeleton / AnimationBuildClip: from raw data, for procedural content,
//     other importers and tests.
//
// Clips are compressed with ACL. Its error metric measures how far a virtual vertex
// `ShellDistance` metres from each joint drifts from the raw animation, and keeps that under
// `Precision` metres - 0.1 mm and 3 cm by default, ACL's own recommendation scaled to metres.
//
// All paths are UTF-8. Every function returns false and writes a message into `error` on
// failure.

#include "AnimationRuntime.h"

struct AnimationRawJoint
{
    const char* Name = nullptr; // must be unique within the skeleton
    int Parent = -1;            // index into the same array, -1 for a root
    AnimationTransform RestPose; // local
    // Model space to joint space at bind time. When absent the inverse of the rest pose's
    // model matrix is used.
    bool HasInverseBind = false;
    float InverseBind[16]{};
};

struct AnimationRawClip
{
    const char* Name = nullptr;
    float SampleRate = 30.0f;
    int SampleCount = 0;  // >= 1; Duration = (SampleCount - 1) / SampleRate
    int TrackCount = 0;
    // One joint name per track; tracks are matched to skeleton joints by name, and joints
    // with no track hold their rest pose.
    const char* const* TrackJointNames = nullptr;
    // SampleCount * TrackCount local transforms, sample-major: [sample * TrackCount + track].
    const AnimationTransform* Samples = nullptr;
    // Store the change from sample 0 instead of the absolute pose (see IsAdditive).
    bool MakeAdditive = false;
};

struct AnimationCompressionSettings
{
    float Precision = 0.0001f;     // metres
    float ShellDistance = 0.03f;   // metres
    int Level = -1;                // -1 lets ACL choose per clip; 0 (fastest) .. 4 (smallest)
};

ANIMATIONS_API bool AnimationBuildSkeleton(const AnimationRawJoint* joints, int jointCount,
                                           const char* outputPath,
                                           char* error, std::size_t errorSize);

ANIMATIONS_API bool AnimationBuildClip(const AnimationSkeleton& skeleton, const AnimationRawClip& clip,
                                       const AnimationCompressionSettings& settings,
                                       const char* outputPath,
                                       char* error, std::size_t errorSize);

struct AnimationFbxImportSettings
{
    float SampleRate = 30.0f;
    bool ImportSkeleton = true;
    bool ImportClips = true;
    bool MakeAdditive = false;
    // Build the clips against this existing skeleton instead of the one in the file: the
    // usual layout of one rig file plus one file per animation. Leave null to use (and,
    // with ImportSkeleton, write) the file's own.
    const char* SkeletonPath = nullptr;
    AnimationCompressionSettings Compression;
};

struct AnimationFbxImportResult
{
    int JointCount = 0;
    int ClipCount = 0;
    int SkinnedJointCount = 0; // joints that got their inverse bind from a skin cluster
};

// Writes <outputDirectory>/<baseName>.pskel and one <baseName>@<take>.panim per animation
// take (empty takes are skipped).
ANIMATIONS_API bool AnimationImportFbx(const char* fbxPath, const char* outputDirectory, const char* baseName,
                                       const AnimationFbxImportSettings& settings,
                                       AnimationFbxImportResult* result,
                                       char* error, std::size_t errorSize);
