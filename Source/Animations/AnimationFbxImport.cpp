#include "pch.h"

#include "AnimationImport.h"
#include "AnimationInternal.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace AnimationDetail;

#ifdef PTERO_FBX_SDK_AVAILABLE

#include <fbxsdk.h>

#include <cmath>

namespace
{
    // Same frame as FbxCompiler::ConvertSceneToEngineFrame: metres and 3ds Max axes (Z up,
    // right-handed). The engine is left-handed, so everything below is then mirrored in X,
    // exactly as FbxCompiler mirrors vertex positions.
    void ConvertSceneToEngineFrame(FbxScene* scene)
    {
        FbxGlobalSettings& settings = scene->GetGlobalSettings();
        if (settings.GetSystemUnit() != FbxSystemUnit::m)
        {
            FbxSystemUnit::m.ConvertScene(scene);
        }
        if (settings.GetAxisSystem() != FbxAxisSystem::Max)
        {
            FbxAxisSystem::Max.ConvertScene(scene);
        }
    }

    // A local transform conjugated by the X mirror M: M*T*M negates the X translation,
    // M*R*M turns quaternion (x, y, z, w) into (x, -y, -z, w), and scale is unchanged.
    AnimationTransform ToEngineTransform(const FbxAMatrix& local)
    {
        const FbxVector4 t = local.GetT();
        const FbxQuaternion q = local.GetQ();
        const FbxVector4 s = local.GetS();

        AnimationTransform out;
        out.Translation[0] = static_cast<float>(-t[0]);
        out.Translation[1] = static_cast<float>(t[1]);
        out.Translation[2] = static_cast<float>(t[2]);

        double x = q[0], y = -q[1], z = -q[2], w = q[3];
        const double length = std::sqrt(x * x + y * y + z * z + w * w);
        if (length > 1e-12)
        {
            x /= length; y /= length; z /= length; w /= length;
        }
        else
        {
            x = y = z = 0.0; w = 1.0;
        }
        out.Rotation[0] = static_cast<float>(x);
        out.Rotation[1] = static_cast<float>(y);
        out.Rotation[2] = static_cast<float>(z);
        out.Rotation[3] = static_cast<float>(w);

        out.Scale[0] = static_cast<float>(s[0]);
        out.Scale[1] = static_cast<float>(s[1]);
        out.Scale[2] = static_cast<float>(s[2]);
        return out;
    }

    // FbxAMatrix is row-vector with the translation in row 3 - the XMFLOAT4X4 layout.
    // Conjugating by the X mirror flips the sign of every element in row 0 or column 0
    // (but not both).
    void ToEngineMatrix(const FbxAMatrix& m, float out[16])
    {
        for (int row = 0; row < 4; ++row)
        {
            for (int column = 0; column < 4; ++column)
            {
                const double sign = ((row == 0) != (column == 0)) ? -1.0 : 1.0;
                out[row * 4 + column] = static_cast<float>(m.Get(row, column) * sign);
            }
        }
    }

    struct FbxJoint
    {
        FbxNode* Node = nullptr;
        int Parent = -1;
        std::string Name;
        bool HasInverseBind = false;
        float InverseBind[16]{};
    };

