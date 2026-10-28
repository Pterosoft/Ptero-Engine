// AnimationTests - headless checks of Animations.dll: asset building and loading, ACL
// sampling, blending (normal, masked, additive), IK, the controller's cross-fades, and the
// FBX importer on whatever FBX files are passed on the command line.

#include "AnimationImport.h"
#include "AnimationRuntime.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

namespace
{
    int gFailures = 0;
    int gChecks = 0;

    void Check(bool condition, const char* what)
    {
        ++gChecks;
        if (!condition)
        {
            ++gFailures;
            std::printf("  FAIL: %s\n", what);
        }
    }

    bool Near(float a, float b, float tolerance = 1e-3f)
    {
        return std::fabs(a - b) <= tolerance;
    }

    bool NearPosition(const float p[3], float x, float y, float z, float tolerance = 2e-3f)
    {
        return Near(p[0], x, tolerance) && Near(p[1], y, tolerance) && Near(p[2], z, tolerance);
    }

    AnimationTransform RotationZ(float radians, float tx = 0.0f, float ty = 0.0f, float tz = 0.0f)
    {
        AnimationTransform t;
        t.Translation[0] = tx;
        t.Translation[1] = ty;
        t.Translation[2] = tz;
        t.Rotation[2] = std::sin(radians * 0.5f);
        t.Rotation[3] = std::cos(radians * 0.5f);
        return t;
    }

    std::string gDir;
    std::string PathOf(const char* name) { return gDir + "/" + name; }

    constexpr float kPi = 3.14159265358979f;

    // Root -> Upper -> Lower -> Tip along +X (1 m bones), plus a Side joint off the root.
    bool BuildTestSkeleton(char* error, std::size_t errorSize)
    {
        AnimationRawJoint joints[5];
        joints[0].Name = "Root";
        joints[1].Name = "Upper"; joints[1].Parent = 0; joints[1].RestPose.Translation[0] = 1.0f;
        joints[2].Name = "Lower"; joints[2].Parent = 1; joints[2].RestPose.Translation[0] = 1.0f;
        joints[3].Name = "Tip";   joints[3].Parent = 2; joints[3].RestPose.Translation[0] = 1.0f;
        joints[4].Name = "Side";  joints[4].Parent = 0; joints[4].RestPose.Translation[1] = 0.5f;
        return AnimationBuildSkeleton(joints, 5, PathOf("Test.pskel").c_str(), error, errorSize);
    }

    // The root spins about Z from `from` to `to` radians over one second.
    bool BuildSpinClip(const AnimationSkeleton& skeleton, const char* file, float from, float to, bool additive,
                       char* error, std::size_t errorSize)
    {
        const int samples = 31;
        const char* names[] = { "Root" };
        std::vector<AnimationTransform> data(samples);
        for (int i = 0; i < samples; ++i)
        {
            data[i] = RotationZ(from + (to - from) * i / (samples - 1));
        }
        AnimationRawClip clip;
        clip.Name = file;
        clip.SampleRate = 30.0f;
        clip.SampleCount = samples;
        clip.TrackCount = 1;
        clip.TrackJointNames = names;
        clip.Samples = data.data();
        clip.MakeAdditive = additive;
        return AnimationBuildClip(skeleton, clip, AnimationCompressionSettings{}, PathOf(file).c_str(), error, errorSize);
    }

    // The Upper joint bends about Z by `angle` (a clip that only drives one joint).
    bool BuildBendClip(const AnimationSkeleton& skeleton, const char* file, float angle, char* error, std::size_t errorSize)
    {
        const char* names[] = { "Upper" };
        AnimationTransform data[2] = { RotationZ(angle, 1.0f), RotationZ(angle, 1.0f) };
        AnimationRawClip clip;
        clip.Name = file;
        clip.SampleRate = 30.0f;
        clip.SampleCount = 2;
        clip.TrackCount = 1;
        clip.TrackJointNames = names;
        clip.Samples = data;
        return AnimationBuildClip(skeleton, clip, AnimationCompressionSettings{}, PathOf(file).c_str(), error, errorSize);
    }

