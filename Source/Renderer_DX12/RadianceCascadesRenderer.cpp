#include "pch.h"
#include "RadianceCascadesRenderer.h"

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

extern "C"
{
	ID3D12Device* __stdcall DX12Context_GetDevice();
	ID3D12DescriptorHeap* __stdcall DX12Context_GetSrvDescriptorHeap();
	bool __stdcall DX12Context_AllocateSrvDescriptor(
		D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
}

namespace
{
	constexpr DXGI_FORMAT kRadianceCascadesFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
	constexpr UINT kGroupSize = 8;

	bool AllocDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& cpu, D3D12_GPU_DESCRIPTOR_HANDLE& gpu, std::string& err)
	{
		if (!DX12Context_AllocateSrvDescriptor(&cpu, &gpu))
		{
			err = "RadianceCascadesRenderer: ran out of SRV descriptor heap slots.";
			return false;
		}

		return true;
	}

	ComPtr<ID3D12Resource> MakeTexture2D(
		ID3D12Device* device,
		UINT width,
		UINT height,
		DXGI_FORMAT format,
		D3D12_RESOURCE_STATES initialState,
		LPCWSTR name)
	{
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		heap.CreationNodeMask = 1;
		heap.VisibleNodeMask = 1;

		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = width;
		desc.Height = height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

		ComPtr<ID3D12Resource> resource;
		DX12_THROW_IF_FAILED(device->CreateCommittedResource(
			&heap,
			D3D12_HEAP_FLAG_NONE,
			&desc,
			initialState,
			nullptr,
			IID_PPV_ARGS(&resource)));
		resource->SetName(name);
		return resource;
	}

	ComPtr<ID3D12Resource> MakeConstantBuffer(ID3D12Device* device, UINT64 size, void** mapped)
	{
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_UPLOAD;
		heap.CreationNodeMask = 1;
		heap.VisibleNodeMask = 1;

		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = size;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		ComPtr<ID3D12Resource> resource;
		DX12_THROW_IF_FAILED(device->CreateCommittedResource(
			&heap,
			D3D12_HEAP_FLAG_NONE,
			&desc,
			D3D12_RESOURCE_STATE_GENERIC_READ,
			nullptr,
			IID_PPV_ARGS(&resource)));
		DX12_THROW_IF_FAILED(resource->Map(0, nullptr, mapped));
		resource->SetName(L"RadianceCascadesConstants");
		return resource;
	}

	ComPtr<ID3D12Resource> MakeStructuredBuffer(ID3D12Device* device, UINT64 size, LPCWSTR name)
	{
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		heap.CreationNodeMask = 1;
		heap.VisibleNodeMask = 1;

		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = size;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

		ComPtr<ID3D12Resource> resource;
		DX12_THROW_IF_FAILED(device->CreateCommittedResource(
			&heap,
			D3D12_HEAP_FLAG_NONE,
			&desc,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			nullptr,
			IID_PPV_ARGS(&resource)));
		resource->SetName(name);
		return resource;
	}

	std::string NarrowWideString(const wchar_t* text)
	{
		if (!text || text[0] == L'\0')
			return {};

		const int requiredSize = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
		if (requiredSize <= 1)
			return {};

		std::string result(static_cast<size_t>(requiredSize), '\0');
		WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), requiredSize, nullptr, nullptr);
		result.resize(static_cast<size_t>(requiredSize - 1));
		return result;
	}

	bool SupportsInlineRayQuery(ID3D12Device* device, std::string& err)
	{
		D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
		const HRESULT hr = device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
		if (FAILED(hr))
		{
			std::ostringstream stream;
			stream << "RadianceCascadesRenderer: failed to query DXR support. HRESULT: 0x"
				<< std::hex << static_cast<unsigned long>(hr) << std::dec << " (" << hr << ")";
			err = stream.str();
			return false;
		}

		if (options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
		{
			err = "RadianceCascadesRenderer: DXR RayQuery requires D3D12_RAYTRACING_TIER_1_1 support.";
			return false;
		}

		return true;
	}

	bool CompileAndCreatePso(
		ID3D12Device* device,
		ID3D12RootSignature* rootSignature,
		const wchar_t* filePath,
		const wchar_t* entryPoint,
		ComPtr<ID3D12PipelineState>& outPso,
		std::string& err)
	{
		ShaderCompileRequest request{};
		request.FilePath = filePath;
		request.EntryPoint = entryPoint;
		request.TargetProfile = L"cs_6_5";
		request.Stage = ShaderStage::Compute;

		DX12Shader shader;
		if (!shader.Compile(request))
		{
			err = std::string("RadianceCascadesRenderer: shader compile failed: ")
				+ (shader.GetLastErrorMessage() ? shader.GetLastErrorMessage() : "unknown error");
			return false;
		}

		D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
		desc.pRootSignature = rootSignature;
		desc.CS = shader.GetBytecode();
		const HRESULT hr = device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&outPso));
		if (FAILED(hr))
		{
			std::ostringstream stream;
			stream << "RadianceCascadesRenderer: CreateComputePipelineState failed for "
				<< NarrowWideString(filePath)
				<< " entry " << NarrowWideString(entryPoint)
				<< ". HRESULT: 0x" << std::hex << static_cast<unsigned long>(hr) << std::dec << " (" << hr << ")"
				<< " BytecodeLength: " << shader.GetBytecode().BytecodeLength;

			const HRESULT deviceReason = device->GetDeviceRemovedReason();
			if (FAILED(deviceReason))
				stream << " DeviceRemovedReason: 0x" << std::hex << static_cast<unsigned long>(deviceReason) << std::dec << " (" << deviceReason << ")";

			err = stream.str();
			return false;
		}

		return true;
	}

	void Transition(
		ID3D12GraphicsCommandList* commandList,
		ID3D12Resource* resource,
		D3D12_RESOURCE_STATES& currentState,
		D3D12_RESOURCE_STATES newState)
	{
		if (!resource || currentState == newState)
		{
			return;
		}

		const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(resource, currentState, newState);
		commandList->ResourceBarrier(1, &barrier);
		currentState = newState;
	}

	void UavBarrier(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource)
	{
		if (!commandList || !resource)
			return;

		const auto barrier = CD3DX12_RESOURCE_BARRIER::UAV(resource);
		commandList->ResourceBarrier(1, &barrier);
	}
}

