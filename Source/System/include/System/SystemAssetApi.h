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

// Unreal Engine 5 .uasset import (static meshes, textures, materials).
extern "C" SYSTEM_ASSET_API bool __stdcall System_ImportUnrealAssetToData(
    const char* sourceUassetPath,
    const char* targetDirectoryRelativeToData,
    char* statusMessage,
    int statusMessageCapacity);

// Reads only the package header: the main asset's class, whether it can be imported, and
// whether an up-to-date converted file already sits next to it.
extern "C" SYSTEM_ASSET_API bool __stdcall System_GetUnrealAssetInfo(
    const char* uassetPath,
    char* assetClass,
    int assetClassCapacity,
    bool* importable,
    bool* alreadyImported);

// False (with the reason) when no Oodle decompressor was found on this machine.
extern "C" SYSTEM_ASSET_API bool __stdcall System_IsUnrealImportAvailable(
    char* statusMessage,
    int statusMessageCapacity);

extern "C" SYSTEM_ASSET_API bool __stdcall System_GenerateMeshLods(
    const char* geometryPath,
    char* statusMessage,
    int statusMessageCapacity);

extern "C" SYSTEM_ASSET_API bool __stdcall System_GenerateCollisions(
    const char* fbxOrPteroPath,
    char* statusMessage,
    int statusMessageCapacity);

// Cooks "<fbx>.ptero" (and its .pskin skin weights) again from an FBX already in Data,
// regenerating the collision hulls when asked (cooking writes the mesh and its LODs only).
// *skinned reports whether the FBX has a skinned mesh, i.e. whether a .pskin was written.
extern "C" SYSTEM_ASSET_API bool __stdcall System_RecookMesh(
    const char* fbxPath,
    bool regenerateCollisions,
    bool* skinned,
    char* statusMessage,
    int statusMessageCapacity);
