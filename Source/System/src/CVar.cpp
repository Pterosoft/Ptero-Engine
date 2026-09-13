#include "System/CVar.h"
#include "System/PteroLog.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <sstream>

namespace
{
    constexpr const char* kCategory = "Console";

    std::string ToLower(std::string text)
    {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    }

    // Registration order is arbitrary (it follows the call sites), so the
    // registry is kept sorted by lower-cased name: "list" then reads as an
    // index rather than a pile. unique_ptr keeps Var addresses stable as the
    // map grows, which is what lets Find hand out a long-lived pointer.
    std::map<std::string, std::unique_ptr<CVar::Var>>& Registry()
    {
        static std::map<std::string, std::unique_ptr<CVar::Var>> registry;
        return registry;
    }

    CVar::Var& Insert(const char* name, const char* description, CVar::Type type)
    {
        auto var = std::make_unique<CVar::Var>();
        var->Name = (name != nullptr) ? name : "";
        var->Description = (description != nullptr) ? description : "";
        var->ValueType = type;

        const std::string key = ToLower(var->Name);
        CVar::Var& stored = *var;
        auto [it, inserted] = Registry().insert_or_assign(key, std::move(var));
        if (!inserted)
            PTERO_LOG_WARNING(kCategory, "Duplicate cvar '%s' - the later registration wins.", stored.Name.c_str());
        return *it->second;
    }

    std::string FormatFloat(float value)
    {
        char buffer[64] = {};
        std::snprintf(buffer, sizeof(buffer), "%g", static_cast<double>(value));
        return buffer;
    }

    bool ParseBool(const std::string& text, bool& out)
    {
        const std::string lowered = ToLower(text);
        if (lowered == "1" || lowered == "true" || lowered == "on" || lowered == "yes")
        {
            out = true;
            return true;
        }
        if (lowered == "0" || lowered == "false" || lowered == "off" || lowered == "no")
        {
            out = false;
            return true;
        }
        return false;
    }

    bool ParseInt(const std::string& text, int& out)
    {
        char* end = nullptr;
        const long parsed = std::strtol(text.c_str(), &end, 0);
        if (end == text.c_str() || (end != nullptr && *end != '\0')) return false;
        out = static_cast<int>(parsed);
        return true;
    }

    bool ParseFloat(const std::string& text, float& out)
    {
        char* end = nullptr;
        const double parsed = std::strtod(text.c_str(), &end);
        if (end == text.c_str() || (end != nullptr && *end != '\0')) return false;
        out = static_cast<float>(parsed);
        return true;
    }

    std::string RangeText(const CVar::Var& var)
    {
        if (var.Minimum == var.Maximum) return {};
        char buffer[96] = {};
        if (var.ValueType == CVar::Type::Int)
            std::snprintf(buffer, sizeof(buffer), " [%d..%d]",
                          static_cast<int>(var.Minimum), static_cast<int>(var.Maximum));
        else
            std::snprintf(buffer, sizeof(buffer), " [%g..%g]", var.Minimum, var.Maximum);
        return buffer;
    }

    void LogVar(const CVar::Var& var)
    {
        std::string line = var.Name + " = " + CVar::ValueText(var);
        line += RangeText(var);
        if (!var.DefaultText.empty() && var.DefaultText != CVar::ValueText(var))
            line += "  (default " + var.DefaultText + ")";
        PTERO_LOG_INFO(kCategory, "%s", line.c_str());
        if (!var.Description.empty())
            PTERO_LOG_INFO(kCategory, "    %s", var.Description.c_str());
    }
}

namespace CVar
{
    void RegisterBool(const char* name, bool* value, const char* description, std::function<void()> onChanged)
    {
        if (value == nullptr) return;
        Var& var = Insert(name, description, Type::Bool);
        var.BoolValue = value;
        var.OnChanged = std::move(onChanged);
        var.DefaultText = *value ? "true" : "false";
    }

    void RegisterInt(const char* name, int* value, const char* description,
                     int minimum, int maximum, std::function<void()> onChanged)
    {
        if (value == nullptr) return;
        Var& var = Insert(name, description, Type::Int);
        var.IntValue = value;
        var.Minimum = minimum;
        var.Maximum = maximum;
        var.OnChanged = std::move(onChanged);
        var.DefaultText = std::to_string(*value);
    }