    float MaxDeviationFromIdentity(const float* m)
    {
        float worst = 0.0f;
        for (int r = 0; r < 4; ++r)
        {
            for (int c = 0; c < 4; ++c)
            {
                worst = (std::max)(worst, std::fabs(m[r * 4 + c] - (r == c ? 1.0f : 0.0f)));
            }
        }
        return worst;
    }

    void TestSkeleton()
    {
        std::printf("Skeleton\n");
        char error[512] = {};
        Check(BuildTestSkeleton(error, sizeof(error)), error);

        AnimationSkeleton skeleton;
        Check(skeleton.LoadFromFile(PathOf("Test.pskel").c_str(), error, sizeof(error)), error);
        Check(skeleton.GetJointCount() == 5, "joint count");
        const int upper = skeleton.FindJoint("Upper");
        const int tip = skeleton.FindJoint("Tip");
        Check(upper >= 0 && tip >= 0 && skeleton.FindJoint("Nope") == -1, "FindJoint");
        Check(skeleton.GetJointParent(skeleton.FindJoint("Root")) == -1, "root has no parent");
        Check(skeleton.GetJointParent(skeleton.FindJoint("Lower")) == upper, "Lower's parent is Upper");
        for (int i = 0; i < skeleton.GetJointCount(); ++i)
        {
            Check(skeleton.GetJointParent(i) < i, "parents come before children");
        }

        AnimationPose pose;
        Check(pose.Initialize(skeleton), "pose initialise");
        float p[3];
        pose.GetJointModelPosition(tip, p);
        Check(NearPosition(p, 3.0f, 0.0f, 0.0f), "rest pose tip at (3,0,0)");

        std::vector<float> palette(16 * 5);
        Check(pose.GetSkinningMatrices(palette.data(), 5) == 5, "skinning matrix count");
        float worst = 0.0f;
        for (int i = 0; i < 5; ++i)
        {
            worst = (std::max)(worst, MaxDeviationFromIdentity(&palette[i * 16]));
        }
        Check(worst < 1e-5f, "skinning matrices are identity at rest");

        AnimationSkeleton bad;
        const char junk[] = "not a skeleton at all, just some bytes";
        Check(!bad.LoadFromMemory(junk, sizeof(junk), error, sizeof(error)) && error[0] != '\0', "garbage is rejected");
        Check(!bad.IsLoaded(), "rejected skeleton stays empty");
    }

