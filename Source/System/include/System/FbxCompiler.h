#pragma once

#include <string>

class FbxCompiler final
{
public:
    FbxCompiler() = delete;

    static bool CompileFbxToPtero(const std::string& fbxPath, const std::string& pteroOutPath);
    static bool GenerateLodsForPtero(const std::string& pteroPath);
};
