#pragma once

// ---------------------------------------------------------------------------
// CVar - a name for every engine setting, bound to the live variable.
//
// The editor's panels and this registry write the same memory: a cvar holds a
// pointer into the settings struct the renderer already reads each frame, so
// setting one from the Console is indistinguishable from moving the slider,
// and no synchronisation step exists to get out of date. That is the whole
// design. It also means a cvar must never outlive what it points at, which is
// why registration happens once, against the long-lived scene renderer, and
// nothing is ever unregistered.
//
// Names are dotted and lower-case by convention - "rtgi.specular.enabled",
// "bloom.intensity" - and matched case-insensitively.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace CVar
{
    enum class Type
    {
        Bool,
        Int,
        Float,
        Enum,     // an int with names for its values
        String
    };

    struct Var
    {
        std::string Name;
        std::string Description;
        Type        ValueType = Type::Bool;

        // Exactly one of these is set, matching ValueType. Enum uses IntValue.
        bool*        BoolValue = nullptr;
        int*         IntValue = nullptr;
        float*       FloatValue = nullptr;
        std::string* StringValue = nullptr;

        // Inclusive. Ignored when Minimum == Maximum.
        double Minimum = 0.0;
        double Maximum = 0.0;

        // Value at registration time, so "reset" means "back to how the engine
        // starts" rather than "back to whatever the level file happened to say".
        std::string DefaultText;

        // Display names for Enum, indexed from EnumFirstValue.
        std::vector<std::string> EnumLabels;
        int EnumFirstValue = 0;

        // Called after a successful write. Most settings are polled by the
        // renderer every frame and need nothing here; the exceptions are the
        // ones that own GPU resources.
        std::function<void()> OnChanged;
    };

    void RegisterBool(const char* name, bool* value, const char* description,
                      std::function<void()> onChanged = {});
    void RegisterInt(const char* name, int* value, const char* description,
                     int minimum = 0, int maximum = 0, std::function<void()> onChanged = {});
    void RegisterFloat(const char* name, float* value, const char* description,
                       float minimum = 0.0f, float maximum = 0.0f, std::function<void()> onChanged = {});
    void RegisterEnum(const char* name, int* value, const char* description,
                      std::vector<std::string> labels, int firstValue = 0,
                      std::function<void()> onChanged = {});
    void RegisterString(const char* name, std::string* value, const char* description,
                        std::function<void()> onChanged = {});

    // Null when unknown. The pointer stays valid for the process lifetime.
    const Var* Find(const char* name);

    // All cvars whose name or description contains `substring`, name-sorted.
    // An empty substring returns everything.
    std::vector<const Var*> Search(const std::string& substring);

    std::size_t Count();

    // Current value rendered the way Set accepts it back.
    std::string ValueText(const Var& var);

    // Applies `text` to `var`. Returns false and fills `outError` on a parse
    // failure or an out-of-range value; a value outside [Minimum, Maximum] is
    // rejected rather than clamped, because silently clamping hides typos.
    bool SetValue(const Var& var, const std::string& text, std::string& outError);

    // Parses and runs one Console line: "name", "name value", or one of the
    // built-in commands (help, list, find, reset, clear, log, logpath, echo,
    // cvarlist, dumpcvars). Output goes to the log under the "Console"
    // category, which is how the Console panel shows it.
    void Execute(const std::string& commandLine);

    // Every cvar as "name value" lines, for dumping to disk or the log.
    std::string DumpAll();

    // A built-in console command - everything Execute understands that is not a
    // cvar. Held as data so the help text and the console's type-ahead list are
    // the same list and cannot drift apart.
    struct Command
    {
        const char* Name;
        const char* Usage;
        const char* Help;
    };
    const std::vector<Command>& Commands();
}
