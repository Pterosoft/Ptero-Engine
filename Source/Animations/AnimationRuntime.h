#pragma once

// AnimationRuntime.h - Animations.dll: skeletal animation for the engine.
//
// ozz-animation and ACL (the Animation Compression Library) never leave this DLL. ozz does
// the skeleton, blending, local-to-model and IK work; ACL stores the clips compressed and
// decompresses them as they are sampled. Like PhysicsWorld.h, everything here is plain data:
// floats, integers and pointers, no STL and no SDK types, so the DLL can be built with
// whatever SIMD level it likes without the caller having to match it.
//
// Space and conventions are the engine's: metres, left-handed, Z up - the same frame
// FbxCompiler bakes meshes into, so a skeleton imported from a rig lines up with the mesh
// imported from the same FBX. Quaternions are (x, y, z, w) and describe the same rotation
// DirectXMath's XMQuaternion* functions do. Matrices are 16 floats laid out exactly like a
// DirectX::XMFLOAT4X4 (row vectors, translation in elements 12..14), so they can be loaded
// with XMLoadFloat4x4 and handed to a shader unchanged.
//
// "Model space" is the skeleton's own space: the space the rig was authored in, which an
// entity's world matrix then places in the level. IK targets are given in model space.
//
// Assets:
//   .pskel - one skeleton: the joint hierarchy, rest pose and inverse bind matrices.
//   .panim - one ACL-compressed clip, bound to the skeleton it was built against.
// Both are written by AnimationImport.h.
//
// Typical use for a character:
//   AnimationSkeleton skeleton;   skeleton.LoadFromFile("Data/Animations/Hero.pskel", ...);
//   AnimationClip walk, run;      walk.LoadFromFile("Data/Animations/Hero@Walk.panim", ...);
//   AnimationPose pose;           pose.Initialize(skeleton);
//   AnimationController control;  control.Play(&walk);
//   every frame:
//     control.Update(dt);
//     AnimationLayer layers[AnimationController::kMaxTracks];
//     pose.Evaluate(layers, control.GetLayers(layers, AnimationController::kMaxTracks));
//     pose.SolveTwoBoneIK(...)      (optional)
//     pose.GetSkinningMatrices(palette, jointCount);

#include <cstddef>
#include <cstdint>

#ifdef ANIMATIONS_EXPORTS
#define ANIMATIONS_API __declspec(dllexport)
#else
#define ANIMATIONS_API __declspec(dllimport)
#endif

// A joint's local transform: translation, then rotation, then scale relative to its parent.
struct AnimationTransform
{
    float Translation[3]{ 0.0f, 0.0f, 0.0f };
    float Rotation[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
    float Scale[3]{ 1.0f, 1.0f, 1.0f };
};

// ---- Skeleton ---------------------------------------------------------------------------

class ANIMATIONS_API AnimationSkeleton
{
public:
    AnimationSkeleton();
    ~AnimationSkeleton();

    AnimationSkeleton(const AnimationSkeleton&) = delete;
    AnimationSkeleton& operator=(const AnimationSkeleton&) = delete;

    // `path` is UTF-8. LoadFromMemory is for packaged games, which read through DataFiles.
    // Both replace whatever was loaded before and leave the skeleton empty on failure.
    bool LoadFromFile(const char* path, char* error, std::size_t errorSize);
    bool LoadFromMemory(const void* data, std::size_t size, char* error, std::size_t errorSize);
    void Reset();

    bool IsLoaded() const;

    // Joints are ordered parents first (depth first), so a joint's parent always has a
    // lower index. Roots have parent -1.
    int GetJointCount() const;
    const char* GetJointName(int joint) const;
    int GetJointParent(int joint) const;
    int FindJoint(const char* name) const; // -1 when there is no joint of that name
    bool GetRestPose(int joint, AnimationTransform& out) const;

    // Model space to joint space at bind time, JointCount matrices. Skinning a vertex that
    // is in the mesh's bind (import) space with joint j is: vertex * InverseBind[j] *
    // Model[j]. Joints no skin referenced get the inverse of their rest-pose model matrix.
    const float* GetInverseBindMatrices() const;

    // Identifies the joint hierarchy (names and parents). Clips record the hash of the
    // skeleton they were built against.
    std::uint32_t GetHierarchyHash() const;

    struct Impl;
    const Impl* GetImpl() const { return mImpl; }

private:
    Impl* mImpl = nullptr;
};

// ---- Clip -------------------------------------------------------------------------------

class ANIMATIONS_API AnimationClip
{
public:
    AnimationClip();
    ~AnimationClip();

    AnimationClip(const AnimationClip&) = delete;
    AnimationClip& operator=(const AnimationClip&) = delete;

    bool LoadFromFile(const char* path, char* error, std::size_t errorSize);
    bool LoadFromMemory(const void* data, std::size_t size, char* error, std::size_t errorSize);
    void Reset();

    bool IsLoaded() const;

    const char* GetName() const;
    float GetDuration() const; // seconds
    float GetSampleRate() const; // samples per second the clip was baked at
    int GetTrackCount() const; // one per joint of the skeleton it was built against
    std::uint32_t GetSkeletonHash() const;

    // An additive clip stores each joint's change from a reference pose (its first frame,
    // usually) and is layered on top of whatever the normal layers produced.
    bool IsAdditive() const;

    // True when the clip was built against this skeleton's hierarchy.
    bool IsCompatibleWith(const AnimationSkeleton& skeleton) const;

    // Decompresses one joint at `time` (clamped to the clip) without needing a pose, for
    // root motion and gameplay queries. Not meant for whole skeletons - use AnimationPose.
    bool SampleJoint(int joint, float time, AnimationTransform& out) const;