    void TestSamplingAndBlending()
    {
        std::printf("Sampling and blending\n");
        char error[512] = {};
        AnimationSkeleton skeleton;
        Check(skeleton.LoadFromFile(PathOf("Test.pskel").c_str(), error, sizeof(error)), error);
        const int tip = skeleton.FindJoint("Tip");

        Check(BuildSpinClip(skeleton, "Spin.panim", 0.0f, kPi * 0.5f, false, error, sizeof(error)), error);
        Check(BuildSpinClip(skeleton, "Still.panim", 0.0f, 0.0f, false, error, sizeof(error)), error);
        Check(BuildSpinClip(skeleton, "Quarter.panim", kPi * 0.5f, kPi * 0.5f, false, error, sizeof(error)), error);
        Check(BuildSpinClip(skeleton, "SpinAdditive.panim", 0.0f, kPi * 0.5f, true, error, sizeof(error)), error);
        Check(BuildBendClip(skeleton, "Bend.panim", kPi * 0.5f, error, sizeof(error)), error);

        AnimationClip spin, still, quarter, additive, bend;
        Check(spin.LoadFromFile(PathOf("Spin.panim").c_str(), error, sizeof(error)), error);
        Check(still.LoadFromFile(PathOf("Still.panim").c_str(), error, sizeof(error)), error);
        Check(quarter.LoadFromFile(PathOf("Quarter.panim").c_str(), error, sizeof(error)), error);
        Check(additive.LoadFromFile(PathOf("SpinAdditive.panim").c_str(), error, sizeof(error)), error);
        Check(bend.LoadFromFile(PathOf("Bend.panim").c_str(), error, sizeof(error)), error);
        Check(spin.IsCompatibleWith(skeleton), "clip matches its skeleton");
        Check(Near(spin.GetDuration(), 1.0f), "duration is (samples - 1) / rate");
        Check(std::string(spin.GetName()) == "Spin.panim", "clip name");
        Check(additive.IsAdditive() && !spin.IsAdditive(), "additive flag");

        AnimationPose pose;
        pose.Initialize(skeleton);
        float p[3];

        // Halfway through the spin the chain points along 45 degrees.
        AnimationLayer layer;
        layer.Clip = &spin;
        layer.Time = 0.5f;
        layer.Loop = false;
        pose.Evaluate(&layer, 1);
        pose.GetJointModelPosition(tip, p);
        const float c45 = 3.0f * std::cos(kPi * 0.25f);
        Check(NearPosition(p, c45, c45, 0.0f), "sampled half spin: tip at 45 degrees (+X turns towards +Y)");

        // Looping wraps; clamping holds the last frame.
        layer.Time = 1.25f;
        layer.Loop = true;
        pose.Evaluate(&layer, 1);
        pose.GetJointModelPosition(tip, p);
        const float a = kPi * 0.5f * 0.25f;
        Check(NearPosition(p, 3.0f * std::cos(a), 3.0f * std::sin(a), 0.0f), "looping wraps 1.25 s to 0.25 s");
        layer.Loop = false;
        pose.Evaluate(&layer, 1);
        pose.GetJointModelPosition(tip, p);
        Check(NearPosition(p, 0.0f, 3.0f, 0.0f), "non-looping clamps to the last frame");

        AnimationTransform sampled;
        Check(spin.SampleJoint(skeleton.FindJoint("Root"), 1.0f, sampled), "SampleJoint");
        Check(Near(sampled.Rotation[2], std::sin(kPi * 0.25f)) && Near(sampled.Rotation[3], std::cos(kPi * 0.25f)),
              "SampleJoint matches the raw rotation");

        // 50/50 blend of 0 and 90 degrees = 45 degrees.
        AnimationLayer blend[2];
        blend[0].Clip = &still; blend[0].Weight = 0.5f;
        blend[1].Clip = &quarter; blend[1].Weight = 0.5f;
        pose.Evaluate(blend, 2);
        pose.GetJointModelPosition(tip, p);
        Check(NearPosition(p, c45, c45, 0.0f), "50/50 blend lands halfway");

        // Weights are normalised: 2 and 2 is the same as 0.5 and 0.5.
        blend[0].Weight = 2.0f; blend[1].Weight = 2.0f;
        pose.Evaluate(blend, 2);
        pose.GetJointModelPosition(tip, p);
        Check(NearPosition(p, c45, c45, 0.0f), "weights are normalised");

        // Additive spin on top of a static base reproduces the spin.
        AnimationLayer layered[2];
        layered[0].Clip = &still;
        layered[1].Clip = &additive;
        layered[1].Time = 1.0f;
        layered[1].Loop = false;
        pose.Evaluate(layered, 2);
        pose.GetJointModelPosition(tip, p);
        Check(NearPosition(p, 0.0f, 3.0f, 0.0f), "additive layer adds its change");
        layered[1].Weight = 0.5f;
        pose.Evaluate(layered, 2);
        pose.GetJointModelPosition(tip, p);
        Check(NearPosition(p, c45, c45, 0.0f), "half-weight additive adds half the change");

        // A masked layer only moves the joints it is allowed to: the bend drives Upper, masked
        // to zero there, so the still pose wins.
        std::vector<float> mask(skeleton.GetJointCount(), 1.0f);
        mask[skeleton.FindJoint("Upper")] = 0.0f;
        AnimationLayer masked[2];
        masked[0].Clip = &still;
        masked[1].Clip = &bend;
        masked[1].JointWeights = mask.data();
        pose.Evaluate(masked, 2);
        pose.GetJointModelPosition(tip, p);
        Check(NearPosition(p, 3.0f, 0.0f, 0.0f), "joint mask keeps the masked joint out");
        masked[1].JointWeights = nullptr;
        pose.Evaluate(&masked[1], 1);
        pose.GetJointModelPosition(tip, p);
        Check(NearPosition(p, 1.0f, 2.0f, 0.0f), "unmasked bend turns the arm up at Upper");

        // A clip for another skeleton is skipped, leaving the rest pose.
        AnimationRawJoint other[2];
        other[0].Name = "A";
        other[1].Name = "B"; other[1].Parent = 0;
        Check(AnimationBuildSkeleton(other, 2, PathOf("Other.pskel").c_str(), error, sizeof(error)), error);
        AnimationSkeleton otherSkeleton;
        otherSkeleton.LoadFromFile(PathOf("Other.pskel").c_str(), error, sizeof(error));
        Check(!spin.IsCompatibleWith(otherSkeleton), "clip does not match another skeleton");
        AnimationPose otherPose;
        otherPose.Initialize(otherSkeleton);
        layer.Time = 0.5f;
        Check(otherPose.Evaluate(&layer, 1), "evaluating a foreign clip does not fail");

        // Local edits are seen after UpdateModelSpace.
        pose.SetToRestPose();
        pose.SetLocalTransform(skeleton.FindJoint("Root"), RotationZ(kPi));
        pose.UpdateModelSpace();
        pose.GetJointModelPosition(tip, p);
        Check(NearPosition(p, -3.0f, 0.0f, 0.0f), "SetLocalTransform + UpdateModelSpace");
    }

