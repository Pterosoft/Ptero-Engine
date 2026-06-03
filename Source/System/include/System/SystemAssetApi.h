#pragma once

#if defined(SYSTEM_EXPORTS)
#define SYSTEM_ASSET_API __declspec(dllexport)
#else
#define SYSTEM_ASSET_API __declspec(dllimport)
#endif

extern "C" SYSTEM_ASSET_API bool __stdcall System_TryLoadMeshFromFile(
    const char* fbxFilePath,
    char* statusMessage,
    int statusMessageCapacity);

extern "C" SYSTEM_ASSET_API bool __stdcall System_ImportFbxToData(
    const char* sourceFbxPath,
    const char* targetDirectoryRelativeToData,
    char* statusMessage,
    int statusMessageCapacity);

extern "C" SYSTEM_ASSET_API bool __stdcall System_ImportTextureToData(
    const char* sourceTexturePath,
    const char* targetDirectoryRelativeToData,
    char* statusMessage,
    int statusMessageCapacity);

extern "C" SYSTEM_ASSET_API bool __stdcall System_GenerateMeshLods(
    const char* geometryPath,
    char* statusMessage,
    int statusMessageCapacity);