    struct Impl;
    const Impl* GetImpl() const { return mImpl; }

private:
    Impl* mImpl = nullptr;
};

// ---- Pose -------------------------------------------------------------------------------

// One clip's contribution to a pose.
struct AnimationLayer
{
    const AnimationClip* Clip = nullptr;
    float Time = 0.0f;   // seconds; wrapped into the clip when Loop, clamped otherwise
    float Weight = 1.0f; // normal layers are normalised by their total weight; additive
                         // layers apply this fraction of their change
    bool Loop = true;
    // Optional per-joint weights (GetJointCount() floats, 0..1) that limit the layer to
    // part of the body, e.g. an upper-body wave over a walk. Null = the whole skeleton.
    const float* JointWeights = nullptr;
};

// Two-bone IK (hip-knee-ankle, shoulder-elbow-wrist). Rotates the start and mid joints so
// that the end joint reaches Target.
struct AnimationTwoBoneIK
{
    int StartJoint = -1;
    int MidJoint = -1;
    int EndJoint = -1;
    float Target[3]{ 0.0f, 0.0f, 0.0f };      // model space
    float PoleVector[3]{ 0.0f, 0.0f, 1.0f };  // model space: which way the chain bends
    float MidAxis[3]{ 0.0f, 0.0f, 1.0f };     // mid joint space: the hinge axis of the knee
    float TwistAngle = 0.0f;                  // radians around the start-to-target axis
    float Soften = 1.0f;                      // < 1 eases the chain out before it locks straight
    float Weight = 1.0f;
};

// Aim IK (head, eyes, a turret). Rotates one joint so that its Forward axis points at Target.
struct AnimationAimIK
{
    int Joint = -1;
    float Target[3]{ 0.0f, 0.0f, 0.0f };     // model space
    float Forward[3]{ 1.0f, 0.0f, 0.0f };    // joint space: the axis that should point at it
    float Up[3]{ 0.0f, 0.0f, 1.0f };         // joint space
    float PoleVector[3]{ 0.0f, 0.0f, 1.0f }; // model space: where Up should point
    float Offset[3]{ 0.0f, 0.0f, 0.0f };     // joint space: aim from here instead of the joint (eyes)
    float TwistAngle = 0.0f;
    float Weight = 1.0f;
};

// The evaluated pose of one skeleton instance - one per animated entity. Holds all the
// scratch memory sampling and blending need, so Evaluate never allocates once a pose has
// seen as many layers as it will be given.
class ANIMATIONS_API AnimationPose
{
public:
    AnimationPose();
    ~AnimationPose();

    AnimationPose(const AnimationPose&) = delete;
    AnimationPose& operator=(const AnimationPose&) = delete;

    // The skeleton must outlive the pose (or the next Initialize/Reset).
    bool Initialize(const AnimationSkeleton& skeleton);
    void Reset();

    bool IsInitialized() const;
    int GetJointCount() const;

    // Samples and blends the layers, then computes the model-space matrices. Layers whose
    // clip is missing or was built for another skeleton are skipped. With nothing left to
    // blend the result is the rest pose. Returns false only when the pose is not initialised.
    bool Evaluate(const AnimationLayer* layers, int layerCount);
    void SetToRestPose();

    // Local transforms, for procedural changes after Evaluate. Call UpdateModelSpace once
    // all edits are in.
    bool GetLocalTransform(int joint, AnimationTransform& out) const;
    bool SetLocalTransform(int joint, const AnimationTransform& transform);
    void UpdateModelSpace();

    // Both solvers need current model-space matrices (Evaluate or UpdateModelSpace first)
    // and refresh them below the joints they turn. `reached` (optional) reports whether
    // the target was reachable.
    bool SolveTwoBoneIK(const AnimationTwoBoneIK& ik, bool* reached = nullptr);
    bool SolveAimIK(const AnimationAimIK& ik, bool* reached = nullptr);

    // GetJointCount() model-space matrices.
    const float* GetModelMatrices() const;
    bool GetJointModelPosition(int joint, float outPosition[3]) const;

    // InverseBind[j] * Model[j] for the first `maxJoints` joints, 16 floats each: the
    // skinning palette a vertex shader multiplies bind-space vertices by.
    int GetSkinningMatrices(float* outMatrices, int maxJoints) const;

    struct Impl;

private:
    Impl* mImpl = nullptr;
};

// ---- Playback ---------------------------------------------------------------------------

// Plays clips with cross-fades. Holds no clip data itself: it only advances times and
// weights and turns them into AnimationLayers for AnimationPose::Evaluate. Plain data, so
// it can live inside a component and be copied freely.
class ANIMATIONS_API AnimationController
{
public:
    static constexpr int kMaxTracks = 4;

    // Starts `clip`, fading every other playing clip out over `fadeSeconds`. Playing the
    // clip that is already the current one only changes its settings, unless `restart`.
    void Play(const AnimationClip* clip, float fadeSeconds = 0.2f, bool loop = true,
              float speed = 1.0f, bool restart = false);
    void Stop(float fadeSeconds = 0.2f);
    void Update(float deltaSeconds);

    // Fills `out` with one layer per audible track; returns how many.
    int GetLayers(AnimationLayer* out, int maxLayers) const;

    const AnimationClip* GetCurrentClip() const;
    float GetPlaybackTime() const;
    void SetPlaybackTime(float seconds);
    void SetSpeed(float speed);
    // True once a non-looping current clip has played to its end (or nothing plays).
    bool IsFinished() const;

private:
    struct Track
    {
        const AnimationClip* Clip = nullptr;
        float Time = 0.0f;
        float Speed = 1.0f;
        float Weight = 0.0f;
        float FadeRate = 0.0f; // weight per second, negative while fading out
        bool Loop = true;
    };

    Track mTracks[kMaxTracks];
    int mCurrent = -1;
};