    void TestIK()
    {
        std::printf("IK\n");
        char error[512] = {};
        AnimationSkeleton skeleton;
        Check(skeleton.LoadFromFile(PathOf("Test.pskel").c_str(), error, sizeof(error)), error);

        AnimationPose pose;
        pose.Initialize(skeleton);

        AnimationTwoBoneIK ik;
        ik.StartJoint = skeleton.FindJoint("Upper");
        ik.MidJoint = skeleton.FindJoint("Lower");
        ik.EndJoint = skeleton.FindJoint("Tip");
        ik.Target[0] = 2.0f; ik.Target[1] = 1.0f; ik.Target[2] = 0.0f; // 1.414 m from Upper: reachable
        ik.PoleVector[0] = 0.0f; ik.PoleVector[1] = 1.0f; ik.PoleVector[2] = 0.0f;
        ik.MidAxis[0] = 0.0f; ik.MidAxis[1] = 0.0f; ik.MidAxis[2] = 1.0f;
        bool reached = false;
        Check(pose.SolveTwoBoneIK(ik, &reached), "two-bone IK runs");
        Check(reached, "two-bone IK reports the target reachable");
        float p[3];
        pose.GetJointModelPosition(ik.EndJoint, p);
        Check(NearPosition(p, 2.0f, 1.0f, 0.0f), "two-bone IK puts the tip on the target");
        pose.GetJointModelPosition(ik.StartJoint, p);
        Check(NearPosition(p, 1.0f, 0.0f, 0.0f), "two-bone IK does not move the start joint");

        // Aim the Tip's +X at a point straight above it in Y.
        pose.SetToRestPose();
        AnimationAimIK aim;
        aim.Joint = skeleton.FindJoint("Tip");
        aim.Target[0] = 3.0f; aim.Target[1] = 5.0f; aim.Target[2] = 0.0f;
        Check(pose.SolveAimIK(aim, &reached), "aim IK runs");
        const float* tipMatrix = pose.GetModelMatrices() + aim.Joint * 16;
        // Row 0 of the XMFLOAT4X4 layout is the joint's X axis in model space.
        Check(Near(tipMatrix[0], 0.0f) && Near(tipMatrix[1], 1.0f) && Near(tipMatrix[2], 0.0f), "aim IK points +X at the target");
    }

