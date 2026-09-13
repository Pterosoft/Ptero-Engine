#pragma once

#include "DX12Helper.h"
#include "..\Ptero-Engine\RenderInterfaces.h"

#include <cstdint>
#include <string>
#include <vector>

class DX12Shader final : public IShader
{
public:
    DX12Shader() = default;

    bool Compile(const ShaderCompileRequest& request);

    const wchar_t* GetSourcePath() const override
    {
        return mSourcePath.c_str();
    }

    const char* GetLastErrorMessage() const
    {
        return mLastErrorMessage.empty() ? nullptr : mLastErrorMessage.c_str();
    }

    bool WasLoadedFromCache() const
    {
        return mLoadedFromCache;
    }

    D3D12_SHADER_BYTECODE GetBytecode() const
    {
        D3D12_SHADER_BYTECODE bytecode{};
        bytecode.pShaderBytecode = mBytecode.empty() ? nullptr : mBytecode.data();
        bytecode.BytecodeLength = mBytecode.size();
        return bytecode;
    }

private:
    std::wstring mSourcePath;
    std::vector<std::uint8_t> mBytecode;
    std::string mLastErrorMessage;
    bool mLoadedFromCache = false;
};
