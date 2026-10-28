#include "pch.h"

#include "SceneAnimation.h"

#include "System/DataFiles.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iterator>

using namespace DirectX;

namespace
{
    std::filesystem::path ResolveDataPath(const std::string& relativePath)
    {
        const std::filesystem::path path(relativePath);
        if (path.is_absolute())
            return path;
        const std::filesystem::path dataDirectory = DataFiles::FindDataDirectory();
        return dataDirectory.empty() ? path : dataDirectory / path;
    }

    std::string FileNameOf(const std::string& path)
    {
        return std::filesystem::path(path).filename().string();
    }

    std::string StemOf(const std::string& path)
    {
        return std::filesystem::path(path).stem().string();
    }

    const XMFLOAT4X4 kIdentity(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
}

std::shared_ptr<AnimationSkeleton> SceneAnimation::LoadSkeleton(const std::string& path, std::string& problem)
{
    if (const auto found = mSkeletons.find(path); found != mSkeletons.end())
        return found->second;
    if (const auto failed = mLoadErrors.find(path); failed != mLoadErrors.end())
    {
        problem = failed->second;
        return nullptr;
    }

    std::vector<std::uint8_t> bytes;
    char error[512] = {};
    auto skeleton = std::make_shared<AnimationSkeleton>();
    if (!DataFiles::ReadBytes(ResolveDataPath(path), bytes))
    {
        problem = "Cannot read skeleton '" + path + "'.";
    }
    else if (!skeleton->LoadFromMemory(bytes.data(), bytes.size(), error, sizeof(error)))
    {
        problem = "Skeleton '" + path + "': " + error;
    }
    else
    {
        mSkeletons.emplace(path, skeleton);
        return skeleton;
    }
    mLoadErrors.emplace(path, problem);
    return nullptr;
}

std::shared_ptr<AnimationClip> SceneAnimation::LoadClip(const std::string& path, std::string& problem)
{
    if (const auto found = mClips.find(path); found != mClips.end())
        return found->second;
    if (const auto failed = mLoadErrors.find(path); failed != mLoadErrors.end())
    {
        problem = failed->second;
        return nullptr;
    }

    std::vector<std::uint8_t> bytes;
    char error[512] = {};
    auto clip = std::make_shared<AnimationClip>();
    if (!DataFiles::ReadBytes(ResolveDataPath(path), bytes))
    {
        problem = "Cannot read clip '" + path + "'.";
    }
    else if (!clip->LoadFromMemory(bytes.data(), bytes.size(), error, sizeof(error)))
    {
        problem = "Clip '" + path + "': " + error;
    }
    else
    {
        mClips.emplace(path, clip);
        return clip;
    }
    mLoadErrors.emplace(path, problem);
    return nullptr;
}

void SceneAnimation::Rebuild(Instance& instance, const Entity& entity, const std::string& configKey)
{
    const AnimatorComponent& animator = *entity.Animator;
    const std::shared_ptr<Mesh>& mesh = entity.Mesh->MeshAsset;

    instance = Instance{};
    instance.ConfigKey = configKey;
    instance.MeshPtr = mesh.get();
    instance.MeshOwner = mesh;
    instance.BlendSeconds = (std::max)(animator.BlendSeconds, 0.0f);

    if (!mesh->HasSkin())
    {
        instance.Problem = "The mesh has no skin weights. Re-cook it from its FBX (the FBX must contain a skinned mesh).";
        return;
    }
    if (animator.SkeletonPath.empty())
    {
        instance.Problem = "No skeleton assigned.";
        return;
    }

    instance.Skeleton = LoadSkeleton(animator.SkeletonPath, instance.Problem);
    if (!instance.Skeleton)
        return;

    for (const std::string& clipPath : animator.ClipPaths)
    {
        std::string clipProblem;
        std::shared_ptr<AnimationClip> clip = clipPath.empty() ? nullptr : LoadClip(clipPath, clipProblem);
        if (clip && !clip->IsCompatibleWith(*instance.Skeleton))
        {
            clipProblem = "Clip '" + clipPath + "' was built for a different skeleton.";
            clip.reset();
        }
        if (!clipProblem.empty() && instance.Problem.empty())
            instance.Problem = clipProblem;
        // Kept even when null, so DefaultClip still indexes ClipPaths.
        instance.Clips.push_back(std::move(clip));
    }

    const MeshSkin& skin = *mesh->GetSkin();
    instance.MeshToSkeleton.resize(skin.JointNames.size(), -1);
    for (std::size_t i = 0; i < skin.JointNames.size(); ++i)
    {
        instance.MeshToSkeleton[i] = instance.Skeleton->FindJoint(skin.JointNames[i].c_str());
        instance.MatchedJoints += instance.MeshToSkeleton[i] >= 0 ? 1 : 0;
    }
    if (instance.MatchedJoints == 0)
    {
        instance.Problem = "None of the mesh's skin joints exist in the skeleton.";
        return;
    }

    instance.Pose = std::make_unique<AnimationPose>();
    instance.Pose->Initialize(*instance.Skeleton);
    instance.SkinMatrices.resize(static_cast<std::size_t>(instance.Skeleton->GetJointCount()) * 16u);
    instance.Palette.assign(skin.JointNames.size(), kIdentity);

    if (mGameRunning ? animator.PlayOnStart : animator.PreviewInEditor)
        StartDefaultClip(instance, animator);
}

void SceneAnimation::StartDefaultClip(Instance& instance, const AnimatorComponent& animator)
{
    if (animator.DefaultClip < 0 || animator.DefaultClip >= static_cast<int>(instance.Clips.size()))
        return;
    if (const AnimationClip* clip = instance.Clips[static_cast<std::size_t>(animator.DefaultClip)].get())
        instance.Controller.Play(clip, 0.0f, animator.Loop, animator.Speed, true);
}

void SceneAnimation::Update(std::vector<Entity>& entities, float deltaSeconds, bool gameRunning)
{
    ++mFrame;
    if (gameRunning != mGameRunning)
    {
        // Play begins (or ends) from the start of the default clip, not wherever the
        // editor preview had got to.
        mGameRunning = gameRunning;
        mInstances.clear();
    }

    const float dt = std::clamp(deltaSeconds, 0.0f, 0.1f);
    for (const Entity& entity : entities)
    {
        if (!entity.Animator || !entity.Mesh || !entity.Mesh->MeshAsset)
            continue;

        const AnimatorComponent& animator = *entity.Animator;
        Instance& instance = mInstances[entity.Id];
        std::string configKey = animator.SkeletonPath;
        for (const std::string& clip : animator.ClipPaths)
            configKey += "|" + clip;
        configKey += "|" + std::to_string(animator.DefaultClip) + (animator.Loop ? "L" : "") +
                     (animator.PreviewInEditor ? "P" : "") + (animator.PlayOnStart ? "S" : "") + "|" +
                     std::to_string(reinterpret_cast<std::uintptr_t>(entity.Mesh->MeshAsset.get()));
        if (configKey != instance.ConfigKey)
            Rebuild(instance, entity, configKey);
        instance.LastFrame = mFrame;
        instance.Active = false;

        if (!instance.Pose)
            continue;
        if (!gameRunning)
        {
            if (!animator.PreviewInEditor)
                continue;
            instance.Controller.SetSpeed(animator.Speed);
        }

        instance.Controller.Update(dt);
        AnimationLayer layers[AnimationController::kMaxTracks];
        const int layerCount = instance.Controller.GetLayers(layers, AnimationController::kMaxTracks);
        instance.Pose->Evaluate(layers, layerCount);

        const int jointCount = instance.Pose->GetSkinningMatrices(instance.SkinMatrices.data(), instance.Skeleton->GetJointCount());
        for (std::size_t i = 0; i < instance.MeshToSkeleton.size(); ++i)
        {
            const int joint = instance.MeshToSkeleton[i];
            if (joint >= 0 && joint < jointCount)
                std::memcpy(&instance.Palette[i], &instance.SkinMatrices[static_cast<std::size_t>(joint) * 16u], sizeof(XMFLOAT4X4));
            else
                instance.Palette[i] = kIdentity;
        }
        instance.Active = true;
    }

    for (auto it = mInstances.begin(); it != mInstances.end(); )
        it = (it->second.LastFrame != mFrame) ? mInstances.erase(it) : std::next(it);
}

void SceneAnimation::Clear()
{
    mInstances.clear();
}

void SceneAnimation::ReloadAssets()
{
    mInstances.clear();
    mSkeletons.clear();
    mClips.clear();
    mLoadErrors.clear();
}

const std::vector<XMFLOAT4X4>* SceneAnimation::GetPalette(std::uint64_t entityId) const
{
    const auto found = mInstances.find(entityId);
    return (found != mInstances.end() && found->second.Active) ? &found->second.Palette : nullptr;
}

const AnimationClip* SceneAnimation::FindClip(const Instance& instance, const std::string& name) const
{
    for (const auto& clip : instance.Clips)
    {
        if (clip && name == clip->GetName())
            return clip.get();
    }
    // Fall back to the file: "Hero@Walk.panim", "Hero@Walk" or just "Walk".
    for (const auto& [path, clip] : mClips)
    {
        if (std::find(instance.Clips.begin(), instance.Clips.end(), clip) == instance.Clips.end())
            continue;
        const std::string stem = StemOf(path);
        const std::size_t at = stem.find('@');
        if (name == FileNameOf(path) || name == stem || (at != std::string::npos && name == stem.substr(at + 1)))
            return clip.get();
    }
    return nullptr;
}

bool SceneAnimation::Play(std::uint64_t entityId, const std::string& clipName, float fadeSeconds, bool loop)
{
    const auto found = mInstances.find(entityId);
    if (found == mInstances.end() || !found->second.Pose)
        return false;
    Instance& instance = found->second;
    const AnimationClip* clip = FindClip(instance, clipName);
    if (clip == nullptr)
        return false;
    instance.Controller.Play(clip, fadeSeconds < 0.0f ? instance.BlendSeconds : fadeSeconds, loop);
    return true;
}

bool SceneAnimation::Stop(std::uint64_t entityId, float fadeSeconds)
{
    const auto found = mInstances.find(entityId);
    if (found == mInstances.end() || !found->second.Pose)
        return false;
    found->second.Controller.Stop(fadeSeconds < 0.0f ? found->second.BlendSeconds : fadeSeconds);
    return true;
}

bool SceneAnimation::SetSpeed(std::uint64_t entityId, float speed)
{
    const auto found = mInstances.find(entityId);
    if (found == mInstances.end() || !found->second.Pose)
        return false;
    found->second.Controller.SetSpeed(speed);
    return true;
}

bool SceneAnimation::GetJointWorldPosition(const Entity& entity, const std::string& jointName, float outPosition[3]) const
{
    const auto found = mInstances.find(entity.Id);
    if (found == mInstances.end() || !found->second.Pose || !found->second.Skeleton)
        return false;
    const int joint = found->second.Skeleton->FindJoint(jointName.c_str());
    float local[3];
    if (joint < 0 || !found->second.Pose->GetJointModelPosition(joint, local))
        return false;
    XMFLOAT3 world;
    XMStoreFloat3(&world, XMVector3TransformCoord(XMVectorSet(local[0], local[1], local[2], 1.0f), entity.Transform.GetTransform()));
    outPosition[0] = world.x;
    outPosition[1] = world.y;
    outPosition[2] = world.z;
    return true;
}

SceneAnimation::Status SceneAnimation::GetStatus(const Entity& entity) const
{
    Status status;
    if (entity.Mesh && entity.Mesh->MeshAsset)
    {
        status.MeshHasSkin = entity.Mesh->MeshAsset->HasSkin();
        if (status.MeshHasSkin)
            status.MeshJointCount = static_cast<int>(entity.Mesh->MeshAsset->GetSkin()->JointNames.size());
    }
    else
    {
        status.Problem = "The mesh is not loaded.";
    }

    const auto found = mInstances.find(entity.Id);
    if (found == mInstances.end())
        return status;

    const Instance& instance = found->second;
    status.Active = instance.Active;
    status.MatchedJointCount = instance.MatchedJoints;
    status.SkeletonJointCount = instance.Skeleton ? instance.Skeleton->GetJointCount() : 0;
    if (const AnimationClip* clip = instance.Controller.GetCurrentClip())
    {
        status.CurrentClip = clip->GetName();
        status.Time = instance.Controller.GetPlaybackTime();
        status.Duration = clip->GetDuration();
    }
    if (!instance.Problem.empty())
        status.Problem = instance.Problem;
    return status;
}
