#pragma once

#include <stdexcept>
#include <string>
#include <wrl/client.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include "d3dx12.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

#ifndef DX12_THROW_IF_FAILED
#define DX12_THROW_IF_FAILED(hr)                                                                 \
    do                                                                                            \
    {                                                                                             \
        HRESULT _hr = (hr);                                                                       \
        if (FAILED(_hr))                                                                          \
        {                                                                                         \
            throw std::runtime_error("DirectX 12 call failed. HRESULT: " + std::to_string(_hr));\
        }                                                                                         \
    } while (0)
#endif

// The format a shader resource view of a texture created with `format` should use:
// typeless and depth formats have no view of their own.
inline DXGI_FORMAT DX12ShaderReadableFormat(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:  return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:     return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:     return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:             return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:     return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:             return DXGI_FORMAT_R16_UNORM;
    default:                                return format;
    }
}
