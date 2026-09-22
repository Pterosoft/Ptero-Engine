#pragma once

// Runtime access to the AMD FidelityFX / FSR API.
//
// The SDK ships signed DLLs only: a small loader plus one DLL per effect type.
// They are loaded with LoadLibrary rather than linked, which is what AMD
// recommends and what keeps the editor starting on a machine where the DLLs
// are missing - FSR then simply reports itself unavailable.

#include "FidelityFX-SDK-2.3.0/Kits/FidelityFX/api/include/ffx_api.h"
#include "FidelityFX-SDK-2.3.0/Kits/FidelityFX/api/include/ffx_api_loader.h"
#include "FidelityFX-SDK-2.3.0/Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h"

#include <d3d12.h>

namespace FfxLoader
{
    // Null when the loader DLL could not be found or does not export the API.
    // Loads on first call; later calls are free.
    const ffxFunctions* Get();

    // Why Get() returned null, for the settings panel and the log.
    const char* GetLastError();

    // Frees the DLLs. Every context must already have been destroyed.
    void Unload();

    // Routes FFX runtime messages into the session log.
    void __cdecl LogMessage(uint32_t type, const wchar_t* message);

    // The DX12 backend descriptor every effect context must chain.
    inline ffxCreateBackendDX12Desc MakeBackendDesc(ID3D12Device* device)
    {
        ffxCreateBackendDX12Desc desc{};
        desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
        desc.device = device;
        return desc;
    }
}
