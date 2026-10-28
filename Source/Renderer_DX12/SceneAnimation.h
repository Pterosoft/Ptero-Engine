#pragma once

// SceneAnimation
// --------------
// Every AnimatorComponent's playback, evaluated once a frame through Animations.dll.
//
// For each animated entity it keeps the loaded skeleton and clips, an AnimationPose and an
// AnimationController, and turns the evaluated pose into a skinning palette in the order of
// the mesh's own skin joints (MeshSkin::JointNames), matched to the skeleton's joints by
// name. EntityMeshRenderer reads that palette (GetPalette) to skin the mesh on the GPU.
//
// Like ScenePhysics, the state is keyed by Entity::Id and never saved: the component holds
// only the settings. Skeletons and clips are shared between entities that use the same file.
//
// In the editor, Animators with PreviewInEditor loop their default clip; while the game
// runs, Animators with PlayOnStart start it when play begins and the game can switch clips
// (Play). Entities without a skinned mesh, or whose skin shares no joint with the skeleton,
// get no palette and draw rigid.

#include "Components.h"
#include "AnimationRuntime.h"

#include <DirectXMath.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class SceneAnimation
{
public:
    // `gameRunning` switches between editor preview and play behaviour; a change of it
    // restarts every Animator so play always begins from the start of the default clip.
    void Update(std::vector<Entity>& entities, float deltaSeconds, bool gameRunning);
    void Clear();

    // Skinning palette for the entity this frame, one XMFLOAT4X4 (row-vector,
    // XMFLOAT4X4 layout, not transposed) per joint of its mesh's skin. Null when the entity
    // is not animated this frame.
    const std::vector<DirectX::XMFLOAT4X4>* GetPalette(std::uint64_t entityId) const;

    // ---- Gameplay -----------------------------------------------------------------------
    // `clip` is a clip's name (the FBX take name) or the file name of one of the Animator's
    // ClipPaths. A negative fade uses the Animator's BlendSeconds.
    bool Play(std::uint64_t entityId, const std::string& clip, float fadeSeconds = -1.0f, bool loop = true);
    bool Stop(std::uint64_t entityId, float fadeSeconds = -1.0f);
    bool SetSpeed(std::uint64_t entityId, float speed);
    // Model-space joint position moved into world space by the entity's transform.
    bool GetJointWorldPosition(const Entity& entity, const std::string& jointName, float outPosition[3]) const;

    // ---- Editor -------------------------------------------------------------------------
    struct Status
    {
        bool Active = false;            // has a palette this frame
        bool MeshHasSkin = false;
        int MeshJointCount = 0;
        int MatchedJointCount = 0;      // mesh skin joints the skeleton has
        int SkeletonJointCount = 0;
        std::string CurrentClip;
        float Time = 0.0f;
        float Duration = 0.0f;
        std::string Problem;            // why it is not animating, or empty
    };
    Status GetStatus(const Entity& entity) const;

    // Forgets the loaded copies of these files so the next frame reads them again (after a
    // re-import), and resets every Animator that used them.
    void ReloadAssets();

private:
    struct Instance
    {
        std::string ConfigKey;
        const Mesh* MeshPtr = nullptr;
        std::shared_ptr<const Mesh> MeshOwner;
        std::shared_ptr<AnimationSkeleton> Skeleton;
        std::vector<std::shared_ptr<AnimationClip>> Clips;
        std::unique_ptr<AnimationPose> Pose;
        AnimationController Controller;
        std::vector<int> MeshToSkeleton; // per mesh skin joint, -1 when the skeleton lacks it
        int MatchedJoints = 0;
        std::vector<float> SkinMatrices; // per skeleton joint
        std::vector<DirectX::XMFLOAT4X4> Palette;
        std::uint64_t LastFrame = 0;
        bool Active = false;
        float BlendSeconds = 0.2f;
        std::string Problem;
    };

    std::shared_ptr<AnimationSkeleton> LoadSkeleton(const std::string& path, std::string& problem);
    std::shared_ptr<AnimationClip> LoadClip(const std::string& path, std::string& problem);
    void Rebuild(Instance& instance, const Entity& entity, const std::string& configKey);
    void StartDefaultClip(Instance& instance, const AnimatorComponent& animator);
    const AnimationClip* FindClip(const Instance& instance, const std::string& name) const;

    std::unordered_map<std::uint64_t, Instance> mInstances;
    std::unordered_map<std::string, std::shared_ptr<AnimationSkeleton>> mSkeletons;
    std::unordered_map<std::string, std::shared_ptr<AnimationClip>> mClips;
    std::unordered_map<std::string, std::string> mLoadErrors;
    std::uint64_t mFrame = 0;
    bool mGameRunning = false;
};