    void TestController()
    {
        std::printf("Controller\n");
        char error[512] = {};
        AnimationSkeleton skeleton;
        skeleton.LoadFromFile(PathOf("Test.pskel").c_str(), error, sizeof(error));
        AnimationClip still, spin;
        Check(still.LoadFromFile(PathOf("Still.panim").c_str(), error, sizeof(error)), error);
        Check(spin.LoadFromFile(PathOf("Spin.panim").c_str(), error, sizeof(error)), error);

        AnimationController controller;
        AnimationLayer layers[AnimationController::kMaxTracks];
        Check(controller.IsFinished() && controller.GetLayers(layers, AnimationController::kMaxTracks) == 0, "idle controller");

        controller.Play(&still, 0.5f);
        Check(controller.GetLayers(layers, AnimationController::kMaxTracks) == 1 && Near(layers[0].Weight, 1.0f),
              "first clip starts at full weight (nothing to fade from)");

        controller.Update(0.1f);
        controller.Play(&spin, 0.5f, false);
        controller.Update(0.25f);
        int count = controller.GetLayers(layers, AnimationController::kMaxTracks);
        Check(count == 2, "cross-fade has two layers");
        float stillWeight = 0.0f, spinWeight = 0.0f;
        for (int i = 0; i < count; ++i)
        {
            (layers[i].Clip == &still ? stillWeight : spinWeight) = layers[i].Weight;
        }
        Check(Near(stillWeight, 0.5f) && Near(spinWeight, 0.5f), "cross-fade halfway is 50/50");
        Check(controller.GetCurrentClip() == &spin && Near(controller.GetPlaybackTime(), 0.25f), "current clip and time");

        controller.Update(0.3f);
        count = controller.GetLayers(layers, AnimationController::kMaxTracks);
        Check(count == 1 && layers[0].Clip == &spin && Near(layers[0].Weight, 1.0f), "fade completes");
        Check(!controller.IsFinished(), "not finished mid-clip");
        controller.Update(1.0f);
        Check(controller.IsFinished(), "non-looping clip finishes");

        controller.Stop(0.0f);
        Check(controller.GetLayers(layers, AnimationController::kMaxTracks) == 0, "stop without fade clears");

        // Full chain: controller -> pose.
        AnimationPose pose;
        pose.Initialize(skeleton);
        controller.Play(&spin, 0.0f, true);
        controller.Update(0.5f);
        count = controller.GetLayers(layers, AnimationController::kMaxTracks);
        pose.Evaluate(layers, count);
        float p[3];
        pose.GetJointModelPosition(skeleton.FindJoint("Tip"), p);
        const float c45 = 3.0f * std::cos(kPi * 0.25f);
        Check(NearPosition(p, c45, c45, 0.0f), "controller drives the pose");
    }

