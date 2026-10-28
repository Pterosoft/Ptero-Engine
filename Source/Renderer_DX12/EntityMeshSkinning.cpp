#include "pch.h"

// EntityMeshRenderer's GPU skinning: the compute pass that poses every animated entity's
// LOD 0 into a vertex buffer of its own (see EntityMeshRenderer.h, "Skinning", and
// SkinVertices.hlsl), and the lookups the draw passes use to bind it.

#include "EntityMeshRenderer.h"
#include "SceneAnimation.h"
#include "d3dx12.h"

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace
{
    // SkinVertices.hlsl's root parameters.
    enum SkinRootParameter : UINT
    {
        SkinRootConstants = 0, // b0: vertex count, joint count
        SkinRootBindVertices,  // t0
        SkinRootInfluences,    // t1
        SkinRootPalette,       // t2
        SkinRootOutput,        // u0
        SkinRootParameterCount
    };

    constexpr UINT kSkinThreadGroupSize = 64;
    constexpr std::size_t kPaletteRowBytes = 16;  // one float4
    constexpr std::size_t kPaletteJointBytes = 64; // four rows

    bool CreateBuffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags,
                      ComPtr<ID3D12Resource>& out, const wchar_t* name)
    {
        const CD3DX12_HEAP_PROPERTIES heap(heapType);
        const CD3DX12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(size, flags);
        // Buffers are created in COMMON whatever is asked for (and decay back to it after
        // every ExecuteCommandLists), so ask for what they really are - except upload
        // heaps, which must be GENERIC_READ.
        const D3D12_RESOURCE_STATES state = heapType == D3D12_HEAP_TYPE_UPLOAD
            ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&out))))
            return false;
        if (name)
            out->SetName(name);
        return true;
    }

    // Uploads `bytes` into a new DEFAULT buffer; the staging copy goes to `staging`, which
    // the caller retires (it must live until the copy has executed).
    bool UploadBuffer(ID3D12Device* device, ID3D12GraphicsCommandList* commandList, const void* bytes, UINT64 size,
                      ComPtr<ID3D12Resource>& out, ComPtr<ID3D12Resource>& staging, const wchar_t* name)
    {
        if (!CreateBuffer(device, size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, out, name) ||
            !CreateBuffer(device, size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, staging, L"Skin_Staging"))
            return false;
        void* mapped = nullptr;
        if (FAILED(staging->Map(0, nullptr, &mapped)))
            return false;
        std::memcpy(mapped, bytes, static_cast<std::size_t>(size));
        staging->Unmap(0, nullptr);
        // COMMON -> COPY_DEST by implicit promotion; made readable explicitly below.
        commandList->CopyBufferRegion(out.Get(), 0, staging.Get(), 0, size);
        return true;
    }
}

bool EntityMeshRenderer::EnsureSkinPipeline()
{
    if (mSkinPipeline)
        return true;
    if (mSkinPipelineFailed)
        return false;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    DX12Shader shader;
    const ShaderCompileRequest request{ L"Shaders\\SkinVertices.hlsl", L"CSMain", L"cs_5_0", ShaderStage::Compute };
    if (!shader.Compile(request))
    {
        mLastError = std::string("SkinVertices.hlsl failed to compile: ") +
                     (shader.GetLastErrorMessage() ? shader.GetLastErrorMessage() : "unknown");
        mSkinPipelineFailed = true;
        return false;
    }

    D3D12_ROOT_PARAMETER parameters[SkinRootParameterCount]{};
    for (D3D12_ROOT_PARAMETER& parameter : parameters)
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[SkinRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[SkinRootConstants].Constants.Num32BitValues = 4;
    parameters[SkinRootBindVertices].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[SkinRootBindVertices].Descriptor.ShaderRegister = 0;
    parameters[SkinRootInfluences].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[SkinRootInfluences].Descriptor.ShaderRegister = 1;
    parameters[SkinRootPalette].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    parameters[SkinRootPalette].Descriptor.ShaderRegister = 2;
    parameters[SkinRootOutput].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    parameters[SkinRootOutput].Descriptor.ShaderRegister = 0;

    D3D12_ROOT_SIGNATURE_DESC rootDesc{};
    rootDesc.NumParameters = SkinRootParameterCount;
    rootDesc.pParameters = parameters;
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
        FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                           IID_PPV_ARGS(&mSkinRootSignature))))
    {
        mLastError = "Could not create the skinning root signature.";
        mSkinPipelineFailed = true;
        return false;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
    psoDesc.pRootSignature = mSkinRootSignature.Get();
    psoDesc.CS = shader.GetBytecode();
    if (FAILED(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&mSkinPipeline))))
    {
        mLastError = "Could not create the skinning pipeline.";
        mSkinPipelineFailed = true;
        return false;
    }
    mSkinPipeline->SetName(L"SkinVertices");
    return true;
}

