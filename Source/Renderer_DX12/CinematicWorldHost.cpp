#include "pch.h"
#include "CinematicWorldHost.h"

#include "Components.h"

#include <cmath>

void CinematicWorldHost::SetEntities(std::vector<Entity>* entities)
{
    mEntities = entities;
    mIndexById.clear();
    RegisterEngineProperties();
}

void CinematicWorldHost::RebuildIndex()
{
    mIndexById.clear();
    if (mEntities == nullptr)
        return;
    mIndexById.reserve(mEntities->size());
    for (std::size_t i = 0; i < mEntities->size(); ++i)
    {
        if ((*mEntities)[i].Id != 0)
            mIndexById.emplace((*mEntities)[i].Id, i);
    }
}

Entity* CinematicWorldHost::FindEntity(std::uint64_t entityId)
{
    if (mEntities == nullptr || entityId == 0)
        return nullptr;

    // The index stores positions, which go stale whenever the level is edited; a hit is
    // verified, and a miss rebuilds once before concluding the entity is gone.
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        const auto it = mIndexById.find(entityId);
        if (it != mIndexById.end() && it->second < mEntities->size() && (*mEntities)[it->second].Id == entityId)
            return &(*mEntities)[it->second];
        if (attempt == 0)
            RebuildIndex();
    }
    return nullptr;
}

void* CinematicWorldHost::ResolveBinding(const SequenceBinding& binding)
{
    if (binding.Domain == "Environment")
        return &mEnvironment;
    if (binding.Domain != "Entity" || mEntities == nullptr)
        return nullptr;

    if (Entity* entity = FindEntity(binding.EntityId))
        return entity;

    if (!binding.EntityName.empty())
    {
        for (Entity& entity : *mEntities)
        {
            if (entity.Name == binding.EntityName)
                return &entity;
        }
    }
    return nullptr;
}

void CinematicWorldHost::SetCutCamera(const SequenceBinding*, void* owner, double sequenceTime)
{
    mCutCameraEntity = owner != nullptr ? static_cast<Entity*>(owner)->Id : 0;
    mCutCameraTime = sequenceTime;
}

bool CinematicWorldHost::GetCutCameraView(CinematicCameraView& view)
{
    const Entity* camera = FindEntity(mCutCameraEntity);
    if (camera == nullptr)
        return false;
    view = ComputeCameraView(*camera, mCutCameraTime);
    return true;
}

CinematicCameraView CinematicWorldHost::ComputeCameraView(const Entity& entity, double shakeTime)
{
    CinematicCameraView view;
    view.Position = entity.Transform.Position;

    // The camera looks along its local +Y; the view's pitch and yaw are read off that
    // direction, so any rotation the animator keys - including one with roll in it -
    // aims the view where the camera's body points.
    const DirectX::XMMATRIX rotation = PteroTransform::ComposeRotation(entity.Transform.Rotation);
    DirectX::XMFLOAT3 forward{};
    DirectX::XMStoreFloat3(&forward, DirectX::XMVector3TransformNormal(DirectX::XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), rotation));
    const float horizontal = std::sqrt(forward.x * forward.x + forward.y * forward.y);
    view.Yaw = std::atan2(forward.x, forward.y);
    view.Pitch = std::atan2(forward.z, horizontal);

    const CameraComponent lens = entity.Camera.value_or(CameraComponent{});
    view.VerticalFovRadians = lens.VerticalFovDegrees() * 0.017453292519943295f;
    view.NearClip = lens.NearClip;

    if (lens.ShakeAmplitudeDegrees > 0.0f && lens.ShakeFrequency > 0.0f)
    {
        // Two sums of sines at unrelated frequencies per axis: smooth, never visibly
        // periodic, and a pure function of time, so scrubbing back shows the same shake.
        const double t = shakeTime * lens.ShakeFrequency * 6.283185307179586;
        const double pitchNoise = 0.6 * std::sin(t * 1.0 + 0.3) + 0.4 * std::sin(t * 2.31 + 1.7);
        const double yawNoise = 0.6 * std::sin(t * 0.83 + 2.1) + 0.4 * std::sin(t * 1.97 + 0.4);
        const double amplitude = lens.ShakeAmplitudeDegrees * 0.017453292519943295;
        view.Pitch += static_cast<float>(pitchNoise * amplitude);
        view.Yaw += static_cast<float>(yawNoise * amplitude);
    }
    return view;
}

DirectX::XMFLOAT3 CinematicWorldHost::RotationForView(float pitch, float yaw)
{
    // ComposeRotation(x, 0, z) takes +Y to (-cos x sin z, cos x cos z, sin x); matching
    // the view direction (sin yaw cos pitch, cos yaw cos pitch, sin pitch) gives
    // x = pitch and z = -yaw.
    return DirectX::XMFLOAT3(pitch, 0.0f, -yaw);
}
