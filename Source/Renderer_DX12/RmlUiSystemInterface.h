#pragma once

#include <RmlUi/Core/SystemInterface.h>

#include <chrono>

// Rml::SystemInterface implemented on top of the platform facilities the engine already
// uses. RmlUi needs a monotonic clock to drive animations and transitions, and a log sink;
// everything else on the interface has a usable default.
class RmlUiSystemInterface final : public Rml::SystemInterface
{
public:
    RmlUiSystemInterface();

    double GetElapsedTime() override;
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;

private:
    std::chrono::steady_clock::time_point mStartTime;
};