    // Joints are skeleton nodes, every node a skin is bound to, and all their ancestors
    // (a Blender "Armature" null still moves the bones under it).
    void CollectJoints(FbxScene* scene, std::vector<FbxJoint>& joints, int& skinnedJoints)
    {
        FbxNode* root = scene->GetRootNode();
        std::unordered_set<FbxNode*> wanted;

        struct BindInfo
        {
            FbxAMatrix BoneBind; // the bone's global matrix at bind time, scene space
        };
        std::unordered_map<FbxNode*, BindInfo> binds;

        std::vector<FbxNode*> stack{ root };
        while (!stack.empty())
        {
            FbxNode* node = stack.back();
            stack.pop_back();
            for (int i = 0; i < node->GetChildCount(); ++i)
            {
                stack.push_back(node->GetChild(i));
            }
            if (node == root)
            {
                continue;
            }

            const FbxNodeAttribute* attribute = node->GetNodeAttribute();
            if (attribute != nullptr && attribute->GetAttributeType() == FbxNodeAttribute::eSkeleton)
            {
                wanted.insert(node);
            }

            FbxGeometry* geometry = node->GetGeometry();
            if (geometry == nullptr)
            {
                continue;
            }
            // The mesh's vertices were baked with its global transform at the default time
            // (FbxCompiler), so the bind is re-expressed relative to that, rather than taken
            // raw from the cluster - which also keeps it right whatever the unit and axis
            // conversions did to the cluster matrices.
            const FbxAMatrix meshGlobal = node->EvaluateGlobalTransform();
            for (int d = 0; d < geometry->GetDeformerCount(FbxDeformer::eSkin); ++d)
            {
                auto* skin = static_cast<FbxSkin*>(geometry->GetDeformer(d, FbxDeformer::eSkin));
                for (int c = 0; c < skin->GetClusterCount(); ++c)
                {
                    FbxCluster* cluster = skin->GetCluster(c);
                    FbxNode* link = cluster ? cluster->GetLink() : nullptr;
                    if (link == nullptr)
                    {
                        continue;
                    }
                    wanted.insert(link);
                    if (binds.count(link) == 0)
                    {
                        FbxAMatrix meshBind;
                        FbxAMatrix linkBind;
                        cluster->GetTransformMatrix(meshBind);
                        cluster->GetTransformLinkMatrix(linkBind);
                        binds[link] = BindInfo{ meshGlobal * meshBind.Inverse() * linkBind };
                    }
                }
            }
        }

        std::unordered_set<FbxNode*> included;
        for (FbxNode* node : wanted)
        {
            for (FbxNode* n = node; n != nullptr && n != root; n = n->GetParent())
            {
                included.insert(n);
            }
        }

        // Depth first from the root, parents before children.
        std::unordered_set<std::string> usedNames;
        struct Pending { FbxNode* Node; int Parent; };
        std::vector<Pending> pending;
        for (int i = root->GetChildCount() - 1; i >= 0; --i)
        {
            pending.push_back({ root->GetChild(i), -1 });
        }
        while (!pending.empty())
        {
            const Pending item = pending.back();
            pending.pop_back();

            int parentForChildren = item.Parent;
            if (included.count(item.Node) != 0)
            {
                FbxJoint joint;
                joint.Node = item.Node;
                joint.Parent = item.Parent;
                joint.Name = item.Node->GetName();
                if (joint.Name.empty())
                {
                    joint.Name = "Joint";
                }
                const std::string base = joint.Name;
                for (int suffix = 2; !usedNames.insert(joint.Name).second; ++suffix)
                {
                    joint.Name = base + "_" + std::to_string(suffix);
                }

                const auto bind = binds.find(item.Node);
                if (bind != binds.end())
                {
                    ToEngineMatrix(bind->second.BoneBind.Inverse(), joint.InverseBind);
                    joint.HasInverseBind = true;
                    ++skinnedJoints;
                }

                parentForChildren = static_cast<int>(joints.size());
                joints.push_back(std::move(joint));
            }
            for (int i = item.Node->GetChildCount() - 1; i >= 0; --i)
            {
                pending.push_back({ item.Node->GetChild(i), parentForChildren });
            }
        }
    }

    // Locals from globals, so every FBX inheritance mode and pivot is already folded in by
    // the evaluator, and ancestors that are not joints do not need to be.
    void EvaluateLocals(const std::vector<FbxJoint>& joints, const FbxTime& time,
                        std::vector<FbxAMatrix>& globals, AnimationTransform* out)
    {
        globals.resize(joints.size());
        for (std::size_t i = 0; i < joints.size(); ++i)
        {
            globals[i] = joints[i].Node->EvaluateGlobalTransform(time);
            const FbxAMatrix local = joints[i].Parent >= 0
                ? globals[joints[i].Parent].Inverse() * globals[i]
                : globals[i];
            out[i] = ToEngineTransform(local);
        }
    }