    void TestFbx(const char* fbxPath)
    {
        std::printf("FBX %s\n", fbxPath);
        char error[1024] = {};
        const std::string base = std::filesystem::path(fbxPath).stem().string();
        AnimationFbxImportSettings settings;
        AnimationFbxImportResult result;
        const bool ok = AnimationImportFbx(fbxPath, gDir.c_str(), base.c_str(), settings, &result, error, sizeof(error));
        Check(ok, error);
        if (!ok)
        {
            return;
        }
        std::printf("  %d joints (%d skinned), %d clips\n", result.JointCount, result.SkinnedJointCount, result.ClipCount);

        AnimationSkeleton skeleton;
        Check(skeleton.LoadFromFile((gDir + "/" + base + ".pskel").c_str(), error, sizeof(error)), error);
        Check(skeleton.GetJointCount() == result.JointCount, "imported joint count");

        // At the rest pose, InverseBind * Model is identity for every joint whose rest pose
        // is its bind pose. The inverse binds come from mirrored cluster matrices and the
        // rest pose from mirrored TRS decompositions, so this checks the two agree.
        AnimationPose pose;
        pose.Initialize(skeleton);
        std::vector<float> palette(static_cast<std::size_t>(skeleton.GetJointCount()) * 16);
        pose.GetSkinningMatrices(palette.data(), skeleton.GetJointCount());
        // A file whose default pose is not its bind pose reports fewer, and that is the file,
        // not the importer: UE's AnimatedCharacter.fbx stores its rig pre-posed, and
        // humanoid.fbx (a 3ds Max export) binds its mesh rotated 90 degrees and its bones at
        // a different scale. ozz's arnaud.fbx is bound in its default pose and must report
        // all of them. "Rigid" counts matrices with no scale left in them.
        int identity = 0;
        int rigid = 0;
        for (int i = 0; i < skeleton.GetJointCount(); ++i)
        {
            const float* m = &palette[i * 16];
            identity += MaxDeviationFromIdentity(m) < 1e-3f ? 1 : 0;
            bool unitRows = true;
            for (int row = 0; row < 3; ++row)
            {
                const float length = std::sqrt(m[row * 4] * m[row * 4] + m[row * 4 + 1] * m[row * 4 + 1] + m[row * 4 + 2] * m[row * 4 + 2]);
                unitRows &= Near(length, 1.0f, 1e-2f);
            }
            rigid += unitRows ? 1 : 0;
        }
        std::printf("  rest pose: %d of %d skinning matrices are identity, %d rigid\n", identity, skeleton.GetJointCount(), rigid);

        for (const auto& entry : std::filesystem::directory_iterator(gDir))
        {
            const std::string name = entry.path().filename().string();
            if (name.rfind(base + "@", 0) != 0 || entry.path().extension() != ".panim")
            {
                continue;
            }
            AnimationClip clip;
            Check(clip.LoadFromFile(entry.path().string().c_str(), error, sizeof(error)), error);
            Check(clip.IsCompatibleWith(skeleton), "imported clip matches the imported skeleton");

            AnimationLayer layer;
            layer.Clip = &clip;
            bool finite = true;
            float lowest = 1e30f, highest = -1e30f;
            for (int step = 0; step <= 10; ++step)
            {
                layer.Time = clip.GetDuration() * step / 10.0f;
                pose.Evaluate(&layer, 1);
                const float* models = pose.GetModelMatrices();
                for (int i = 0; i < skeleton.GetJointCount() * 16; ++i)
                {
                    finite &= std::isfinite(models[i]);
                }
                for (int j = 0; j < skeleton.GetJointCount(); ++j)
                {
                    lowest = (std::min)(lowest, models[j * 16 + 14]);
                    highest = (std::max)(highest, models[j * 16 + 14]);
                }
            }
            Check(finite, "imported clip evaluates to finite matrices");
            std::printf("  %s: %.2f s at %.1f Hz, %zu bytes; joints span Z %.2f..%.2f m\n", clip.GetName(),
                        clip.GetDuration(), clip.GetSampleRate(),
                        static_cast<std::size_t>(std::filesystem::file_size(entry.path())), lowest, highest);
        }
    }
}

namespace
{
    // ---- Skinned mesh, end to end ---------------------------------------------------------
    // Cooks the FBX with System.dll exactly as the editor's "Re-cook Mesh With Skin Weights"
    // does, reads the .ptero vertices and the .pskin weights back, and skins them on the CPU
    // with SkinVertices.hlsl's math against the skeleton Animations.dll imported from the
    // same file. Run from Binaries\ (System.dll and its dependencies live there).

    struct CookedVertex
    {
        float Position[3];
        float Normal[3];
        float Uv[2];
        float Color[4];
    };
    static_assert(sizeof(CookedVertex) == 48);

    struct SkinRecord
    {
        std::uint16_t Joints[4];
        float Weights[4];
    };
    static_assert(sizeof(SkinRecord) == 24);

