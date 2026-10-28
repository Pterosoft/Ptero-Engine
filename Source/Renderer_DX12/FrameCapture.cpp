#include "pch.h"
#include "FrameCapture.h"

#include "d3dx12.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <cstring>

using Microsoft::WRL::ComPtr;

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    ID3D12CommandQueue* __stdcall DX12Context_GetCommandQueue();
}

namespace
{
    float HalfToFloat(std::uint16_t value)
    {
        const std::uint32_t sign = (static_cast<std::uint32_t>(value & 0x8000u)) << 16;
        std::uint32_t exponent = (value >> 10) & 0x1Fu;
        std::uint32_t mantissa = value & 0x03FFu;

        std::uint32_t bits = 0;
        if (exponent == 0)
        {
            if (mantissa == 0)
            {
                bits = sign;
            }
            else
            {
                exponent = 1;
                while ((mantissa & 0x0400u) == 0)
                {
                    mantissa <<= 1;
                    --exponent;
                }

                mantissa &= 0x03FFu;
                bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
            }
        }
        else if (exponent == 0x1Fu)
        {
            bits = sign | 0x7F800000u | (mantissa << 13);
        }
        else
        {
            bits = sign | ((exponent + (127 - 15)) << 23) | (mantissa << 13);
        }

        float result = 0.0f;
        std::memcpy(&result, &bits, sizeof(result));
        return result;
    }

    BYTE FloatToByte(float value)
    {
        const float clamped = (value < 0.0f) ? 0.0f : ((value > 1.0f) ? 1.0f : value);
        return static_cast<BYTE>((clamped * 255.0f) + 0.5f);
    }

    // CoInitializeEx for the scope, undone only if this call did it.
    struct ComScope
    {
        ComScope() : Initialized(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}
        ~ComScope()
        {
            if (Initialized)
                CoUninitialize();
        }
        bool Initialized;
    };
}

namespace FrameCapture
{
    bool ReadbackBgra(ID3D12Resource* texture, std::vector<std::uint8_t>& pixels, std::uint32_t& width,
                      std::uint32_t& height)
    {
        ID3D12Device* device = DX12Context_GetDevice();
        ID3D12CommandQueue* queue = DX12Context_GetCommandQueue();
        if (texture == nullptr || device == nullptr || queue == nullptr)
            return false;

        const D3D12_RESOURCE_DESC desc = texture->GetDesc();
        width = static_cast<std::uint32_t>(desc.Width);
        height = static_cast<std::uint32_t>(desc.Height);
        const DXGI_FORMAT format = desc.Format;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rowCount = 0;
        UINT64 rowSize = 0;
        UINT64 totalBytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rowCount, &rowSize, &totalBytes);

