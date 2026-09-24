#pragma once

// The C ABI a packaged game's exe exports so every engine DLL can read the mounted
// .ppak archives (see DataFiles.h). Plain C types only: the exe and the DLLs share no
// allocator or C++ runtime state across this boundary.
//
// Paths are Data-relative, forward-slash, UTF-8 ("Textures/Foo.dds"), matched
// case-insensitively.

extern "C"
{
    // Uncompressed size of a file, or -1 when there is no such file.
    typedef long long(__cdecl* PteroDataFileSizeFn)(const char* relativePath);

    // Decrypts one file into `buffer`, which must be exactly its FileSize bytes.
    typedef bool(__cdecl* PteroDataReadFileFn)(const char* relativePath, void* buffer, unsigned long long size);

    // True for "Textures", "UI/Farkle" and the like - any prefix of a stored path.
    typedef bool(__cdecl* PteroDataIsDirectoryFn)(const char* relativePath);

    // Calls `callback` once per file under `directory` ("" for everything).
    typedef void(__cdecl* PteroDataListCallback)(const char* relativePath, void* context);
    typedef void(__cdecl* PteroDataListFn)(const char* directory, bool recursive, PteroDataListCallback callback, void* context);
}

#define PTERO_DATA_FILE_SIZE_EXPORT "PteroData_FileSize"
#define PTERO_DATA_READ_FILE_EXPORT "PteroData_ReadFile"
#define PTERO_DATA_IS_DIRECTORY_EXPORT "PteroData_IsDirectory"
#define PTERO_DATA_LIST_EXPORT "PteroData_List"