    void RegisterFloat(const char* name, float* value, const char* description,
                       float minimum, float maximum, std::function<void()> onChanged)
    {
        if (value == nullptr) return;
        Var& var = Insert(name, description, Type::Float);
        var.FloatValue = value;
        var.Minimum = minimum;
        var.Maximum = maximum;
        var.OnChanged = std::move(onChanged);
        var.DefaultText = FormatFloat(*value);
    }

    void RegisterEnum(const char* name, int* value, const char* description,
                      std::vector<std::string> labels, int firstValue, std::function<void()> onChanged)
    {
        if (value == nullptr) return;
        Var& var = Insert(name, description, Type::Enum);
        var.IntValue = value;
        var.EnumLabels = std::move(labels);
        var.EnumFirstValue = firstValue;
        var.Minimum = firstValue;
        var.Maximum = firstValue + static_cast<int>(var.EnumLabels.size()) - 1;
        var.OnChanged = std::move(onChanged);
        var.DefaultText = ValueText(var);
    }

    void RegisterString(const char* name, std::string* value, const char* description, std::function<void()> onChanged)
    {
        if (value == nullptr) return;
        Var& var = Insert(name, description, Type::String);
        var.StringValue = value;
        var.OnChanged = std::move(onChanged);
        var.DefaultText = *value;
    }

    const Var* Find(const char* name)
    {
        if (name == nullptr) return nullptr;
        auto& registry = Registry();
        const auto it = registry.find(ToLower(name));
        return (it != registry.end()) ? it->second.get() : nullptr;
    }

    std::vector<const Var*> Search(const std::string& substring)
    {
        const std::string needle = ToLower(substring);
        std::vector<const Var*> matches;
        for (const auto& [key, var] : Registry())
        {
            if (needle.empty()
                || key.find(needle) != std::string::npos
                || ToLower(var->Description).find(needle) != std::string::npos)
            {
                matches.push_back(var.get());
            }
        }
        return matches;
    }

    std::size_t Count()
    {
        return Registry().size();
    }

    std::string ValueText(const Var& var)
    {
        switch (var.ValueType)
        {
        case Type::Bool:
            return (var.BoolValue != nullptr && *var.BoolValue) ? "true" : "false";
        case Type::Int:
            return (var.IntValue != nullptr) ? std::to_string(*var.IntValue) : "0";
        case Type::Float:
            return (var.FloatValue != nullptr) ? FormatFloat(*var.FloatValue) : "0";
        case Type::Enum:
        {
            if (var.IntValue == nullptr) return "0";
            const int index = *var.IntValue - var.EnumFirstValue;
            if (index >= 0 && index < static_cast<int>(var.EnumLabels.size()))
                return var.EnumLabels[static_cast<std::size_t>(index)];
            return std::to_string(*var.IntValue);
        }
        case Type::String:
            return (var.StringValue != nullptr) ? *var.StringValue : std::string();
        }
        return {};
    }

    bool SetValue(const Var& var, const std::string& text, std::string& outError)
    {
        switch (var.ValueType)
        {
        case Type::Bool:
        {
            bool parsed = false;
            if (!ParseBool(text, parsed))
            {
                outError = "expected true/false (or 1/0, on/off).";
                return false;
            }
            *var.BoolValue = parsed;
            break;
        }
        case Type::Int:
        {
            int parsed = 0;
            if (!ParseInt(text, parsed))
            {
                outError = "expected a whole number.";
                return false;
            }
            if (var.Minimum != var.Maximum
                && (parsed < static_cast<int>(var.Minimum) || parsed > static_cast<int>(var.Maximum)))
            {
                outError = "out of range" + RangeText(var) + ".";
                return false;
            }
            *var.IntValue = parsed;
            break;
        }
        case Type::Float:
        {
            float parsed = 0.0f;
            if (!ParseFloat(text, parsed))
            {
                outError = "expected a number.";
                return false;
            }
            if (var.Minimum != var.Maximum
                && (static_cast<double>(parsed) < var.Minimum || static_cast<double>(parsed) > var.Maximum))
            {
                outError = "out of range" + RangeText(var) + ".";
                return false;
            }
            *var.FloatValue = parsed;
            break;
        }
        case Type::Enum:
        {
            // Accept the label or the raw number, so scripts and people can
            // both drive it.
            const std::string lowered = ToLower(text);
            for (std::size_t i = 0; i < var.EnumLabels.size(); ++i)
            {
                if (ToLower(var.EnumLabels[i]) == lowered)
                {
                    *var.IntValue = var.EnumFirstValue + static_cast<int>(i);
                    if (var.OnChanged) var.OnChanged();
                    return true;
                }
            }

            int parsed = 0;
            if (!ParseInt(text, parsed))
            {
                std::string joined;
                for (const std::string& label : var.EnumLabels)
                {
                    if (!joined.empty()) joined += ", ";
                    joined += label;
                }
                outError = "expected one of: " + joined + ".";
                return false;
            }
            if (parsed < static_cast<int>(var.Minimum) || parsed > static_cast<int>(var.Maximum))
            {
                outError = "out of range" + RangeText(var) + ".";
                return false;
            }
            *var.IntValue = parsed;
            break;
        }
        case Type::String:
            *var.StringValue = text;
            break;
        }

        if (var.OnChanged) var.OnChanged();
        return true;
    }