bool RadianceCascadesRenderer::Initialize(UINT width, UINT height)
{
	if (width == 0 || height == 0)
	{
		mLastError = "RadianceCascadesRenderer: invalid resolution.";
		mInitFailed = true;
		return false;
	}

	mLastError.clear();

	try
	{
		if (!mRootSignature && !CreateRootSignature())
			return false;
		if (!mTraceRootSignature && !CreateTraceRootSignature())
			return false;
		if (!mDispatchIndirectSignature && !CreateCommandSignature())
			return false;
		if (!mClearSparsePso || !mAllocateSparsePso || !mResolveSparseAnchorsPso || !mCompactSparsePso || !mBuildIndirectPso || !mTraceSparsePso || !mGatherSparsePso || !mCompositePso)
		{
			if (!CreatePipelines())
				return false;
		}

		if (!mConstantBuffer)
		{
			mConstantBuffer = MakeConstantBuffer(DX12Context_GetDevice(), sizeof(RadianceCascadesConstants), reinterpret_cast<void**>(&mMappedConstants));
		}

		mWidth = width;
		mHeight = height;

		if (!CreateResolutionBuffers())
			return false;
		if (!CreateSparseProbeResources(RadianceCascadesSettings{}.SparseProbeTableCapacity))
			return false;
		if (!CreateDescriptors())
			return false;

		mFrameIndex = 0;
		mSparseFrameIndex = 0;
		mIsInitialized = true;
		mInitFailed = false;
		return true;
	}
	catch (const std::exception& ex)
	{
		mLastError = std::string("RadianceCascadesRenderer::Initialize: ") + ex.what();
		mInitFailed = true;
		mIsInitialized = false;
		return false;
	}
}

bool RadianceCascadesRenderer::EnsureSize(UINT width, UINT height)
{
	if (!mIsInitialized)
		return Initialize(width, height);

	if (width == mWidth && height == mHeight)
		return true;

	if (width == 0 || height == 0)
		return false;

	mWidth = width;
	mHeight = height;
	return CreateResolutionBuffers() && CreateSparseProbeResources(mSparseProbeTableCapacity) && CreateDescriptors();
}

void RadianceCascadesRenderer::Shutdown()
{
	if (mConstantBuffer && mMappedConstants)
	{
		mConstantBuffer->Unmap(0, nullptr);
		mMappedConstants = nullptr;
	}

	mConstantBuffer.Reset();
	mTraceTexture.Reset();
	mHistoryTexture.Reset();
	mSparseProbeBuffer.Reset();
	mActiveProbeListBuffer.Reset();
	mActiveProbeCountBuffer.Reset();
	mIndirectArgsBuffer.Reset();
	mOutputTexture.Reset();
	mDispatchIndirectSignature.Reset();
	mClearSparsePso.Reset();
	mAllocateSparsePso.Reset();
	mResolveSparseAnchorsPso.Reset();
	mCompactSparsePso.Reset();
	mBuildIndirectPso.Reset();
	mTraceSparsePso.Reset();
	mGatherSparsePso.Reset();
	mCompositePso.Reset();
	mTraceRootSignature.Reset();
	mRootSignature.Reset();
	mIsInitialized = false;
	mInitFailed = false;
	mWidth = 0;
	mHeight = 0;
	mFrameIndex = 0;
	mSparseFrameIndex = 0;
	mTraceState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	mSparseProbeState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	mActiveProbeListState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	mActiveProbeCountState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	mIndirectArgsState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	mOutputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	mSparseProbeTableCapacity = 0;
	mClearSparseProbeTable = true;
}