    std::string SanitizeFileName(std::string name)
    {
        for (char& c : name)
        {
            if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' ||
                c == '?' || c == '*' || static_cast<unsigned char>(c) < 32)
            {
                c = '_';
            }
        }
        return name.empty() ? std::string("Take") : name;
    }

    struct FbxManagerGuard
    {
        FbxManager* Manager = FbxManager::Create();
        ~FbxManagerGuard()
        {
            if (Manager)
            {
                Manager->Destroy();
            }
        }
    };
}

bool AnimationImportFbx(const char* fbxPath, const char* outputDirectory, const char* baseName,
                        const AnimationFbxImportSettings& settings, AnimationFbxImportResult* result,
                        char* error, std::size_t errorSize)
{
    if (result)
    {
        *result = AnimationFbxImportResult{};
    }
    if (fbxPath == nullptr || outputDirectory == nullptr || baseName == nullptr || baseName[0] == '\0')
    {
        SetError(error, errorSize, "Missing FBX path, output directory or base name.");
        return false;
    }
    if (!(settings.SampleRate > 0.0f))
    {
        SetError(error, errorSize, "The sample rate must be positive.");
        return false;
    }

    FbxManagerGuard guard;
    if (guard.Manager == nullptr)
    {
        SetError(error, errorSize, "Could not start the FBX SDK.");
        return false;
    }
    guard.Manager->SetIOSettings(FbxIOSettings::Create(guard.Manager, IOSROOT));

    FbxImporter* importer = FbxImporter::Create(guard.Manager, "");
    if (importer == nullptr || !importer->Initialize(fbxPath, -1, guard.Manager->GetIOSettings()))
    {
        SetError(error, errorSize, "Cannot open '%s': %s", fbxPath,
                 importer ? importer->GetStatus().GetErrorString() : "no importer");
        if (importer)
        {
            importer->Destroy();
        }
        return false;
    }

    FbxScene* scene = FbxScene::Create(guard.Manager, "PteroAnimationScene");
    const bool imported = scene != nullptr && importer->Import(scene);
    importer->Destroy();
    if (!imported)
    {
        SetError(error, errorSize, "The FBX SDK could not read '%s'.", fbxPath);
        return false;
    }

    ConvertSceneToEngineFrame(scene);

    std::vector<FbxJoint> joints;
    int skinnedJoints = 0;
    CollectJoints(scene, joints, skinnedJoints);
    if (joints.empty())
    {
        SetError(error, errorSize, "'%s' has no skeleton (no bones and no skinned meshes).", fbxPath);
        return false;
    }
    if (joints.size() > static_cast<std::size_t>(ozz::animation::Skeleton::kMaxJoints))
    {
        SetError(error, errorSize, "'%s' has %zu joints; at most %d are supported.", fbxPath,
                 joints.size(), static_cast<int>(ozz::animation::Skeleton::kMaxJoints));
        return false;
    }

    const std::string directory = outputDirectory;
    const std::string separator = (directory.empty() || directory.back() == '/' || directory.back() == '\\') ? "" : "/";
    const std::string base = SanitizeFileName(baseName);

    // The skeleton the clips are built against: the file's own, or an existing one.
    AnimationSkeleton skeleton;
    std::vector<FbxAMatrix> globals;
    if (settings.SkeletonPath == nullptr || settings.SkeletonPath[0] == '\0')
    {
        std::vector<AnimationTransform> rest(joints.size());
        EvaluateLocals(joints, FBXSDK_TIME_INFINITE, globals, rest.data());

        std::vector<AnimationRawJoint> raw(joints.size());
        for (std::size_t i = 0; i < joints.size(); ++i)
        {
            raw[i].Name = joints[i].Name.c_str();
            raw[i].Parent = joints[i].Parent;
            raw[i].RestPose = rest[i];
            raw[i].HasInverseBind = joints[i].HasInverseBind;
            std::memcpy(raw[i].InverseBind, joints[i].InverseBind, sizeof(raw[i].InverseBind));
        }

        const std::string skeletonPath = directory + separator + base + ".pskel";
        if (settings.ImportSkeleton)
        {
            if (!AnimationBuildSkeleton(raw.data(), static_cast<int>(raw.size()), skeletonPath.c_str(), error, errorSize) ||
                !skeleton.LoadFromFile(skeletonPath.c_str(), error, errorSize))
            {
                return false;
            }
        }
        else if (settings.ImportClips)
        {
            SetError(error, errorSize, "Importing clips without the file's skeleton needs a SkeletonPath.");
            return false;
        }
    }
    else if (!skeleton.LoadFromFile(settings.SkeletonPath, error, errorSize))
    {
        return false;
    }

    int clipCount = 0;
    if (settings.ImportClips)
    {
        std::vector<const char*> trackNames(joints.size());
        for (std::size_t i = 0; i < joints.size(); ++i)
        {
            trackNames[i] = joints[i].Name.c_str();
        }

        const int stackCount = scene->GetSrcObjectCount<FbxAnimStack>();
        for (int s = 0; s < stackCount; ++s)
        {
            FbxAnimStack* stack = scene->GetSrcObject<FbxAnimStack>(s);
            scene->SetCurrentAnimationStack(stack);

            FbxTimeSpan span = stack->GetLocalTimeSpan();
            if (span.GetDuration().GetSecondDouble() <= 0.0)
            {
                if (const FbxTakeInfo* take = scene->GetTakeInfo(stack->GetName()))
                {
                    span = take->mLocalTimeSpan;
                }
            }
            const double start = span.GetStart().GetSecondDouble();
            const double duration = span.GetDuration().GetSecondDouble();
            if (!(duration > 0.0))
            {
                continue;
            }

            // Whole number of intervals so the last sample lands exactly on the end.
            const int intervals = (std::max)(1, static_cast<int>(std::ceil(duration * settings.SampleRate - 1e-3)));
            const int sampleCount = intervals + 1;
            const float sampleRate = static_cast<float>(intervals / duration);

            std::vector<AnimationTransform> samples(static_cast<std::size_t>(sampleCount) * joints.size());
            for (int i = 0; i < sampleCount; ++i)
            {
                FbxTime time;
                time.SetSecondDouble(start + duration * i / intervals);
                EvaluateLocals(joints, time, globals, samples.data() + static_cast<std::size_t>(i) * joints.size());
            }

            const std::string takeName = stack->GetName();
            AnimationRawClip clip;
            clip.Name = takeName.c_str();
            clip.SampleRate = sampleRate;
            clip.SampleCount = sampleCount;
            clip.TrackCount = static_cast<int>(joints.size());
            clip.TrackJointNames = trackNames.data();
            clip.Samples = samples.data();
            clip.MakeAdditive = settings.MakeAdditive;

            const std::string clipPath = directory + separator + base + "@" + SanitizeFileName(takeName) + ".panim";
            if (!AnimationBuildClip(skeleton, clip, settings.Compression, clipPath.c_str(), error, errorSize))
            {
                return false;
            }
            ++clipCount;
        }
    }

    if (result)
    {
        result->JointCount = static_cast<int>(joints.size());
        result->ClipCount = clipCount;
        result->SkinnedJointCount = skinnedJoints;
    }
    return true;
}

#else

bool AnimationImportFbx(const char*, const char*, const char*, const AnimationFbxImportSettings&,
                        AnimationFbxImportResult* result, char* error, std::size_t errorSize)
{
    if (result)
    {
        *result = AnimationFbxImportResult{};
    }
    SetError(error, errorSize, "This build of Animations.dll has no FBX SDK.");
    return false;
}

#endif