bool EntityMeshRenderer::EnsureSkinSource(ID3D12GraphicsCommandList* commandList, const std::shared_ptr<Mesh>& mesh,
                                          SkinSource*& outSource)
{
    outSource = nullptr;
    auto found = mSkinSources.find(mesh.get());
    if (found != mSkinSources.end() && found->second.Owner == mesh)
    {
        found->second.LastUsedFrame = mFrameCounter;
        outSource = &found->second;
        return true;
    }
    if (found != mSkinSources.end())
    {
        RetireBuffer(std::move(found->second.BindVertices), "stale skin bind vertices");
        RetireBuffer(std::move(found->second.Influences), "stale skin influences");
        mSkinSources.erase(found);
    }

    const std::vector<Vertex>& vertices = mesh->GetVertices();
    const std::shared_ptr<const MeshSkin>& skin = mesh->GetSkin();
    if (!skin || vertices.empty() || skin->Vertices.size() != vertices.size())
        return false;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    SkinSource source;
    ComPtr<ID3D12Resource> vertexStaging;
    ComPtr<ID3D12Resource> influenceStaging;
    if (!UploadBuffer(device, commandList, vertices.data(), vertices.size() * sizeof(Vertex),
                      source.BindVertices, vertexStaging, L"Skin_BindVertices") ||
        !UploadBuffer(device, commandList, skin->Vertices.data(), skin->Vertices.size() * sizeof(MeshSkinVertex),
                      source.Influences, influenceStaging, L"Skin_Influences"))
    {
        return false;
    }
    RetireBuffer(std::move(vertexStaging), "skin vertex staging buffer");
    RetireBuffer(std::move(influenceStaging), "skin influence staging buffer");

    const D3D12_RESOURCE_BARRIER barriers[2] = {
        CD3DX12_RESOURCE_BARRIER::Transition(source.BindVertices.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        CD3DX12_RESOURCE_BARRIER::Transition(source.Influences.Get(),
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    commandList->ResourceBarrier(2, barriers);

    source.VertexCount = static_cast<UINT>(vertices.size());
    source.Owner = mesh;
    source.LastUsedFrame = mFrameCounter;
    outSource = &mSkinSources.insert_or_assign(mesh.get(), std::move(source)).first->second;
    return true;
}

bool EntityMeshRenderer::EnsurePaletteCapacity(std::size_t bytesPerFrame)
{
    if (mPaletteUpload && bytesPerFrame <= mPaletteSlotBytes)
        return true;

    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
        return false;

    // Grow in generous steps: a level usually animates the same few characters every frame.
    const std::size_t slotBytes = (std::max)(bytesPerFrame * 2, std::size_t(64 * 1024));
    if (mPaletteUpload)
    {
        mPaletteUpload->Unmap(0, nullptr);
        mPaletteMapped = nullptr;
        RetireBuffer(std::move(mPaletteUpload), "skin palette upload buffer");
    }
    if (!CreateBuffer(device, slotBytes * kFramesInFlight, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE,
                      mPaletteUpload, L"Skin_Palettes"))
        return false;
    void* mapped = nullptr;
    if (FAILED(mPaletteUpload->Map(0, nullptr, &mapped)))
    {
        mPaletteUpload.Reset();
        return false;
    }
    mPaletteMapped = static_cast<std::byte*>(mapped);
    mPaletteSlotBytes = slotBytes;
    return true;
}

void EntityMeshRenderer::ReleaseSkinnedInstance(SkinnedInstance& instance)
{
    RetireBuffer(std::move(instance.Output[0]), "skinned vertex buffer");
    RetireBuffer(std::move(instance.Output[1]), "skinned vertex buffer");
}

void EntityMeshRenderer::UpdateSkinning(ID3D12GraphicsCommandList* commandList, const SceneAnimation& animation)
{
    if (commandList == nullptr || mEntities == nullptr)
        return;

    struct Job
    {
        SkinnedInstance* Instance;
        const SkinSource* Source;
        const std::vector<DirectX::XMFLOAT4X4>* Palette;
    };
    std::vector<Job> jobs;
    std::size_t paletteBytes = 0;

    for (std::size_t i = 0; i < mEntities->size(); ++i)
    {
        const Entity& entity = (*mEntities)[i];
        if (!entity.Animator || !entity.Mesh || !entity.Mesh->MeshAsset || entity.Id == 0)
            continue;
        const std::vector<DirectX::XMFLOAT4X4>* palette = animation.GetPalette(entity.Id);
        if (palette == nullptr || palette->empty())
            continue;
        if (!EnsureSkinPipeline())
            return;

        const std::shared_ptr<Mesh>& mesh = entity.Mesh->MeshAsset;
        // The draws index LOD 0's index buffer, so that is what has to be on the GPU.
        if (!EnsureEntityGpuMesh(commandList, i, mesh, 0))
            continue;
        SkinSource* source = nullptr;
        if (!EnsureSkinSource(commandList, mesh, source) || source == nullptr)
            continue;

        SkinnedInstance& instance = mSkinnedInstances[entity.Id];
        if (instance.SourceMesh != mesh.get() || instance.VertexCount != source->VertexCount || !instance.Output[0])
        {
            ReleaseSkinnedInstance(instance);
            instance = SkinnedInstance{};
            ID3D12Device* device = DX12Context_GetDevice();
            const UINT64 bytes = static_cast<UINT64>(source->VertexCount) * sizeof(Vertex);
            bool created = device != nullptr;
            for (int b = 0; b < 2 && created; ++b)
            {
                created = CreateBuffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                       instance.Output[b], L"Skin_PosedVertices");
                if (created)
                {
                    instance.Views[b].BufferLocation = instance.Output[b]->GetGPUVirtualAddress();
                    instance.Views[b].StrideInBytes = sizeof(Vertex);
                    instance.Views[b].SizeInBytes = static_cast<UINT>(bytes);
                }
            }
            if (!created)
            {
                mSkinnedInstances.erase(entity.Id);
                continue;
            }
            instance.SourceMesh = mesh.get();
            instance.VertexCount = source->VertexCount;
        }
        else
        {
            instance.HasPrevious = instance.SkinnedFrame + 1 == mFrameCounter;
            instance.Current ^= 1;
        }
        instance.SkinnedFrame = mFrameCounter;

        jobs.push_back({ &instance, source, palette });
        paletteBytes += palette->size() * kPaletteJointBytes;
    }

    // Entities that stopped animating (or were deleted) give their buffers back; the
    // retire list keeps them alive until in-flight frames are done with them.
    for (auto it = mSkinnedInstances.begin(); it != mSkinnedInstances.end(); )
    {
        if (it->second.SkinnedFrame != mFrameCounter)
        {
            ReleaseSkinnedInstance(it->second);
            it = mSkinnedInstances.erase(it);
        }
        else
        {
            ++it;
        }
    }
    if (mFrameCounter % 120 == 0)
    {
        for (auto it = mSkinSources.begin(); it != mSkinSources.end(); )
        {
            if (mFrameCounter - it->second.LastUsedFrame > kMeshEvictionFrames)
            {
                RetireBuffer(std::move(it->second.BindVertices), "evicted skin bind vertices");
                RetireBuffer(std::move(it->second.Influences), "evicted skin influences");
                it = mSkinSources.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    if (jobs.empty())
        return;
    if (!EnsurePaletteCapacity(paletteBytes))
    {
        for (const Job& job : jobs)
            job.Instance->SkinnedFrame = 0; // draw rigid rather than from unwritten buffers
        return;
    }

    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    barriers.reserve(jobs.size());
    for (const Job& job : jobs)
    {
        barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(job.Instance->Output[job.Instance->Current].Get(),
            D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    }
    commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());

    commandList->SetComputeRootSignature(mSkinRootSignature.Get());
    commandList->SetPipelineState(mSkinPipeline.Get());

    const std::size_t slotOffset = mFrameSlot * mPaletteSlotBytes;
    const D3D12_GPU_VIRTUAL_ADDRESS paletteBase = mPaletteUpload->GetGPUVirtualAddress() + slotOffset;
    std::size_t paletteOffset = 0;
    for (const Job& job : jobs)
    {
        const std::size_t bytes = job.Palette->size() * kPaletteJointBytes;
        static_assert(sizeof(DirectX::XMFLOAT4X4) == kPaletteJointBytes);
        std::memcpy(mPaletteMapped + slotOffset + paletteOffset, job.Palette->data(), bytes);

        const UINT constants[4] = { job.Instance->VertexCount, static_cast<UINT>(job.Palette->size()), 0, 0 };
        commandList->SetComputeRoot32BitConstants(SkinRootConstants, 4, constants, 0);
        commandList->SetComputeRootShaderResourceView(SkinRootBindVertices, job.Source->BindVertices->GetGPUVirtualAddress());
        commandList->SetComputeRootShaderResourceView(SkinRootInfluences, job.Source->Influences->GetGPUVirtualAddress());
        commandList->SetComputeRootShaderResourceView(SkinRootPalette, paletteBase + paletteOffset);
        commandList->SetComputeRootUnorderedAccessView(SkinRootOutput,
            job.Instance->Output[job.Instance->Current]->GetGPUVirtualAddress());
        commandList->Dispatch((job.Instance->VertexCount + kSkinThreadGroupSize - 1) / kSkinThreadGroupSize, 1, 1);

        paletteOffset += bytes;
        static_assert(kPaletteJointBytes % kPaletteRowBytes == 0);
    }

    barriers.clear();
    for (const Job& job : jobs)
    {
        barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(job.Instance->Output[job.Instance->Current].Get(),
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    }
    commandList->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
}

bool EntityMeshRenderer::IsSkinnedThisFrame(const Entity& entity) const
{
    if (!entity.Animator || entity.Id == 0 || mSkinnedInstances.empty())
        return false;
    const auto found = mSkinnedInstances.find(entity.Id);
    return found != mSkinnedInstances.end() && found->second.SkinnedFrame == mFrameCounter &&
           entity.Mesh && found->second.SourceMesh == entity.Mesh->MeshAsset.get();
}

bool EntityMeshRenderer::GetSkinnedVertexViews(const Entity& entity, D3D12_VERTEX_BUFFER_VIEW& current,
                                               D3D12_VERTEX_BUFFER_VIEW& previous) const
{
    if (!IsSkinnedThisFrame(entity))
        return false;
    const SkinnedInstance& instance = mSkinnedInstances.at(entity.Id);
    current = instance.Views[instance.Current];
    previous = instance.HasPrevious ? instance.Views[instance.Current ^ 1] : current;
    return true;
}

const D3D12_VERTEX_BUFFER_VIEW& EntityMeshRenderer::VertexViewFor(const Entity& entity, const EntityGpuMesh& gpuMesh) const
{
    if (gpuMesh.LodIndex == 0 && IsSkinnedThisFrame(entity))
    {
        const SkinnedInstance& instance = mSkinnedInstances.at(entity.Id);
        return instance.Views[instance.Current];
    }
    return gpuMesh.VertexBufferView;
}
