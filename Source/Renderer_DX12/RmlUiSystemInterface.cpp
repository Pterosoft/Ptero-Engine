#include "pch.h"

#include "RmlUiSystemInterface.h"

#include <string>

RmlUiSystemInterface::RmlUiSystemInterface() : mStartTime(std::chrono::steady_clock::now()) {}

double RmlUiSystemInterface::GetElapsedTime()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - mStartTime).count();
}

bool RmlUiSystemInterface::LogMessage(Rml::Log::Type type, const Rml::String& message)
{
    const char* prefix = "[RmlUi] ";
    switch (type)
    {
    case Rml::Log::LT_ERROR:   prefix = "[RmlUi][error] "; break;
    case Rml::Log::LT_ASSERT:  prefix = "[RmlUi][assert] "; break;
    case Rml::Log::LT_WARNING: prefix = "[RmlUi][warning] "; break;
    case Rml::Log::LT_INFO:    prefix = "[RmlUi][info] "; break;
    case Rml::Log::LT_DEBUG:   prefix = "[RmlUi][debug] "; break;
    default: break;
    }

    OutputDebugStringA((std::string(prefix) + message + "\n").c_str());

    // Returning true keeps going after an assert instead of breaking into the debugger,
    // which matters because document authoring errors are recoverable at runtime.
    return true;
}
