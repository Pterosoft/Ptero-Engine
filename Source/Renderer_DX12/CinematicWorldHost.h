#pragma once

#include "EntityProperties.h"

#include "System/Cinematics/SequencePlayer.h"

#include <DirectXMath.h>

#include <cstdint>
#include <unordered_map>
#include <vector>

struct Entity;

// What a camera entity sees, in the terms the renderer's camera takes.
struct CinematicCameraView
{
    DirectX::XMFLOAT3 Position{};
    float Pitch = 0.0f;
    float Yaw = 0.0f;
    float VerticalFovRadians = 0.785398f;
    float NearClip = 0.1f;
};

// Plays sequences against a level: the CinematicHost the game's CinematicSystem and the
// Sequencer's preview both use, so the editor previews exactly what the game will do.
//
// Bindings resolve to Entity* through an id index rebuilt only when it goes stale, so a
// sequence binding hundreds of entities costs a hash lookup per binding per frame, not a
// scan of the level. A binding whose id is not in this level falls back to the entity's
// name, which is what lets a sequence be reused in a level built separately.
//
// The cut camera is recorded rather than applied: who owns the view differs - a play
// session lets it override the game camera, the editor only when the viewport is locked -
// so the owner asks for it with GetCutCamera each frame.
class CinematicWorldHost final : public CinematicHost
{
public:
    void SetEntities(std::vector<Entity>* entities);
    void SetEnvironment(const EngineEnvironment& environment) { mEnvironment = environment; }

    void* ResolveBinding(const SequenceBinding& binding) override;
    void SetCutCamera(const SequenceBinding* camera, void* owner, double sequenceTime) override;

    // The entity id of the camera the playing sequence cuts to (0 for none), and the
    // sequence time it was last evaluated at, for the shake.
    std::uint64_t GetCutCameraEntity() const { return mCutCameraEntity; }
    double GetCutCameraTime() const { return mCutCameraTime; }
    void ClearCutCamera() { mCutCameraEntity = 0; }

    // The current view of the cut camera; false when there is none or it has gone.
    bool GetCutCameraView(CinematicCameraView& view);

    Entity* FindEntity(std::uint64_t entityId);

    // A camera entity's view. Works for any entity: one without a CameraComponent sees
    // with the default lens. `shakeTime` drives the camera's shake.
    static CinematicCameraView ComputeCameraView(const Entity& entity, double shakeTime);
    // The rotation that makes an entity look along pitch/yaw (no roll): Rotation.x =
    // pitch, Rotation.z = -yaw, in radians. The inverse of ComputeCameraView's aim.
    static DirectX::XMFLOAT3 RotationForView(float pitch, float yaw);

private:
    void RebuildIndex();

    std::vector<Entity>* mEntities = nullptr;
    EngineEnvironment mEnvironment;
    std::unordered_map<std::uint64_t, std::size_t> mIndexById;
    std::uint64_t mCutCameraEntity = 0;
    double mCutCameraTime = 0.0;
};