        ComPtr<ID3D12Resource> readback;
        const auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_READBACK);
        const auto bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(totalBytes);
        if (FAILED(device->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))))
            return false;

        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commandList;
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                             IID_PPV_ARGS(&commandList))))
            return false;

        const auto toSource = CD3DX12_RESOURCE_BARRIER::Transition(texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                                                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        commandList->ResourceBarrier(1, &toSource);

        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = texture;
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = readback.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = footprint;
        commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);

        const auto toShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
            texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        commandList->ResourceBarrier(1, &toShaderResource);
        commandList->Close();

        ID3D12CommandList* lists[] = { commandList.Get() };
        queue->ExecuteCommandLists(1, lists);

        // A fence and event of its own: the context's shared fence event must not be
        // waited on from here (see the context's notes on shared fence events).
        ComPtr<ID3D12Fence> fence;
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return false;
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event == nullptr)
            return false;
        queue->Signal(fence.Get(), 1);
        fence->SetEventOnCompletion(1, event);
        WaitForSingleObject(event, INFINITE);
        CloseHandle(event);

        void* mapped = nullptr;
        const D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(totalBytes) };
        if (FAILED(readback->Map(0, &readRange, &mapped)) || mapped == nullptr)
            return false;

        const BYTE* bytes = static_cast<const BYTE*>(mapped);
        const std::size_t stride = static_cast<std::size_t>(width) * 4;
        pixels.resize(stride * height);
        bool supported = true;
        for (std::uint32_t y = 0; y < height && supported; ++y)
        {
            const BYTE* row = bytes + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch;
            BYTE* out = pixels.data() + static_cast<std::size_t>(y) * stride;
            switch (format)
            {
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            {
                const auto* half = reinterpret_cast<const std::uint16_t*>(row);
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    out[x * 4 + 0] = FloatToByte(HalfToFloat(half[x * 4 + 2]));
                    out[x * 4 + 1] = FloatToByte(HalfToFloat(half[x * 4 + 1]));
                    out[x * 4 + 2] = FloatToByte(HalfToFloat(half[x * 4 + 0]));
                    out[x * 4 + 3] = FloatToByte(HalfToFloat(half[x * 4 + 3]));
                }
                break;
            }
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
                for (std::uint32_t x = 0; x < width; ++x)
                {
                    out[x * 4 + 0] = row[x * 4 + 2];
                    out[x * 4 + 1] = row[x * 4 + 1];
                    out[x * 4 + 2] = row[x * 4 + 0];
                    out[x * 4 + 3] = row[x * 4 + 3];
                }
                break;
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
                std::memcpy(out, row, stride);
                break;
            default:
                supported = false;
                break;
            }
        }
        readback->Unmap(0, nullptr);
        if (!supported)
            OutputDebugStringA("FrameCapture: unsupported texture format.\n");
        return supported;
    }

    bool WriteImage(const std::string& path, const std::vector<std::uint8_t>& bgraPixels, std::uint32_t width,
                    std::uint32_t height, ImageFormat format, int quality)
    {
        if (path.empty() || bgraPixels.size() < static_cast<std::size_t>(width) * height * 4)
            return false;

        const ComScope com;
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> options;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
            FAILED(factory->CreateStream(&stream)))
            return false;

        const int wideLength = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring widePath(static_cast<std::size_t>((std::max)(wideLength, 1)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, widePath.data(), wideLength);
        if (FAILED(stream->InitializeFromFilename(widePath.c_str(), GENERIC_WRITE)))
            return false;

        const GUID container = format == ImageFormat::Jpeg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng;
        if (FAILED(factory->CreateEncoder(container, nullptr, &encoder)) ||
            FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
            FAILED(encoder->CreateNewFrame(&frame, &options)))
            return false;

        if (format == ImageFormat::Jpeg && options)
        {
            PROPBAG2 option{};
            option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            VARIANT value;
            VariantInit(&value);
            value.vt = VT_R4;
            value.fltVal = static_cast<float>((std::max)(1, (std::min)(quality, 100))) / 100.0f;
            options->Write(1, &option, &value);
        }

        if (FAILED(frame->Initialize(options.Get())) || FAILED(frame->SetSize(width, height)))
            return false;

        // JPEG has no alpha: hand the encoder 24-bit BGR.
        WICPixelFormatGUID pixelFormat = format == ImageFormat::Jpeg ? GUID_WICPixelFormat24bppBGR : GUID_WICPixelFormat32bppBGRA;
        if (FAILED(frame->SetPixelFormat(&pixelFormat)))
            return false;

        HRESULT result = S_OK;
        if (format == ImageFormat::Jpeg)
        {
            const std::size_t stride = static_cast<std::size_t>(width) * 3;
            std::vector<std::uint8_t> bgr(stride * height);
            for (std::size_t i = 0, count = static_cast<std::size_t>(width) * height; i < count; ++i)
            {
                bgr[i * 3 + 0] = bgraPixels[i * 4 + 0];
                bgr[i * 3 + 1] = bgraPixels[i * 4 + 1];
                bgr[i * 3 + 2] = bgraPixels[i * 4 + 2];
            }
            result = frame->WritePixels(height, static_cast<UINT>(stride), static_cast<UINT>(bgr.size()), bgr.data());
        }
        else
        {
            result = frame->WritePixels(height, width * 4, static_cast<UINT>(bgraPixels.size()),
                                        const_cast<BYTE*>(bgraPixels.data()));
        }
        return SUCCEEDED(result) && SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
    }

    bool SaveTexture(ID3D12Resource* texture, const std::string& path, ImageFormat format, int quality)
    {
        std::vector<std::uint8_t> pixels;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        return ReadbackBgra(texture, pixels, width, height) && WriteImage(path, pixels, width, height, format, quality);
    }
}
