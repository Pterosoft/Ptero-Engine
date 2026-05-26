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