    std::string DumpAll()
    {
        std::ostringstream out;
        for (const auto& [key, var] : Registry())
            out << var->Name << ' ' << ValueText(*var) << '\n';
        return out.str();
    }

    const std::vector<Command>& Commands()
    {
        // Usage strings are what the console's type-ahead inserts, so each is
        // the bare command word; the argument shape lives in Help beside it.
        static const std::vector<Command> commands = {
            { "help",      "help [cvar]",      "list these commands, or describe one cvar" },
            { "list",      "list [text]",      "list cvars whose name or description contains text" },
            { "find",      "find <text>",      "same as list" },
            { "reset",     "reset <cvar>|all", "restore the startup default" },
            { "dumpcvars", "dumpcvars",        "print every cvar and its current value" },
            { "cvarlist",  "cvarlist",         "same as dumpcvars" },
            { "log",       "log <level>",      "trace | debug | info | warning | error | fatal" },
            { "logpath",   "logpath",          "where this session's .txt is being written" },
            { "clear",     "clear",            "empty the console; the log file keeps everything" },
            { "echo",      "echo <text>",      "print text" },
        };
        return commands;
    }

    void Execute(const std::string& commandLine)
    {
        // Trim.
        const std::size_t first = commandLine.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return;
        const std::size_t last = commandLine.find_last_not_of(" \t\r\n");
        const std::string line = commandLine.substr(first, last - first + 1);

        PTERO_LOG_INFO(kCategory, "> %s", line.c_str());

        // Split into the first token and the rest, so string cvars and `echo`
        // keep their spaces.
        const std::size_t space = line.find_first_of(" \t");
        const std::string command = (space == std::string::npos) ? line : line.substr(0, space);
        std::string argument;
        if (space != std::string::npos)
        {
            const std::size_t argStart = line.find_first_not_of(" \t", space);
            if (argStart != std::string::npos) argument = line.substr(argStart);
        }

        const std::string lowered = ToLower(command);

        if (lowered == "help" && argument.empty())
        {
            PTERO_LOG_INFO(kCategory, "Commands:");
            PTERO_LOG_INFO(kCategory, "  <cvar>              show a setting's value, range and default");
            PTERO_LOG_INFO(kCategory, "  <cvar> <value>      change it");
            for (const Command& builtin : Commands())
                PTERO_LOG_INFO(kCategory, "  %-18s  %s", builtin.Usage, builtin.Help);
            PTERO_LOG_INFO(kCategory, "%llu cvars registered. Start typing a name for the list.",
                           static_cast<unsigned long long>(Count()));
            return;
        }

        if (lowered == "list" || lowered == "find")
        {
            const std::vector<const Var*> matches = Search(argument);
            if (matches.empty())
            {
                PTERO_LOG_WARNING(kCategory, "No cvar matches '%s'.", argument.c_str());
                return;
            }
            for (const Var* var : matches)
                PTERO_LOG_INFO(kCategory, "  %-44s %s", var->Name.c_str(), ValueText(*var).c_str());
            PTERO_LOG_INFO(kCategory, "%llu match%s.",
                           static_cast<unsigned long long>(matches.size()),
                           matches.size() == 1 ? "" : "es");
            return;
        }

        if (lowered == "help")
        {
            const Var* var = Find(argument.c_str());
            if (var == nullptr)
            {
                PTERO_LOG_WARNING(kCategory, "Unknown cvar '%s'.", argument.c_str());
                return;
            }
            LogVar(*var);
            if (var->ValueType == Type::Enum)
            {
                std::string joined;
                for (const std::string& label : var->EnumLabels)
                {
                    if (!joined.empty()) joined += ", ";
                    joined += label;
                }
                PTERO_LOG_INFO(kCategory, "    values: %s", joined.c_str());
            }
            return;
        }

        if (lowered == "reset")
        {
            if (ToLower(argument) == "all")
            {
                std::size_t restored = 0;
                std::string error;
                for (const auto& [key, var] : Registry())
                    if (SetValue(*var, var->DefaultText, error)) ++restored;
                PTERO_LOG_INFO(kCategory, "Restored %llu cvars to their startup defaults.",
                               static_cast<unsigned long long>(restored));
                return;
            }

            const Var* var = Find(argument.c_str());
            if (var == nullptr)
            {
                PTERO_LOG_WARNING(kCategory, "Unknown cvar '%s'.", argument.c_str());
                return;
            }
            std::string error;
            if (SetValue(*var, var->DefaultText, error))
                PTERO_LOG_INFO(kCategory, "%s = %s", var->Name.c_str(), ValueText(*var).c_str());
            else
                PTERO_LOG_ERROR(kCategory, "%s: %s", var->Name.c_str(), error.c_str());
            return;
        }

        if (lowered == "dumpcvars" || lowered == "cvarlist")
        {
            for (const auto& [key, var] : Registry())
                PTERO_LOG_INFO(kCategory, "  %-44s %s", var->Name.c_str(), ValueText(*var).c_str());
            return;
        }

        if (lowered == "clear")
        {
            PteroLog::Clear();
            return;
        }

        if (lowered == "echo")
        {
            PTERO_LOG_INFO(kCategory, "%s", argument.c_str());
            return;
        }

        if (lowered == "logpath")
        {
            const std::string path = PteroLog::SessionFilePathUtf8();
            PTERO_LOG_INFO(kCategory, "%s", path.empty() ? "(no log file open)" : path.c_str());
            return;
        }

        if (lowered == "log")
        {
            const std::string level = ToLower(argument);
            if (level == "trace")        PteroLog::SetMinimumLevel(PteroLog::Level::Trace);
            else if (level == "debug")   PteroLog::SetMinimumLevel(PteroLog::Level::Debug);
            else if (level == "info")    PteroLog::SetMinimumLevel(PteroLog::Level::Info);
            else if (level == "warning" || level == "warn") PteroLog::SetMinimumLevel(PteroLog::Level::Warning);
            else if (level == "error")   PteroLog::SetMinimumLevel(PteroLog::Level::Error);
            else if (level == "fatal")   PteroLog::SetMinimumLevel(PteroLog::Level::Fatal);
            else
            {
                PTERO_LOG_WARNING(kCategory, "Minimum level is %s. Use trace|debug|info|warning|error|fatal.",
                                  PteroLog::LevelName(PteroLog::MinimumLevel()));
                return;
            }
            PTERO_LOG_INFO(kCategory, "Minimum log level is now %s.",
                           PteroLog::LevelName(PteroLog::MinimumLevel()));
            return;
        }

        const Var* var = Find(command.c_str());
        if (var == nullptr)
        {
            PTERO_LOG_WARNING(kCategory, "Unknown command or cvar '%s'. Type 'help'.", command.c_str());

            // A near miss is almost always a half-remembered name, so offer the
            // handful that contain what was typed.
            const std::vector<const Var*> similar = Search(command);
            for (std::size_t i = 0; i < similar.size() && i < 8; ++i)
                PTERO_LOG_INFO(kCategory, "  did you mean %s?", similar[i]->Name.c_str());
            return;
        }

        if (argument.empty())
        {
            LogVar(*var);
            return;
        }

        std::string error;
        if (SetValue(*var, argument, error))
            PTERO_LOG_INFO(kCategory, "%s = %s", var->Name.c_str(), ValueText(*var).c_str());
        else
            PTERO_LOG_ERROR(kCategory, "%s: %s", var->Name.c_str(), error.c_str());
    }
}