void RadianceCascadesRenderer::Dispatch(
	ID3D12GraphicsCommandList4* cmdList,
	D3D12_GPU_DESCRIPTOR_HANDLE gbufferAlbedoSrv,
	D3D12_GPU_DESCRIPTOR_HANDLE gbufferNormalDepthSrv,
	D3D12_GPU_DESCRIPTOR_HANDLE gbufferMaterialSrv,
	D3D12_GPU_DESCRIPTOR_HANDLE sceneDepthSrv,
	D3D12_GPU_DESCRIPTOR_HANDLE tlasSrv,
	D3D12_GPU_DESCRIPTOR_HANDLE vertexSrv,
	D3D12_GPU_DESCRIPTOR_HANDLE indexSrv,
	D3D12_GPU_DESCRIPTOR_HANDLE instanceInfoSrv,
	D3D12_GPU_DESCRIPTOR_HANDLE materialRangeSrv,
	D3D12_GPU_DESCRIPTOR_HANDLE baseTextureTableSrv,
	const RadianceCascadesSettings& settings,
	const float viewProjInv[16],
	const float currViewProj[16],
	const float prevViewProj[16],
	const float cameraPos[3],
	float sunDirX,
	float sunDirY,
	float sunDirZ,
	float sunR,
	float sunG,
	float sunB,
	float skyR,
	float skyG,
	float skyB,
	const DeferredLightingPass::PointLightGpu* pointLights,
	uint32_t numPointLights,
	float sceneMaxDistance,
	bool resetHistory)
{
	if (!mIsInitialized || !cmdList || !mRootSignature || !mTraceRootSignature || !mDispatchIndirectSignature || !mClearSparsePso || !mAllocateSparsePso || !mResolveSparseAnchorsPso || !mCompactSparsePso || !mBuildIndirectPso || !mTraceSparsePso || !mGatherSparsePso || !mCompositePso)
		return;

	ID3D12DescriptorHeap* const srvHeap = DX12Context_GetSrvDescriptorHeap();
	if (!srvHeap)
		return;

	const uint32_t requestedTableCapacity = (std::clamp)(settings.SparseProbeTableCapacity, 1024u, 262144u);
	if (requestedTableCapacity != mSparseProbeTableCapacity)
	{
		if (!CreateSparseProbeResources(requestedTableCapacity) || !CreateDescriptors())
			return;
	}

	if (resetHistory)
		mFrameIndex = 0;

	UploadConstants(
		settings,
		viewProjInv,
		currViewProj,
		prevViewProj,
		cameraPos,
		sunDirX,
		sunDirY,
		sunDirZ,
		sunR,
		sunG,
		sunB,
		skyR,
		skyG,
		skyB,
		pointLights,
		numPointLights,
		sceneMaxDistance);

	Transition(cmdList, mTraceTexture.Get(), mTraceState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Transition(cmdList, mHistoryTexture.Get(), mHistoryState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	Transition(cmdList, mOutputTexture.Get(), mOutputState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Transition(cmdList, mSparseProbeBuffer.Get(), mSparseProbeState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Transition(cmdList, mActiveProbeListBuffer.Get(), mActiveProbeListState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Transition(cmdList, mActiveProbeCountBuffer.Get(), mActiveProbeCountState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Transition(cmdList, mIndirectArgsBuffer.Get(), mIndirectArgsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	cmdList->SetDescriptorHeaps(1, &srvHeap);
	cmdList->SetComputeRootSignature(mRootSignature.Get());
	cmdList->SetComputeRootConstantBufferView(0, mConstantBuffer->GetGPUVirtualAddress());

	cmdList->SetPipelineState(mClearSparsePso.Get());
	cmdList->SetComputeRootDescriptorTable(10, mTraceUavGpu);
	cmdList->SetComputeRootDescriptorTable(11, mSparseProbeUavGpu);
	cmdList->SetComputeRootDescriptorTable(12, mActiveProbeListUavGpu);
	cmdList->SetComputeRootDescriptorTable(13, mActiveProbeCountUavGpu);
	cmdList->SetComputeRootDescriptorTable(14, mIndirectArgsUavGpu);
	cmdList->Dispatch((mSparseProbeTableCapacity + 63u) / 64u, 1u, 1u);
	UavBarrier(cmdList, mSparseProbeBuffer.Get());
	UavBarrier(cmdList, mActiveProbeListBuffer.Get());
	UavBarrier(cmdList, mActiveProbeCountBuffer.Get());
	UavBarrier(cmdList, mIndirectArgsBuffer.Get());
	mClearSparseProbeTable = false;

	cmdList->SetPipelineState(mAllocateSparsePso.Get());
	cmdList->SetComputeRootDescriptorTable(1, gbufferAlbedoSrv);
	cmdList->SetComputeRootDescriptorTable(2, gbufferNormalDepthSrv);
	cmdList->SetComputeRootDescriptorTable(3, gbufferMaterialSrv);
	cmdList->SetComputeRootDescriptorTable(4, sceneDepthSrv);
	cmdList->SetComputeRootDescriptorTable(5, tlasSrv);
	cmdList->SetComputeRootDescriptorTable(6, vertexSrv);
	cmdList->SetComputeRootDescriptorTable(7, indexSrv);
	cmdList->SetComputeRootDescriptorTable(8, instanceInfoSrv);
	cmdList->SetComputeRootDescriptorTable(9, materialRangeSrv);
	cmdList->SetComputeRootDescriptorTable(10, mTraceUavGpu);
	cmdList->SetComputeRootDescriptorTable(11, mSparseProbeUavGpu);
	cmdList->SetComputeRootDescriptorTable(12, mActiveProbeListUavGpu);
	cmdList->SetComputeRootDescriptorTable(13, mActiveProbeCountUavGpu);
	cmdList->SetComputeRootDescriptorTable(14, mIndirectArgsUavGpu);
	cmdList->Dispatch((mWidth + kGroupSize - 1) / kGroupSize, (mHeight + kGroupSize - 1) / kGroupSize, 1);
	UavBarrier(cmdList, mSparseProbeBuffer.Get());

	cmdList->SetPipelineState(mResolveSparseAnchorsPso.Get());
	cmdList->Dispatch((mWidth + kGroupSize - 1) / kGroupSize, (mHeight + kGroupSize - 1) / kGroupSize, 1);
	UavBarrier(cmdList, mSparseProbeBuffer.Get());

	cmdList->SetPipelineState(mCompactSparsePso.Get());
	cmdList->Dispatch((mSparseProbeTableCapacity + 63u) / 64u, 1u, 1u);
	UavBarrier(cmdList, mActiveProbeListBuffer.Get());
	UavBarrier(cmdList, mActiveProbeCountBuffer.Get());

	cmdList->SetPipelineState(mBuildIndirectPso.Get());
	cmdList->Dispatch(1u, 1u, 1u);
	UavBarrier(cmdList, mIndirectArgsBuffer.Get());

	Transition(cmdList, mActiveProbeListBuffer.Get(), mActiveProbeListState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	Transition(cmdList, mIndirectArgsBuffer.Get(), mIndirectArgsState, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);

	cmdList->SetComputeRootSignature(mTraceRootSignature.Get());
	cmdList->SetComputeRootConstantBufferView(0, mConstantBuffer->GetGPUVirtualAddress());
	cmdList->SetComputeRootDescriptorTable(1, tlasSrv);
	cmdList->SetComputeRootDescriptorTable(2, vertexSrv);
	cmdList->SetComputeRootDescriptorTable(3, indexSrv);
	cmdList->SetComputeRootDescriptorTable(4, instanceInfoSrv);
	cmdList->SetComputeRootDescriptorTable(5, materialRangeSrv);
	cmdList->SetComputeRootDescriptorTable(6, mActiveProbeListSrvGpu);
	cmdList->SetComputeRootDescriptorTable(7, baseTextureTableSrv);
	cmdList->SetComputeRootDescriptorTable(8, mSparseProbeUavGpu);
	cmdList->SetComputeRootDescriptorTable(9, mActiveProbeCountUavGpu);
	cmdList->SetPipelineState(mTraceSparsePso.Get());
	cmdList->ExecuteIndirect(mDispatchIndirectSignature.Get(), 1, mIndirectArgsBuffer.Get(), 0, nullptr, 0);
	UavBarrier(cmdList, mSparseProbeBuffer.Get());

	Transition(cmdList, mIndirectArgsBuffer.Get(), mIndirectArgsState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Transition(cmdList, mActiveProbeListBuffer.Get(), mActiveProbeListState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Transition(cmdList, mActiveProbeCountBuffer.Get(), mActiveProbeCountState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	Transition(cmdList, mTraceTexture.Get(), mTraceState, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

	cmdList->SetComputeRootSignature(mRootSignature.Get());
	cmdList->SetComputeRootConstantBufferView(0, mConstantBuffer->GetGPUVirtualAddress());
	cmdList->SetPipelineState(mGatherSparsePso.Get());
	cmdList->SetComputeRootDescriptorTable(2, gbufferNormalDepthSrv);
	cmdList->SetComputeRootDescriptorTable(9, mActiveProbeListSrvGpu);
	cmdList->SetComputeRootDescriptorTable(10, mTraceUavGpu);
	cmdList->SetComputeRootDescriptorTable(11, mSparseProbeUavGpu);
	cmdList->Dispatch((mWidth + kGroupSize - 1) / kGroupSize, (mHeight + kGroupSize - 1) / kGroupSize, 1);

	Transition(cmdList, mTraceTexture.Get(), mTraceState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	cmdList->SetPipelineState(mCompositePso.Get());
	cmdList->SetComputeRootConstantBufferView(0, mConstantBuffer->GetGPUVirtualAddress());
	cmdList->SetComputeRootDescriptorTable(1, mTraceSrvGpu);
	cmdList->SetComputeRootDescriptorTable(2, mHistorySrvGpu);
	cmdList->SetComputeRootDescriptorTable(3, gbufferNormalDepthSrv);
	cmdList->SetComputeRootDescriptorTable(4, sceneDepthSrv);
	cmdList->SetComputeRootDescriptorTable(5, tlasSrv);
	cmdList->SetComputeRootDescriptorTable(6, vertexSrv);
	cmdList->SetComputeRootDescriptorTable(7, indexSrv);
	cmdList->SetComputeRootDescriptorTable(8, instanceInfoSrv);
	cmdList->SetComputeRootDescriptorTable(9, materialRangeSrv);
	cmdList->SetComputeRootDescriptorTable(10, mOutputUavGpu);
	cmdList->Dispatch((mWidth + kGroupSize - 1) / kGroupSize, (mHeight + kGroupSize - 1) / kGroupSize, 1);

	Transition(cmdList, mOutputTexture.Get(), mOutputState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	Transition(cmdList, mOutputTexture.Get(), mOutputState, D3D12_RESOURCE_STATE_COPY_SOURCE);
	Transition(cmdList, mHistoryTexture.Get(), mHistoryState, D3D12_RESOURCE_STATE_COPY_DEST);
	cmdList->CopyResource(mHistoryTexture.Get(), mOutputTexture.Get());
	Transition(cmdList, mHistoryTexture.Get(), mHistoryState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	Transition(cmdList, mOutputTexture.Get(), mOutputState, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

	++mFrameIndex;
	++mSparseFrameIndex;
}

bool RadianceCascadesRenderer::CreateRootSignature()
{
	ID3D12Device* device = DX12Context_GetDevice();
	if (!device)
	{
		mLastError = "RadianceCascadesRenderer: no D3D12 device.";
		return false;
	}

	D3D12_DESCRIPTOR_RANGE srvRanges[10]{};
	for (UINT i = 0; i < 10; ++i)
	{
		srvRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		srvRanges[i].NumDescriptors = 1;
		srvRanges[i].BaseShaderRegister = i;
		srvRanges[i].RegisterSpace = 0;
		srvRanges[i].OffsetInDescriptorsFromTableStart = 0;
	}

	D3D12_DESCRIPTOR_RANGE uavRanges[5]{};
	for (UINT i = 0; i < 5; ++i)
	{
		uavRanges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		uavRanges[i].NumDescriptors = 1;
		uavRanges[i].BaseShaderRegister = i;
		uavRanges[i].RegisterSpace = 0;
		uavRanges[i].OffsetInDescriptorsFromTableStart = 0;
	}

	D3D12_ROOT_PARAMETER params[15]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].Descriptor.ShaderRegister = 0;
	params[0].Descriptor.RegisterSpace = 0;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	for (UINT i = 0; i < 10; ++i)
	{
		params[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[i + 1].DescriptorTable.NumDescriptorRanges = 1;
		params[i + 1].DescriptorTable.pDescriptorRanges = &srvRanges[i];
		params[i + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	}

	for (UINT i = 0; i < 5; ++i)
	{
		params[10 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[10 + i].DescriptorTable.NumDescriptorRanges = 1;
		params[10 + i].DescriptorTable.pDescriptorRanges = &uavRanges[i];
		params[10 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	}

	D3D12_STATIC_SAMPLER_DESC linearClamp{};
	linearClamp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	linearClamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	linearClamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	linearClamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	linearClamp.ShaderRegister = 0;
	linearClamp.RegisterSpace = 0;
	linearClamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	linearClamp.MaxLOD = D3D12_FLOAT32_MAX;

	D3D12_ROOT_SIGNATURE_DESC desc{};
	desc.NumParameters = static_cast<UINT>(std::size(params));
	desc.pParameters = params;
	desc.NumStaticSamplers = 1;
	desc.pStaticSamplers = &linearClamp;
	desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

	ComPtr<ID3DBlob> serialized;
	ComPtr<ID3DBlob> errors;
	if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
	{
		mLastError = "RadianceCascadesRenderer: D3D12SerializeRootSignature failed.";
		return false;
	}

	if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&mRootSignature))))
	{
		mLastError = "RadianceCascadesRenderer: CreateRootSignature failed.";
		return false;
	}

	return true;
}

bool RadianceCascadesRenderer::CreateTraceRootSignature()
{
	ID3D12Device* device = DX12Context_GetDevice();
	if (!device)
	{
		mLastError = "RadianceCascadesRenderer: no D3D12 device for trace root signature.";
		return false;
	}

	D3D12_DESCRIPTOR_RANGE ranges[9]{};
	for (UINT i = 0; i < 6; ++i)
	{
		ranges[i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[i].NumDescriptors = 1;
		ranges[i].BaseShaderRegister = 4 + i;
		ranges[i].RegisterSpace = 0;
		ranges[i].OffsetInDescriptorsFromTableStart = 0;
	}

	ranges[6].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
	ranges[6].NumDescriptors = 32;
	ranges[6].BaseShaderRegister = 10;
	ranges[6].RegisterSpace = 0;
	ranges[6].OffsetInDescriptorsFromTableStart = 0;

	ranges[7].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[7].NumDescriptors = 1;
	ranges[7].BaseShaderRegister = 1;
	ranges[7].RegisterSpace = 0;
	ranges[7].OffsetInDescriptorsFromTableStart = 0;

	ranges[8].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
	ranges[8].NumDescriptors = 1;
	ranges[8].BaseShaderRegister = 3;
	ranges[8].RegisterSpace = 0;
	ranges[8].OffsetInDescriptorsFromTableStart = 0;

	D3D12_ROOT_PARAMETER params[10]{};
	params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
	params[0].Descriptor.ShaderRegister = 0;
	params[0].Descriptor.RegisterSpace = 0;
	params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

	for (UINT i = 0; i < static_cast<UINT>(std::size(ranges)); ++i)
	{
		params[i + 1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[i + 1].DescriptorTable.NumDescriptorRanges = 1;
		params[i + 1].DescriptorTable.pDescriptorRanges = &ranges[i];
		params[i + 1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	}

	D3D12_ROOT_SIGNATURE_DESC desc{};
	desc.NumParameters = static_cast<UINT>(std::size(params));
	desc.pParameters = params;
	D3D12_STATIC_SAMPLER_DESC linearClamp{};
	linearClamp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
	linearClamp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	linearClamp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	linearClamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
	linearClamp.ShaderRegister = 0;
	linearClamp.RegisterSpace = 0;
	linearClamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
	linearClamp.MaxLOD = D3D12_FLOAT32_MAX;
	desc.NumStaticSamplers = 1;
	desc.pStaticSamplers = &linearClamp;
	desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

	ComPtr<ID3DBlob> serialized;
	ComPtr<ID3DBlob> errors;
	if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
	{
		mLastError = "RadianceCascadesRenderer: D3D12SerializeRootSignature failed for trace root signature.";
		return false;
	}

	if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&mTraceRootSignature))))
	{
		mLastError = "RadianceCascadesRenderer: CreateRootSignature failed for trace root signature.";
		return false;
	}

	return true;
}

bool RadianceCascadesRenderer::CreateCommandSignature()
{
	ID3D12Device* device = DX12Context_GetDevice();
	if (!device)
	{
		mLastError = "RadianceCascadesRenderer: no D3D12 device for command signature.";
		return false;
	}

	D3D12_INDIRECT_ARGUMENT_DESC argument{};
	argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

	D3D12_COMMAND_SIGNATURE_DESC desc{};
	desc.ByteStride = sizeof(uint32_t) * 3;
	desc.NumArgumentDescs = 1;
	desc.pArgumentDescs = &argument;

	if (FAILED(device->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&mDispatchIndirectSignature))))
	{
		mLastError = "RadianceCascadesRenderer: CreateCommandSignature failed.";
		return false;
	}

	return true;
}

bool RadianceCascadesRenderer::CreatePipelines()
{
	ID3D12Device* device = DX12Context_GetDevice();
	if (!device || !mRootSignature || !mTraceRootSignature)
	{
		mLastError = "RadianceCascadesRenderer: missing device or root signature.";
		return false;
	}

	if (!SupportsInlineRayQuery(device, mLastError))
	{
		return false;
	}

	return CompileAndCreatePso(device, mRootSignature.Get(), L"Data/Shaders/RadianceCascadesSparse.hlsl", L"ClearSparseCS", mClearSparsePso, mLastError)
		&& CompileAndCreatePso(device, mRootSignature.Get(), L"Data/Shaders/RadianceCascadesSparse.hlsl", L"AllocateSparseCS", mAllocateSparsePso, mLastError)
		&& CompileAndCreatePso(device, mRootSignature.Get(), L"Data/Shaders/RadianceCascadesSparse.hlsl", L"ResolveSparseAnchorsCS", mResolveSparseAnchorsPso, mLastError)
		&& CompileAndCreatePso(device, mRootSignature.Get(), L"Data/Shaders/RadianceCascadesSparse.hlsl", L"CompactSparseCS", mCompactSparsePso, mLastError)
		&& CompileAndCreatePso(device, mRootSignature.Get(), L"Data/Shaders/RadianceCascadesSparse.hlsl", L"BuildIndirectArgsCS", mBuildIndirectPso, mLastError)
		&& CompileAndCreatePso(device, mTraceRootSignature.Get(), L"Data/Shaders/RadianceCascadesSparse.hlsl", L"TraceSparseCS", mTraceSparsePso, mLastError)
		&& CompileAndCreatePso(device, mRootSignature.Get(), L"Data/Shaders/RadianceCascadesSparse.hlsl", L"GatherSparseCS", mGatherSparsePso, mLastError)
		&& CompileAndCreatePso(device, mRootSignature.Get(), L"Data/Shaders/RadianceCascadesComposite.hlsl", L"CSMain", mCompositePso, mLastError);
}

bool RadianceCascadesRenderer::CreateResolutionBuffers()
{
	ID3D12Device* device = DX12Context_GetDevice();
	if (!device || mWidth == 0 || mHeight == 0)
	{
		mLastError = "RadianceCascadesRenderer: invalid device or resolution for buffers.";
		return false;
	}

	try
	{
		mTraceTexture = MakeTexture2D(device, mWidth, mHeight, kRadianceCascadesFormat, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, L"RadianceCascadesTrace");
		mHistoryTexture = MakeTexture2D(device, mWidth, mHeight, kRadianceCascadesFormat, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"RadianceCascadesHistory");
		mOutputTexture = MakeTexture2D(device, mWidth, mHeight, kRadianceCascadesFormat, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, L"RadianceCascadesOutput");
		mTraceState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		mHistoryState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		mOutputState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
		return true;
	}
	catch (const std::exception& ex)
	{
		mLastError = std::string("RadianceCascadesRenderer::CreateResolutionBuffers: ") + ex.what();
		return false;
	}
}

bool RadianceCascadesRenderer::CreateDescriptors()
{
	ID3D12Device* device = DX12Context_GetDevice();
	if (!device || !mTraceTexture || !mHistoryTexture || !mOutputTexture || !mSparseProbeBuffer || !mActiveProbeListBuffer || !mActiveProbeCountBuffer || !mIndirectArgsBuffer)
	{
		mLastError = "RadianceCascadesRenderer: missing resources for descriptor creation.";
		return false;
	}

	if (!mDescriptorsAllocated)
	{
		if (!AllocDescriptor(mTraceSrvCpu, mTraceSrvGpu, mLastError)
			|| !AllocDescriptor(mTraceUavCpu, mTraceUavGpu, mLastError)
			|| !AllocDescriptor(mHistorySrvCpu, mHistorySrvGpu, mLastError)
			|| !AllocDescriptor(mSparseProbeSrvCpu, mSparseProbeSrvGpu, mLastError)
			|| !AllocDescriptor(mSparseProbeUavCpu, mSparseProbeUavGpu, mLastError)
			|| !AllocDescriptor(mActiveProbeListSrvCpu, mActiveProbeListSrvGpu, mLastError)
			|| !AllocDescriptor(mActiveProbeListUavCpu, mActiveProbeListUavGpu, mLastError)
			|| !AllocDescriptor(mActiveProbeCountSrvCpu, mActiveProbeCountSrvGpu, mLastError)
			|| !AllocDescriptor(mActiveProbeCountUavCpu, mActiveProbeCountUavGpu, mLastError)
			|| !AllocDescriptor(mIndirectArgsUavCpu, mIndirectArgsUavGpu, mLastError)
			|| !AllocDescriptor(mOutputSrvCpu, mOutputSrvGpu, mLastError)
			|| !AllocDescriptor(mOutputUavCpu, mOutputUavGpu, mLastError))
		{
			return false;
		}

		mDescriptorsAllocated = true;
	}

	D3D12_SHADER_RESOURCE_VIEW_DESC traceSrvDesc{};
	traceSrvDesc.Format = kRadianceCascadesFormat;
	traceSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
	traceSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	traceSrvDesc.Texture2D.MipLevels = 1;

	D3D12_UNORDERED_ACCESS_VIEW_DESC traceUavDesc{};
	traceUavDesc.Format = kRadianceCascadesFormat;
	traceUavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;

	D3D12_SHADER_RESOURCE_VIEW_DESC sparseProbeSrvDesc{};
	sparseProbeSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	sparseProbeSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	sparseProbeSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
	sparseProbeSrvDesc.Buffer.NumElements = mSparseProbeTableCapacity;
	sparseProbeSrvDesc.Buffer.StructureByteStride = sizeof(RadianceCascadesSparseProbeGpu);
	sparseProbeSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

	D3D12_UNORDERED_ACCESS_VIEW_DESC sparseProbeUavDesc{};
	sparseProbeUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	sparseProbeUavDesc.Format = DXGI_FORMAT_UNKNOWN;
	sparseProbeUavDesc.Buffer.NumElements = mSparseProbeTableCapacity;
	sparseProbeUavDesc.Buffer.StructureByteStride = sizeof(RadianceCascadesSparseProbeGpu);

	D3D12_SHADER_RESOURCE_VIEW_DESC activeProbeListSrvDesc{};
	activeProbeListSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	activeProbeListSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	activeProbeListSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
	activeProbeListSrvDesc.Buffer.NumElements = mSparseProbeTableCapacity;
	activeProbeListSrvDesc.Buffer.StructureByteStride = sizeof(uint32_t);
	activeProbeListSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

	D3D12_UNORDERED_ACCESS_VIEW_DESC activeProbeListUavDesc{};
	activeProbeListUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	activeProbeListUavDesc.Format = DXGI_FORMAT_UNKNOWN;
	activeProbeListUavDesc.Buffer.NumElements = mSparseProbeTableCapacity;
	activeProbeListUavDesc.Buffer.StructureByteStride = sizeof(uint32_t);

	D3D12_SHADER_RESOURCE_VIEW_DESC activeProbeCountSrvDesc{};
	activeProbeCountSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
	activeProbeCountSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	activeProbeCountSrvDesc.Format = DXGI_FORMAT_UNKNOWN;
	activeProbeCountSrvDesc.Buffer.NumElements = 1;
	activeProbeCountSrvDesc.Buffer.StructureByteStride = sizeof(uint32_t);
	activeProbeCountSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

	D3D12_UNORDERED_ACCESS_VIEW_DESC activeProbeCountUavDesc{};
	activeProbeCountUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	activeProbeCountUavDesc.Format = DXGI_FORMAT_UNKNOWN;
	activeProbeCountUavDesc.Buffer.NumElements = 1;
	activeProbeCountUavDesc.Buffer.StructureByteStride = sizeof(uint32_t);

	D3D12_UNORDERED_ACCESS_VIEW_DESC indirectArgsUavDesc{};
	indirectArgsUavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
	indirectArgsUavDesc.Format = DXGI_FORMAT_UNKNOWN;
	indirectArgsUavDesc.Buffer.NumElements = 3;
	indirectArgsUavDesc.Buffer.StructureByteStride = sizeof(uint32_t);

	device->CreateShaderResourceView(mTraceTexture.Get(), &traceSrvDesc, mTraceSrvCpu);
	device->CreateUnorderedAccessView(mTraceTexture.Get(), nullptr, &traceUavDesc, mTraceUavCpu);
	device->CreateShaderResourceView(mHistoryTexture.Get(), &traceSrvDesc, mHistorySrvCpu);
	device->CreateShaderResourceView(mSparseProbeBuffer.Get(), &sparseProbeSrvDesc, mSparseProbeSrvCpu);
	device->CreateUnorderedAccessView(mSparseProbeBuffer.Get(), nullptr, &sparseProbeUavDesc, mSparseProbeUavCpu);
	device->CreateShaderResourceView(mActiveProbeListBuffer.Get(), &activeProbeListSrvDesc, mActiveProbeListSrvCpu);
	device->CreateUnorderedAccessView(mActiveProbeListBuffer.Get(), nullptr, &activeProbeListUavDesc, mActiveProbeListUavCpu);
	device->CreateShaderResourceView(mActiveProbeCountBuffer.Get(), &activeProbeCountSrvDesc, mActiveProbeCountSrvCpu);
	device->CreateUnorderedAccessView(mActiveProbeCountBuffer.Get(), nullptr, &activeProbeCountUavDesc, mActiveProbeCountUavCpu);
	device->CreateUnorderedAccessView(mIndirectArgsBuffer.Get(), nullptr, &indirectArgsUavDesc, mIndirectArgsUavCpu);
	device->CreateShaderResourceView(mOutputTexture.Get(), &traceSrvDesc, mOutputSrvCpu);
	device->CreateUnorderedAccessView(mOutputTexture.Get(), nullptr, &traceUavDesc, mOutputUavCpu);
	return true;
}

bool RadianceCascadesRenderer::CreateSparseProbeResources(uint32_t tableCapacity)
{
	ID3D12Device* device = DX12Context_GetDevice();
	if (!device)
	{
		mLastError = "RadianceCascadesRenderer: missing device for sparse probe resources.";
		return false;
	}

	tableCapacity = (std::max)(tableCapacity, 1024u);
	if (mSparseProbeBuffer && mActiveProbeListBuffer && mActiveProbeCountBuffer && mIndirectArgsBuffer && mSparseProbeTableCapacity == tableCapacity)
		return true;

	try
	{
		mSparseProbeBuffer = MakeStructuredBuffer(device, sizeof(RadianceCascadesSparseProbeGpu) * static_cast<UINT64>(tableCapacity), L"RadianceCascadesSparseProbeTable");
		mActiveProbeListBuffer = MakeStructuredBuffer(device, sizeof(uint32_t) * static_cast<UINT64>(tableCapacity), L"RadianceCascadesActiveProbeList");
		mActiveProbeCountBuffer = MakeStructuredBuffer(device, sizeof(uint32_t), L"RadianceCascadesActiveProbeCount");
		mIndirectArgsBuffer = MakeStructuredBuffer(device, sizeof(uint32_t) * 3ull, L"RadianceCascadesIndirectDispatchArgs");
		mSparseProbeState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		mActiveProbeListState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		mActiveProbeCountState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		mIndirectArgsState = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
		mSparseProbeTableCapacity = tableCapacity;
		mClearSparseProbeTable = true;
		mSparseFrameIndex = 0;
		return true;
	}
	catch (const std::exception& ex)
	{
		mLastError = std::string("RadianceCascadesRenderer::CreateSparseProbeResources: ") + ex.what();
		return false;
	}
}

void RadianceCascadesRenderer::UploadConstants(
	const RadianceCascadesSettings& settings,
	const float viewProjInv[16],
	const float currViewProj[16],
	const float prevViewProj[16],
	const float cameraPos[3],
	float sunDirX,
	float sunDirY,
	float sunDirZ,
	float sunR,
	float sunG,
	float sunB,
	float skyR,
	float skyG,
	float skyB,
	const DeferredLightingPass::PointLightGpu* pointLights,
	uint32_t numPointLights,
	float sceneMaxDistance)
{
	if (!mMappedConstants)
		return;

	RadianceCascadesConstants constants{};
	constants.FrameWidth = mWidth;
	constants.FrameHeight = mHeight;
	constants.FrameIndex = mFrameIndex;
	constants.CascadeCount = (std::max)(settings.CascadeCount, 1u);
	constants.ProbeSpacingBase = (std::max)(settings.ProbeSpacingBase, 1u);
	constants.RaysPerProbe = (std::clamp)(settings.RaysPerProbe, 1u, 16u);
	constants.DebugView = settings.DebugView;
	constants.RayLengthBase = (std::max)(settings.RayLengthBase, 0.01f);
	constants.RayLengthScale = (std::max)(settings.RayLengthScale, 1.0f);
	constants.IntervalLengthScale = (std::max)(settings.IntervalLengthScale, 1.0f);
	constants.Hysteresis = std::clamp(settings.Hysteresis, 0.0f, 0.99f);
	constants.GiIntensity = settings.GiIntensity;
	constants.SunDir[0] = sunDirX;
	constants.SunDir[1] = sunDirY;
	constants.SunDir[2] = sunDirZ;
	constants.ProbeSpacingBaseFloat = static_cast<float>(constants.ProbeSpacingBase);
	constants.SunColor[0] = sunR;
	constants.SunColor[1] = sunG;
	constants.SunColor[2] = sunB;
	constants.NumPointLights = static_cast<int32_t>((std::min)(numPointLights, static_cast<uint32_t>(DeferredLightingPass::kMaxPointLights)));
	constants.SkyColor[0] = skyR;
	constants.SkyColor[1] = skyG;
	constants.SkyColor[2] = skyB;
	constants.SceneMaxDistance = sceneMaxDistance;
	constants.ColorBleedingStrength = settings.ColorBleedingStrength;
	constants.SparseProbeTableCapacity = mSparseProbeTableCapacity;
	constants.SparseProbeCount = mClearSparseProbeTable ? 1u : 0u;
	constants.SparseProbeCellSize = (std::max)(settings.SparseProbeCellSize, 1u);
	constants.SparseProbeSearchSteps = (std::clamp)(settings.SparseProbeSearchSteps, 4u, 128u);
	constants.SparseProbeReuseStrength = std::clamp(settings.SparseProbeReuseStrength, 0.0f, 1.0f);
	constants.RayBias = (std::max)(settings.RayBias, 0.0f);
	constants.SpatialFilterStrength = std::clamp(settings.SpatialFilterStrength, 0.0f, 2.0f);
	constants.HistoryClampScale = std::clamp(settings.HistoryClampScale, 0.0f, 2.0f);
	constants.HistoryDepthSensitivity = (std::max)(settings.HistoryDepthSensitivity, 1.0f);
	constants.HistoryNormalThreshold = std::clamp(settings.HistoryNormalThreshold, 0.0f, 0.999f);
	constants.SparseFrameIndex = mSparseFrameIndex;

	if (viewProjInv)
		std::memcpy(constants.ViewProjInv, viewProjInv, sizeof(constants.ViewProjInv));
	if (currViewProj)
		std::memcpy(constants.CurrViewProj, currViewProj, sizeof(constants.CurrViewProj));
	if (prevViewProj)
		std::memcpy(constants.PrevViewProj, prevViewProj, sizeof(constants.PrevViewProj));
	if (cameraPos)
		std::memcpy(constants.CameraPos, cameraPos, sizeof(constants.CameraPos));
	if (pointLights != nullptr && constants.NumPointLights > 0)
	{
		std::memcpy(constants.PointLights, pointLights, static_cast<size_t>(constants.NumPointLights) * sizeof(DeferredLightingPass::PointLightGpu));
	}

	*mMappedConstants = constants;
}
