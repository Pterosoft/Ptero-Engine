#pragma once

#include "DX12Helper.h"
#include "DX12ShaderCompiler.h"
#include "d3dx12.h"

#include <DirectXMath.h>
#include <d3d12.h>
#include <string>
#include <vector>
#include <wrl/client.h>

static constexpr UINT kDefaultPointShadowMapSize = 1024;
static constexpr DXGI_FORMAT kPointShadowDepthFormat = DXGI_FORMAT_D32_FLOAT;
static constexpr DXGI_FORMAT kPointShadowTypelessFormat = DXGI_FORMAT_R32_TYPELESS;
static constexpr DXGI_FORMAT kPointShadowResourceFormat = DXGI_FORMAT_R32_FLOAT;
static constexpr UINT kMaxShadowCastingPointLights = 4;

extern "C"
{
	ID3D12Device* __stdcall DX12Context_GetDevice();
	bool __stdcall DX12Context_AllocateSrvDescriptor(
		D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
		D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
}

class PointShadowMapRenderer
{
public:
	struct ShadowedPointLight
	{
		DirectX::XMFLOAT3 Position{};
		float Radius = 1.0f;
	};

	bool Initialize()
	{
		mLastError.clear();
		mIsInitialized = false;

		if (!CreateResources())
			return false;
		if (!CreatePipeline())
			return false;

		mIsInitialized = true;
		return true;
	}

	void Shutdown()
	{
       mShadowDepthTexture.Reset();
		mDsvHeap.Reset();
		mDsvHandles.clear();
		// The SRV slot is kept: the shared heap never frees, so a map-size change
		// (Shutdown + Initialize) must rewrite the same descriptor, not take another.
		mRootSignature.Reset();
		mPipelineState.Reset();
		mVertexShader = DX12Shader{};
		mPixelShader = DX12Shader{};
		mLights.clear();
		mFaceViewProjections.clear();
		mIsInitialized = false;
	}

	bool IsInitialized() const { return mIsInitialized; }
	void SetMapSize(UINT mapSize) { mMapSize = (mapSize > 0) ? mapSize : kDefaultPointShadowMapSize; }
	UINT GetMapSize() const { return mMapSize; }
	void SetShadowBias(float shadowBias) { mShadowBias = shadowBias; }
	void SetSlopeScaledDepthBias(float slopeScaledDepthBias) { mSlopeScaledDepthBias = slopeScaledDepthBias; }

	void BeginFrame(const std::vector<ShadowedPointLight>& lights)
	{
		mLights = lights;
		if (mLights.size() > kMaxShadowCastingPointLights)
			mLights.resize(kMaxShadowCastingPointLights);
		UpdateFaceMatrices();
	}

	int GetActiveLightCount() const { return static_cast<int>(mLights.size()); }
	const ShadowedPointLight* GetLight(int index) const
	{
		return (index >= 0 && index < static_cast<int>(mLights.size())) ? &mLights[index] : nullptr;
	}

	const DirectX::XMFLOAT4X4& GetFaceViewProjection(int lightIndex, int faceIndex) const
	{
		return mFaceViewProjections[lightIndex * kFacesPerLight + faceIndex];
	}

	const DirectX::XMFLOAT4X4* GetAllFaceViewProjections() const
	{
		return mFaceViewProjections.empty() ? nullptr : mFaceViewProjections.data();
	}

	const DirectX::XMFLOAT3& GetLightPosition(int lightIndex) const
	{
		return mLights[lightIndex].Position;
	}

	float GetLightFarPlane(int lightIndex) const
	{
		// Must match the far plane BuildPointLightViewProjection actually used, otherwise
		// the depth pass would normalise against a different range than it rendered with.
		return ShadowFarPlaneFor(mLights[lightIndex].Radius);
	}

	float GetShadowBias() const { return mShadowBias; }

	void BeginShadowFacePass(ID3D12GraphicsCommandList* commandList, int lightIndex, int faceIndex)
	{
		if (!commandList || !mIsInitialized || lightIndex < 0 || lightIndex >= static_cast<int>(mLights.size()) || faceIndex < 0 || faceIndex >= kFacesPerLight)
			return;

		const int sliceIndex = lightIndex * kFacesPerLight + faceIndex;
		auto toDepthWrite = CD3DX12_RESOURCE_BARRIER::Transition(
			mShadowDepthTexture.Get(),
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
			D3D12_RESOURCE_STATE_DEPTH_WRITE,
			D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
		commandList->ResourceBarrier(1, &toDepthWrite);

		commandList->ClearDepthStencilView(mDsvHandles[sliceIndex], D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
		commandList->OMSetRenderTargets(0, nullptr, FALSE, &mDsvHandles[sliceIndex]);

        const D3D12_VIEWPORT vp{ 0.0f, 0.0f, static_cast<float>(mMapSize), static_cast<float>(mMapSize), 0.0f, 1.0f };
		const D3D12_RECT scissor{ 0, 0, static_cast<LONG>(mMapSize), static_cast<LONG>(mMapSize) };
		commandList->RSSetViewports(1, &vp);
		commandList->RSSetScissorRects(1, &scissor);
	}

	void EndShadowFacePass(ID3D12GraphicsCommandList* commandList, int lightIndex, int faceIndex)
	{
		if (!commandList || !mIsInitialized || lightIndex < 0 || lightIndex >= static_cast<int>(mLights.size()) || faceIndex < 0 || faceIndex >= kFacesPerLight)
			return;

		// ALL_SHADER_RESOURCE rather than PIXEL_SHADER_RESOURCE: the deferred
		// lighting pixel shader is no longer the only reader. The volumetric fog
		// injection is a compute pass and samples these same cubemaps, and a
		// pixel-only state would have it read a resource in the wrong state.
		auto toSrv = CD3DX12_RESOURCE_BARRIER::Transition(
			mShadowDepthTexture.Get(),
			D3D12_RESOURCE_STATE_DEPTH_WRITE,
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
			D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
		commandList->ResourceBarrier(1, &toSrv);
	}

	ID3D12RootSignature* GetRootSignature() const { return mRootSignature.Get(); }
	ID3D12PipelineState* GetPipelineState() const { return mPipelineState.Get(); }
	D3D12_GPU_DESCRIPTOR_HANDLE GetShadowTextureArraySrvGpuHandle() const { return mShadowSrvGpu; }
	const char* GetLastError() const { return mLastError.empty() ? nullptr : mLastError.c_str(); }

private:
	static constexpr int kFacesPerLight = 6;
	static constexpr int kTotalShadowFaces = kMaxShadowCastingPointLights * kFacesPerLight;

	// Near plane of every point-shadow face projection.
	static constexpr float kShadowNearPlane = 0.05f;

	// Dragging a light's Radius slider down to zero would hand XMMatrixPerspectiveFovLH a
	// far plane at or below its near plane. That trips a DirectXMath assertion
	// (!XMScalarNearEqual(FarZ, NearZ)) in debug and produces a degenerate projection in
	// release, so keep a minimum depth range. A light this small lights nothing anyway;
	// the point is only that the slider stays usable across its whole travel.
	static float ShadowFarPlaneFor(float radius)
	{
		const float minimumFarPlane = kShadowNearPlane + 0.05f;
		return radius > minimumFarPlane ? radius : minimumFarPlane;
	}

	static DirectX::XMMATRIX BuildPointLightViewProjection(const DirectX::XMFLOAT3& lightPosition, int faceIndex, float farPlane)
	{
		using namespace DirectX;
		static const XMVECTORF32 directions[] =
		{
			{ 1.0f, 0.0f, 0.0f, 0.0f },
			{ -1.0f, 0.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f, 0.0f },
			{ 0.0f, -1.0f, 0.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f, 0.0f },
			{ 0.0f, 0.0f, -1.0f, 0.0f }
		};

		static const XMVECTORF32 upVectors[] =
		{
			{ 0.0f, 1.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f, 0.0f },
			{ 0.0f, 0.0f, -1.0f, 0.0f },
			{ 0.0f, 0.0f, 1.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f, 0.0f },
			{ 0.0f, 1.0f, 0.0f, 0.0f }
		};

		const XMVECTOR eye = XMVectorSet(lightPosition.x, lightPosition.y, lightPosition.z, 1.0f);
		const XMMATRIX view = XMMatrixLookToLH(eye, directions[faceIndex], upVectors[faceIndex]);
		const XMMATRIX proj = XMMatrixPerspectiveFovLH(XM_PIDIV2, 1.0f, kShadowNearPlane, ShadowFarPlaneFor(farPlane));
		return view * proj;
	}

	bool CreateResources()
	{
		ID3D12Device* device = DX12Context_GetDevice();
		if (!device)
		{
			mLastError = "PointShadowMapRenderer: DX12 device is null.";
			return false;
		}

		mShadowDepthTexture.Reset();

		D3D12_HEAP_PROPERTIES defaultHeap{};
		defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
		defaultHeap.CreationNodeMask = 1;
		defaultHeap.VisibleNodeMask = 1;

		D3D12_RESOURCE_DESC texDesc{};
		texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = mMapSize;
		texDesc.Height = mMapSize;
		texDesc.DepthOrArraySize = static_cast<UINT16>(kTotalShadowFaces);
		texDesc.MipLevels = 1;
		texDesc.Format = kPointShadowTypelessFormat;
		texDesc.SampleDesc.Count = 1;
		texDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

		D3D12_CLEAR_VALUE clearVal{};
		clearVal.Format = kPointShadowDepthFormat;
		clearVal.DepthStencil.Depth = 1.0f;
		clearVal.DepthStencil.Stencil = 0;

		if (FAILED(device->CreateCommittedResource(
			&defaultHeap,
			D3D12_HEAP_FLAG_NONE,
			&texDesc,
			D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE,
			&clearVal,
			IID_PPV_ARGS(&mShadowDepthTexture))))
		{
			mLastError = "PointShadowMapRenderer: Failed to create point shadow depth texture array.";
			return false;
		}
		mDsvHeap.Reset();
		mDsvHandles.clear();

		D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc{};
		dsvHeapDesc.NumDescriptors = kTotalShadowFaces;
		dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		if (FAILED(device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&mDsvHeap))))
		{
			mLastError = "PointShadowMapRenderer: Failed to create DSV heap.";
			return false;
		}

		const UINT dsvSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
		D3D12_CPU_DESCRIPTOR_HANDLE dsvBase = mDsvHeap->GetCPUDescriptorHandleForHeapStart();
		mDsvHandles.resize(kTotalShadowFaces);
		for (int sliceIndex = 0; sliceIndex < kTotalShadowFaces; ++sliceIndex)
		{
			mDsvHandles[sliceIndex].ptr = dsvBase.ptr + static_cast<SIZE_T>(sliceIndex) * dsvSize;
			D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
			dsvDesc.Format = kPointShadowDepthFormat;
			dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
			dsvDesc.Texture2DArray.ArraySize = 1;
			dsvDesc.Texture2DArray.FirstArraySlice = sliceIndex;
			device->CreateDepthStencilView(mShadowDepthTexture.Get(), &dsvDesc, mDsvHandles[sliceIndex]);
		}

     if (mShadowSrvCpu.ptr == 0 || mShadowSrvGpu.ptr == 0)
		{
          if (!DX12Context_AllocateSrvDescriptor(&mShadowSrvCpu, &mShadowSrvGpu))
			{
				mLastError = "PointShadowMapRenderer: Failed to allocate SRV descriptor.";
				return false;
			}
		}

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = kPointShadowResourceFormat;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2DArray.ArraySize = kTotalShadowFaces;
		srvDesc.Texture2DArray.FirstArraySlice = 0;
		srvDesc.Texture2DArray.MipLevels = 1;
		device->CreateShaderResourceView(mShadowDepthTexture.Get(), &srvDesc, mShadowSrvCpu);

		return true;
	}

	bool CreatePipeline()
	{
		ID3D12Device* device = DX12Context_GetDevice();
		if (!device)
		{
			mLastError = "PointShadowMapRenderer: DX12 device is null.";
			return false;
		}

		const ShaderCompileRequest vsReq{ L"Shaders\\PointShadowDepth.hlsl", L"VSMain", L"vs_5_0", ShaderStage::Vertex };
		const ShaderCompileRequest psReq{ L"Shaders\\PointShadowDepth.hlsl", L"PSMain", L"ps_5_0", ShaderStage::Pixel };
		if (!mVertexShader.Compile(vsReq))
		{
			mLastError = std::string("PointShadowMapRenderer: VS compile failed: ") + (mVertexShader.GetLastErrorMessage() ? mVertexShader.GetLastErrorMessage() : "unknown");
			return false;
		}
		if (!mPixelShader.Compile(psReq))
		{
			mLastError = std::string("PointShadowMapRenderer: PS compile failed: ") + (mPixelShader.GetLastErrorMessage() ? mPixelShader.GetLastErrorMessage() : "unknown");
			return false;
		}

		D3D12_ROOT_PARAMETER rootParams[2]{};
		rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rootParams[0].Descriptor.ShaderRegister = 0;
		rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
		rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
		rootParams[1].Descriptor.ShaderRegister = 1;
		rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_ROOT_SIGNATURE_DESC rsDesc{};
		rsDesc.NumParameters = static_cast<UINT>(std::size(rootParams));
		rsDesc.pParameters = rootParams;
		rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

		Microsoft::WRL::ComPtr<ID3DBlob> serialized;
		Microsoft::WRL::ComPtr<ID3DBlob> errors;
		if (FAILED(D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)))
		{
			mLastError = "PointShadowMapRenderer: D3D12SerializeRootSignature failed.";
			return false;
		}
		if (FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&mRootSignature))))
		{
			mLastError = "PointShadowMapRenderer: CreateRootSignature failed.";
			return false;
		}

		const D3D12_INPUT_ELEMENT_DESC inputLayout[] =
		{
			{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			{ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
		};

		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = mRootSignature.Get();
		psoDesc.VS = mVertexShader.GetBytecode();
		psoDesc.PS = mPixelShader.GetBytecode();
		psoDesc.SampleMask = UINT_MAX;
		psoDesc.NumRenderTargets = 0;
		psoDesc.DSVFormat = kPointShadowDepthFormat;
		psoDesc.SampleDesc.Count = 1;
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
           psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
		psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
		psoDesc.RasterizerState.DepthBias = 0;
		psoDesc.RasterizerState.DepthBiasClamp = 0.0f;
        psoDesc.RasterizerState.SlopeScaledDepthBias = mSlopeScaledDepthBias;
		psoDesc.RasterizerState.DepthClipEnable = TRUE;
		psoDesc.RasterizerState.MultisampleEnable = FALSE;
		psoDesc.RasterizerState.AntialiasedLineEnable = FALSE;
		psoDesc.RasterizerState.ForcedSampleCount = 0;
		psoDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
		psoDesc.RasterizerState.DepthClipEnable = TRUE;
      psoDesc.DepthStencilState.DepthEnable = TRUE;
		psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
		psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		psoDesc.DepthStencilState.StencilEnable = FALSE;
		psoDesc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
		psoDesc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
		psoDesc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
		psoDesc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
		psoDesc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
		psoDesc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
		psoDesc.DepthStencilState.BackFace = psoDesc.DepthStencilState.FrontFace;
		psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
		psoDesc.InputLayout = { inputLayout, static_cast<UINT>(std::size(inputLayout)) };

		if (FAILED(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&mPipelineState))))
		{
			mLastError = "PointShadowMapRenderer: CreateGraphicsPipelineState failed.";
			return false;
		}

		return true;
	}

	void UpdateFaceMatrices()
	{
		mFaceViewProjections.resize(mLights.size() * kFacesPerLight);
		for (int lightIndex = 0; lightIndex < static_cast<int>(mLights.size()); ++lightIndex)
		{
			for (int faceIndex = 0; faceIndex < kFacesPerLight; ++faceIndex)
			{
				DirectX::XMFLOAT4X4 matrix;
				DirectX::XMStoreFloat4x4(&matrix, DirectX::XMMatrixTranspose(BuildPointLightViewProjection(mLights[lightIndex].Position, faceIndex, mLights[lightIndex].Radius)));
				mFaceViewProjections[lightIndex * kFacesPerLight + faceIndex] = matrix;
			}
		}
	}

	Microsoft::WRL::ComPtr<ID3D12Resource> mShadowDepthTexture;
	Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mDsvHeap;
	std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> mDsvHandles;
	D3D12_CPU_DESCRIPTOR_HANDLE mShadowSrvCpu{};
	D3D12_GPU_DESCRIPTOR_HANDLE mShadowSrvGpu{};
	DX12Shader mVertexShader;
	DX12Shader mPixelShader;
	Microsoft::WRL::ComPtr<ID3D12RootSignature> mRootSignature;
	Microsoft::WRL::ComPtr<ID3D12PipelineState> mPipelineState;
	std::vector<ShadowedPointLight> mLights;
	std::vector<DirectX::XMFLOAT4X4> mFaceViewProjections;
	bool mIsInitialized = false;
  UINT mMapSize = kDefaultPointShadowMapSize;
  float mShadowBias = 0.002f;
 float mSlopeScaledDepthBias = 2.0f;
	std::string mLastError;
};