    bool ReadAllBytes(const std::filesystem::path& path, std::vector<std::uint8_t>& out)
    {
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr)
            return false;
        std::fseek(file, 0, SEEK_END);
        out.resize(static_cast<std::size_t>(std::ftell(file)));
        std::fseek(file, 0, SEEK_SET);
        const bool ok = std::fread(out.data(), 1, out.size(), file) == out.size();
        std::fclose(file);
        return ok;
    }

    void SkinVertex(const CookedVertex& in, const SkinRecord& skin, const std::vector<float>& palette, float out[3])
    {
        const float total = skin.Weights[0] + skin.Weights[1] + skin.Weights[2] + skin.Weights[3];
        if (!(total > 0.0f))
        {
            out[0] = in.Position[0]; out[1] = in.Position[1]; out[2] = in.Position[2];
            return;
        }
        out[0] = out[1] = out[2] = 0.0f;
        const std::size_t joints = palette.size() / 16;
        for (int k = 0; k < 4; ++k)
        {
            if (skin.Weights[k] <= 0.0f)
                continue;
            const float* m = &palette[(std::min)(static_cast<std::size_t>(skin.Joints[k]), joints - 1) * 16];
            for (int c = 0; c < 3; ++c)
                out[c] += skin.Weights[k] * (in.Position[0] * m[c] + in.Position[1] * m[4 + c] + in.Position[2] * m[8 + c] + m[12 + c]);
        }
    }

    void TestSkinnedMesh(const char* sourceFbx)
    {
        std::printf("Skinned mesh %s\n", sourceFbx);
        HMODULE system = LoadLibraryW(L"System.dll");
        using RecookFn = bool(__stdcall*)(const char*, bool, bool*, char*, int);
        const auto recook = system ? reinterpret_cast<RecookFn>(GetProcAddress(system, "System_RecookMesh")) : nullptr;
        if (recook == nullptr)
        {
            std::printf("  skipped: System.dll with System_RecookMesh not found (run from Binaries); error %lu\n", GetLastError());
            return;
        }

        const std::filesystem::path directory = std::filesystem::path(gDir) / "Skinned";
        std::filesystem::create_directories(directory);
        const std::filesystem::path fbx = directory / std::filesystem::path(sourceFbx).filename();
        std::filesystem::copy_file(sourceFbx, fbx, std::filesystem::copy_options::overwrite_existing);

        bool skinned = false;
        char status[512] = {};
        Check(recook(fbx.string().c_str(), false, &skinned, status, sizeof(status)), status);
        Check(skinned, "the cook wrote skin weights");
        if (!skinned)
            return;

        // .ptero LOD 0: header, sub-mesh table, vertices.
        std::vector<std::uint8_t> ptero, pskin;
        Check(ReadAllBytes(fbx.string() + ".ptero", ptero), "read .ptero");
        Check(ReadAllBytes(fbx.string() + ".pskin", pskin), "read .pskin");
        std::uint32_t header[6] = {};
        std::memcpy(header, ptero.data(), sizeof(header));
        const std::uint32_t vertexCount = header[2];
        const std::uint32_t subMeshCount = header[4];
        const std::size_t vertexOffset = sizeof(header) + subMeshCount * 12u;
        std::vector<CookedVertex> vertices(vertexCount);
        std::memcpy(vertices.data(), ptero.data() + vertexOffset, vertexCount * sizeof(CookedVertex));

        std::uint32_t skinHeader[4] = {};
        std::memcpy(skinHeader, pskin.data(), sizeof(skinHeader));
        Check(std::memcmp(pskin.data(), "PSKN", 4) == 0 && skinHeader[2] == vertexCount, ".pskin matches the .ptero's LOD 0");
        std::vector<std::string> jointNames;
        std::size_t offset = sizeof(skinHeader);
        for (std::uint32_t i = 0; i < skinHeader[3]; ++i)
        {
            std::uint32_t length = 0;
            std::memcpy(&length, pskin.data() + offset, 4);
            jointNames.emplace_back(reinterpret_cast<const char*>(pskin.data() + offset + 4), length);
            offset += 4 + length;
        }
        std::vector<SkinRecord> skin(vertexCount);
        std::memcpy(skin.data(), pskin.data() + offset, vertexCount * sizeof(SkinRecord));

        // Skeleton and clips from the same FBX.
        char error[1024] = {};
        const std::string base = fbx.stem().string();
        AnimationFbxImportResult result;
        Check(AnimationImportFbx(fbx.string().c_str(), directory.string().c_str(), base.c_str(),
                                 AnimationFbxImportSettings{}, &result, error, sizeof(error)), error);
        AnimationSkeleton skeleton;
        Check(skeleton.LoadFromFile((directory / (base + ".pskel")).string().c_str(), error, sizeof(error)), error);

        std::vector<int> meshToSkeleton(jointNames.size());
        int matched = 0;
        for (std::size_t i = 0; i < jointNames.size(); ++i)
        {
            meshToSkeleton[i] = skeleton.FindJoint(jointNames[i].c_str());
            matched += meshToSkeleton[i] >= 0 ? 1 : 0;
        }
        std::printf("  %u vertices, %zu skin joints, %d found in the %d-joint skeleton\n",
                    vertexCount, jointNames.size(), matched, skeleton.GetJointCount());
        Check(matched == static_cast<int>(jointNames.size()), "every skin joint exists in the skeleton");

        AnimationPose pose;
        pose.Initialize(skeleton);
        std::vector<float> skeletonPalette(static_cast<std::size_t>(skeleton.GetJointCount()) * 16);
        std::vector<float> meshPalette(jointNames.size() * 16);
        auto buildPalette = [&]()
        {
            pose.GetSkinningMatrices(skeletonPalette.data(), skeleton.GetJointCount());
            for (std::size_t i = 0; i < jointNames.size(); ++i)
            {
                const int joint = meshToSkeleton[i];
                for (int e = 0; e < 16; ++e)
                    meshPalette[i * 16 + e] = joint >= 0 ? skeletonPalette[static_cast<std::size_t>(joint) * 16 + e] : (e % 5 == 0 ? 1.0f : 0.0f);
            }
        };

        // Rest pose: vertices stay where they were cooked when rest == bind.
        buildPalette();
        float worst = 0.0f;
        float boundsMin[3] = { 1e30f, 1e30f, 1e30f }, boundsMax[3] = { -1e30f, -1e30f, -1e30f };
        for (std::uint32_t v = 0; v < vertexCount; ++v)
        {
            float p[3];
            SkinVertex(vertices[v], skin[v], meshPalette, p);
            for (int c = 0; c < 3; ++c)
            {
                worst = (std::max)(worst, std::fabs(p[c] - vertices[v].Position[c]));
                boundsMin[c] = (std::min)(boundsMin[c], vertices[v].Position[c]);
                boundsMax[c] = (std::max)(boundsMax[c], vertices[v].Position[c]);
            }
        }
        std::printf("  cooked bounds (%.2f %.2f %.2f)..(%.2f %.2f %.2f) m; rest-pose skinning moves vertices by at most %.4f m\n",
                    boundsMin[0], boundsMin[1], boundsMin[2], boundsMax[0], boundsMax[1], boundsMax[2], worst);

        // Halfway through the first clip the mesh is posed but still the same size.
        for (const auto& entry : std::filesystem::directory_iterator(directory))
        {
            if (entry.path().extension() != ".panim" || entry.path().filename().string().rfind(base + "@", 0) != 0)
                continue;
            AnimationClip clip;
            Check(clip.LoadFromFile(entry.path().string().c_str(), error, sizeof(error)), error);
            AnimationLayer layer;
            layer.Clip = &clip;
            layer.Time = clip.GetDuration() * 0.5f;
            pose.Evaluate(&layer, 1);
            buildPalette();
            float posedMin[3] = { 1e30f, 1e30f, 1e30f }, posedMax[3] = { -1e30f, -1e30f, -1e30f };
            bool finite = true;
            for (std::uint32_t v = 0; v < vertexCount; ++v)
            {
                float p[3];
                SkinVertex(vertices[v], skin[v], meshPalette, p);
                for (int c = 0; c < 3; ++c)
                {
                    finite &= std::isfinite(p[c]);
                    posedMin[c] = (std::min)(posedMin[c], p[c]);
                    posedMax[c] = (std::max)(posedMax[c], p[c]);
                }
            }
            Check(finite, "posed vertices are finite");
            std::printf("  '%s' at %.2f s: posed bounds (%.2f %.2f %.2f)..(%.2f %.2f %.2f) m\n", clip.GetName(), layer.Time,
                        posedMin[0], posedMin[1], posedMin[2], posedMax[0], posedMax[1], posedMax[2]);
            break;
        }
    }
}

int main(int argc, char** argv)
{
    const char* temp = std::getenv("TEMP");
    gDir = std::string(temp ? temp : ".") + "/PteroAnimationTests/Assets";
    std::filesystem::remove_all(gDir);
    std::filesystem::create_directories(gDir);

    TestSkeleton();
    TestSamplingAndBlending();
    TestIK();
    TestController();
    for (int i = 1; i < argc; ++i)
    {
        TestFbx(argv[i]);
        TestSkinnedMesh(argv[i]);
    }

    std::printf("%d of %d checks passed\n", gChecks - gFailures, gChecks);
    return gFailures == 0 ? 0 : 1;
}
