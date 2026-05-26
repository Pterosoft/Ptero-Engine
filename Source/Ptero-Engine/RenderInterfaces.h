#pragma once

#include <string>
#include <vector>

// Shared renderer-facing abstractions live in the editor project so the editor owns
// the contracts and the platform renderer DLL only provides the implementation.
enum class ShaderStage
{
    Vertex,
    Pixel,
    Compute
};

struct ShaderCompileRequest
{
    std::wstring FilePath;
    std::wstring EntryPoint;
    std::wstring TargetProfile;
    // Stage must remain the 4th field so existing positional aggregate initializers
    // { FilePath, EntryPoint, TargetProfile, Stage } continue to compile.
    ShaderStage Stage = ShaderStage::Vertex;
    std::vector<std::wstring> Defines;
    std::vector<std::wstring> IncludeDirectories;
};

class IShader
{
public:
    virtual ~IShader() = default;

    virtual const wchar_t* GetSourcePath() const = 0;
};

class IPipeline
{
public:
    virtual ~IPipeline() = default;

    virtual const wchar_t* GetDebugName() const = 0;
};
