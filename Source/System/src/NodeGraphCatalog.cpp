#include "System/NodeGraphCatalog.h"

#include <cstring>

namespace
{
    constexpr NodePinKind Exec = NodePinKind::Exec;
    constexpr NodePinKind Bool = NodePinKind::Bool;
    constexpr NodePinKind Num = NodePinKind::Number;
    constexpr NodePinKind Str = NodePinKind::String;
    constexpr NodePinKind Any = NodePinKind::Any;

    using Pin = NodeGraphPin;
    using Param = NodeGraphParam;

    // ---- Events -----------------------------------------------------------------

    constexpr Pin kEventOut[] = { { "Then", Exec } };
    constexpr Pin kTickOut[] = { { "Then", Exec }, { "Delta Seconds", Num } };

    // ---- Flow -------------------------------------------------------------------

    constexpr Pin kBranchIn[] = { { "In", Exec }, { "Condition", Bool } };
    constexpr Pin kBranchOut[] = { { "True", Exec }, { "False", Exec } };
    constexpr Param kBranchParams[] = { { "Condition", "Condition", NodeParamKind::Bool, "false", nullptr } };

    constexpr Pin kDelayIn[] = { { "In", Exec }, { "Duration", Num } };
    constexpr Pin kDelayOut[] = { { "Completed", Exec } };
    constexpr Param kDelayParams[] = { { "Duration", "Seconds", NodeParamKind::Number, "1", nullptr } };

    constexpr Pin kGateIn[] = { { "Enter", Exec }, { "Open", Exec }, { "Close", Exec }, { "Toggle", Exec } };
    constexpr Pin kGateOut[] = { { "Exit", Exec } };
    constexpr Param kGateParams[] = { { "StartClosed", "Start closed", NodeParamKind::Bool, "true", nullptr } };

    constexpr Pin kExecIn[] = { { "In", Exec } };
    constexpr Pin kExecOut[] = { { "Then", Exec } };

    constexpr Pin kSequenceOut[] = {
        { "Then 0", Exec }, { "Then 1", Exec }, { "Then 2", Exec }, { "Then 3", Exec }
    };

    constexpr Pin kDoOnceIn[] = { { "In", Exec }, { "Reset", Exec } };
    constexpr Pin kDoOnceOut[] = { { "Completed", Exec } };
    constexpr Param kDoOnceParams[] = { { "StartClosed", "Start closed", NodeParamKind::Bool, "false", nullptr } };

    constexpr Pin kDoNIn[] = { { "In", Exec }, { "Reset", Exec }, { "N", Num } };
    constexpr Pin kDoNOut[] = { { "Exit", Exec }, { "Counter", Num } };
    constexpr Param kDoNParams[] = { { "N", "N", NodeParamKind::Number, "1", nullptr } };

    constexpr Pin kFlipFlopOut[] = { { "A", Exec }, { "B", Exec }, { "Is A", Bool } };

    constexpr Pin kForLoopIn[] = { { "In", Exec }, { "First", Num }, { "Last", Num } };
    constexpr Pin kForLoopOut[] = { { "Loop Body", Exec }, { "Index", Num }, { "Completed", Exec } };
    constexpr Param kForLoopParams[] = {
        { "First", "First", NodeParamKind::Number, "0", nullptr },
        { "Last", "Last", NodeParamKind::Number, "4", nullptr }
    };

    constexpr Pin kSwitchIn[] = { { "In", Exec }, { "Index", Num } };
    constexpr Pin kSwitchOut[] = {
        { "Case 0", Exec }, { "Case 1", Exec }, { "Case 2", Exec }, { "Case 3", Exec }, { "Default", Exec }
    };
    constexpr Param kSwitchParams[] = { { "Index", "Index", NodeParamKind::Number, "0", nullptr } };

    // ---- Logic ------------------------------------------------------------------

    constexpr Pin kMultiplexerIn[] = {
        { "Index", Num }, { "In 0", Any }, { "In 1", Any }, { "In 2", Any }, { "In 3", Any }
    };
    constexpr Pin kMultiplexerOut[] = { { "Out", Any } };
    constexpr Param kMultiplexerParams[] = { { "Index", "Index", NodeParamKind::Number, "0", nullptr } };

    constexpr Pin kBoolPairIn[] = { { "A", Bool }, { "B", Bool } };
    constexpr Pin kBoolOut[] = { { "Result", Bool } };
    constexpr Pin kNotIn[] = { { "In", Bool } };

    constexpr Pin kComparePairIn[] = { { "A", Num }, { "B", Num } };
    constexpr Param kCompareParams[] = {
        { "Operator", "Operator", NodeParamKind::Enum, "==", "==|!=|>|>=|<|<=" },
        { "A", "A", NodeParamKind::Number, "0", nullptr },
        { "B", "B", NodeParamKind::Number, "0", nullptr }
    };

    // ---- Math -------------------------------------------------------------------

    constexpr Pin kNumPairIn[] = { { "A", Num }, { "B", Num } };
    constexpr Pin kNumOut[] = { { "Result", Num } };
    constexpr Param kNumPairParams[] = {
        { "A", "A", NodeParamKind::Number, "0", nullptr },
        { "B", "B", NodeParamKind::Number, "0", nullptr }
    };

    constexpr Pin kClampIn[] = { { "Value", Num }, { "Min", Num }, { "Max", Num } };
    constexpr Param kClampParams[] = {
        { "Value", "Value", NodeParamKind::Number, "0", nullptr },
        { "Min", "Min", NodeParamKind::Number, "0", nullptr },
        { "Max", "Max", NodeParamKind::Number, "1", nullptr }
    };

    constexpr Pin kLerpIn[] = { { "A", Num }, { "B", Num }, { "Alpha", Num } };
    constexpr Param kLerpParams[] = {
        { "A", "A", NodeParamKind::Number, "0", nullptr },
        { "B", "B", NodeParamKind::Number, "1", nullptr },
        { "Alpha", "Alpha", NodeParamKind::Number, "0.5", nullptr }
    };

    constexpr Pin kRandomIn[] = { { "Min", Num }, { "Max", Num } };
    constexpr Param kRandomParams[] = {
        { "Min", "Min", NodeParamKind::Number, "0", nullptr },
        { "Max", "Max", NodeParamKind::Number, "1", nullptr }
    };

    // ---- Values -----------------------------------------------------------------

    constexpr Pin kNumberLiteralOut[] = { { "Value", Num } };
    constexpr Param kNumberLiteralParams[] = { { "Value", "Value", NodeParamKind::Number, "0", nullptr } };

    constexpr Pin kBoolLiteralOut[] = { { "Value", Bool } };
    constexpr Param kBoolLiteralParams[] = { { "Value", "Value", NodeParamKind::Bool, "false", nullptr } };

    constexpr Pin kStringLiteralOut[] = { { "Value", Str } };
    constexpr Param kStringLiteralParams[] = { { "Value", "Value", NodeParamKind::String, "", nullptr } };

    constexpr Pin kAppendIn[] = { { "A", Str }, { "B", Str } };
    constexpr Pin kStringOut[] = { { "Result", Str } };
    constexpr Param kAppendParams[] = {
        { "A", "A", NodeParamKind::String, "", nullptr },
        { "B", "B", NodeParamKind::String, "", nullptr }
    };

    constexpr Pin kToStringIn[] = { { "In", Any } };

    // ---- Variables --------------------------------------------------------------

    constexpr Pin kGetVariableOut[] = { { "Value", Any } };
    constexpr Param kVariableParams[] = { { "Variable", "Variable", NodeParamKind::Variable, "", nullptr } };

    constexpr Pin kSetVariableIn[] = { { "In", Exec }, { "Value", Any } };
    constexpr Pin kSetVariableOut[] = { { "Then", Exec }, { "Value", Any } };

    // ---- Functions --------------------------------------------------------------

    constexpr Pin kFunctionEntryOut[] = { { "Then", Exec } };
    constexpr Param kFunctionEntryParams[] = {
        { "Function", "Name", NodeParamKind::String, "NewFunction", nullptr }
    };

    constexpr Pin kCallFunctionIn[] = { { "In", Exec } };
    constexpr Pin kCallFunctionOut[] = { { "Then", Exec } };
    constexpr Param kCallFunctionParams[] = {
        { "Function", "Function", NodeParamKind::Function, "", nullptr }
    };

    // ---- UI ---------------------------------------------------------------------

    constexpr Pin kShowDocumentIn[] = { { "In", Exec }, { "Document", Str } };
    constexpr Pin kExecSuccessOut[] = { { "Then", Exec }, { "Success", Bool } };
    constexpr Param kShowDocumentParams[] = {
        { "Document", "Document", NodeParamKind::String, "hud.rml", nullptr }
    };

    constexpr Pin kSetUiVisibleIn[] = { { "In", Exec }, { "Visible", Bool } };
    constexpr Param kSetUiVisibleParams[] = { { "Visible", "Visible", NodeParamKind::Bool, "true", nullptr } };

    constexpr Pin kSetUiInputIn[] = { { "In", Exec }, { "Enabled", Bool } };
    constexpr Param kSetUiInputParams[] = { { "Enabled", "Enabled", NodeParamKind::Bool, "true", nullptr } };

    constexpr Pin kSetTextIn[] = { { "In", Exec }, { "Element Id", Str }, { "Text", Str } };
    constexpr Param kSetTextParams[] = {
        { "Element Id", "Element", NodeParamKind::String, "", nullptr },
        { "Text", "Text", NodeParamKind::String, "", nullptr }
    };

    constexpr Pin kSetPropertyIn[] = {
        { "In", Exec }, { "Element Id", Str }, { "Property", Str }, { "Value", Str }
    };
    constexpr Param kSetPropertyParams[] = {
        { "Element Id", "Element", NodeParamKind::String, "", nullptr },
        { "Property", "Property", NodeParamKind::String, "color", nullptr },
        { "Value", "Value", NodeParamKind::String, "white", nullptr }
    };

    constexpr Pin kSetClassIn[] = {
        { "In", Exec }, { "Element Id", Str }, { "Class", Str }, { "Enabled", Bool }
    };
    constexpr Param kSetClassParams[] = {
        { "Element Id", "Element", NodeParamKind::String, "", nullptr },
        { "Class", "Class", NodeParamKind::String, "", nullptr },
        { "Enabled", "Enabled", NodeParamKind::Bool, "true", nullptr }
    };

    constexpr Pin kSetElementVisibleIn[] = { { "In", Exec }, { "Element Id", Str }, { "Visible", Bool } };
    constexpr Param kSetElementVisibleParams[] = {
        { "Element Id", "Element", NodeParamKind::String, "", nullptr },
        { "Visible", "Visible", NodeParamKind::Bool, "true", nullptr }
    };

    // ---- Game -------------------------------------------------------------------

    constexpr Pin kPrintIn[] = { { "In", Exec }, { "Text", Str } };
    constexpr Param kPrintParams[] = { { "Text", "Text", NodeParamKind::String, "Hello", nullptr } };

    constexpr Pin kPlayTimeOut[] = { { "Seconds", Num } };

    template <std::size_t N>
    constexpr unsigned Count(const Pin (&)[N])
    {
        return static_cast<unsigned>(N);
    }

    template <std::size_t N>
    constexpr unsigned Count(const Param (&)[N])
    {
        return static_cast<unsigned>(N);
    }

    const NodeGraphNodeType kTypes[] = {
        { "Event.GameStart", "On Game Start", "Events",
          "Fires once when the play session starts.",
          nullptr, 0, kEventOut, Count(kEventOut), nullptr, 0 },
        { "Event.GameStop", "On Game Stop", "Events",
          "Fires once when the play session ends.",
          nullptr, 0, kEventOut, Count(kEventOut), nullptr, 0 },
        { "Event.Tick", "On Tick", "Events",
          "Fires every frame while the game runs.",
          nullptr, 0, kTickOut, Count(kTickOut), nullptr, 0 },

        { "Flow.Branch", "Branch", "Flow",
          "Takes the True or the False path depending on Condition.",
          kBranchIn, Count(kBranchIn), kBranchOut, Count(kBranchOut), kBranchParams, Count(kBranchParams) },
        { "Flow.Delay", "Delay", "Flow",
          "Waits the given number of seconds, then continues. Re-entering while waiting is ignored.",
          kDelayIn, Count(kDelayIn), kDelayOut, Count(kDelayOut), kDelayParams, Count(kDelayParams) },
        { "Flow.Gate", "Gate", "Flow",
          "Passes Enter through to Exit only while the gate is open.",
          kGateIn, Count(kGateIn), kGateOut, Count(kGateOut), kGateParams, Count(kGateParams) },
        { "Flow.Sequence", "Sequence", "Flow",
          "Fires each output in order, one after the other.",
          kExecIn, Count(kExecIn), kSequenceOut, Count(kSequenceOut), nullptr, 0 },
        { "Flow.DoOnce", "Do Once", "Flow",
          "Passes through the first time only, until Reset is fired.",
          kDoOnceIn, Count(kDoOnceIn), kDoOnceOut, Count(kDoOnceOut), kDoOnceParams, Count(kDoOnceParams) },
        { "Flow.DoN", "Do N", "Flow",
          "Passes through the first N times, until Reset is fired.",
          kDoNIn, Count(kDoNIn), kDoNOut, Count(kDoNOut), kDoNParams, Count(kDoNParams) },
        { "Flow.FlipFlop", "Flip Flop", "Flow",
          "Alternates between the A and B outputs on every execution.",
          kExecIn, Count(kExecIn), kFlipFlopOut, Count(kFlipFlopOut), nullptr, 0 },
        { "Flow.ForLoop", "For Loop", "Flow",
          "Fires Loop Body once for every index from First to Last, then Completed.",
          kForLoopIn, Count(kForLoopIn), kForLoopOut, Count(kForLoopOut), kForLoopParams, Count(kForLoopParams) },
        { "Flow.Switch", "Switch", "Flow",
          "Fires the case output matching Index, or Default when it is out of range.",
          kSwitchIn, Count(kSwitchIn), kSwitchOut, Count(kSwitchOut), kSwitchParams, Count(kSwitchParams) },

        { "Logic.Multiplexer", "Multiplexer", "Logic",
          "Forwards whichever input Index selects.",
          kMultiplexerIn, Count(kMultiplexerIn), kMultiplexerOut, Count(kMultiplexerOut),
          kMultiplexerParams, Count(kMultiplexerParams) },
        { "Logic.And", "And", "Logic", "True when both inputs are true.",
          kBoolPairIn, Count(kBoolPairIn), kBoolOut, Count(kBoolOut), nullptr, 0 },
        { "Logic.Or", "Or", "Logic", "True when either input is true.",
          kBoolPairIn, Count(kBoolPairIn), kBoolOut, Count(kBoolOut), nullptr, 0 },
        { "Logic.Not", "Not", "Logic", "Inverts the input.",
          kNotIn, Count(kNotIn), kBoolOut, Count(kBoolOut), nullptr, 0 },
        { "Logic.Compare", "Compare", "Logic", "Compares two numbers.",
          kComparePairIn, Count(kComparePairIn), kBoolOut, Count(kBoolOut),
          kCompareParams, Count(kCompareParams) },

        { "Math.Add", "Add", "Math", "A + B",
          kNumPairIn, Count(kNumPairIn), kNumOut, Count(kNumOut), kNumPairParams, Count(kNumPairParams) },
        { "Math.Subtract", "Subtract", "Math", "A - B",
          kNumPairIn, Count(kNumPairIn), kNumOut, Count(kNumOut), kNumPairParams, Count(kNumPairParams) },
        { "Math.Multiply", "Multiply", "Math", "A * B",
          kNumPairIn, Count(kNumPairIn), kNumOut, Count(kNumOut), kNumPairParams, Count(kNumPairParams) },
        { "Math.Divide", "Divide", "Math", "A / B, or 0 when B is zero.",
          kNumPairIn, Count(kNumPairIn), kNumOut, Count(kNumOut), kNumPairParams, Count(kNumPairParams) },
        { "Math.Clamp", "Clamp", "Math", "Keeps Value inside the Min..Max range.",
          kClampIn, Count(kClampIn), kNumOut, Count(kNumOut), kClampParams, Count(kClampParams) },
        { "Math.Lerp", "Lerp", "Math", "Blends from A to B by Alpha.",
          kLerpIn, Count(kLerpIn), kNumOut, Count(kNumOut), kLerpParams, Count(kLerpParams) },
        { "Math.RandomRange", "Random Range", "Math",
          "A new random number between Min and Max each time it is read.",
          kRandomIn, Count(kRandomIn), kNumOut, Count(kNumOut), kRandomParams, Count(kRandomParams) },

        { "Value.Number", "Number", "Values", "A number literal.",
          nullptr, 0, kNumberLiteralOut, Count(kNumberLiteralOut),
          kNumberLiteralParams, Count(kNumberLiteralParams) },
        { "Value.Bool", "Boolean", "Values", "A true/false literal.",
          nullptr, 0, kBoolLiteralOut, Count(kBoolLiteralOut),
          kBoolLiteralParams, Count(kBoolLiteralParams) },
        { "Value.String", "String", "Values", "A text literal.",
          nullptr, 0, kStringLiteralOut, Count(kStringLiteralOut),
          kStringLiteralParams, Count(kStringLiteralParams) },
        { "Value.Append", "Append", "Values", "Joins two strings.",
          kAppendIn, Count(kAppendIn), kStringOut, Count(kStringOut), kAppendParams, Count(kAppendParams) },
        { "Value.ToString", "To String", "Values", "Converts any value to text.",
          kToStringIn, Count(kToStringIn), kStringOut, Count(kStringOut), nullptr, 0 },

        { "Variable.Get", "Get Variable", "Variables", "Reads one of the graph's variables.",
          nullptr, 0, kGetVariableOut, Count(kGetVariableOut), kVariableParams, Count(kVariableParams) },
        { "Variable.Set", "Set Variable", "Variables",
          "Writes one of the graph's variables and passes the value on.",
          kSetVariableIn, Count(kSetVariableIn), kSetVariableOut, Count(kSetVariableOut),
          kVariableParams, Count(kVariableParams) },

        { "Function.Entry", "Function Entry", "Functions",
          "Start of a named function. Call Function nodes anywhere in the graph run it.",
          nullptr, 0, kFunctionEntryOut, Count(kFunctionEntryOut),
          kFunctionEntryParams, Count(kFunctionEntryParams) },
        { "Function.Call", "Call Function", "Functions",
          "Runs a function and continues once it has finished.",
          kCallFunctionIn, Count(kCallFunctionIn), kCallFunctionOut, Count(kCallFunctionOut),
          kCallFunctionParams, Count(kCallFunctionParams) },
        { "Function.Return", "Return", "Functions",
          "Ends the function body early. Execution resumes after the Call Function node.",
          kCallFunctionIn, Count(kCallFunctionIn), nullptr, 0, nullptr, 0 },

        { "UI.ShowDocument", "Show UI Document", "UI",
          "Loads a document from Data/UI and shows it.",
          kShowDocumentIn, Count(kShowDocumentIn), kExecSuccessOut, Count(kExecSuccessOut),
          kShowDocumentParams, Count(kShowDocumentParams) },
        { "UI.CloseDocument", "Close UI Document", "UI", "Closes the document currently shown.",
          kExecIn, Count(kExecIn), kExecOut, Count(kExecOut), nullptr, 0 },
        { "UI.ReloadDocument", "Reload UI Document", "UI", "Re-reads the current document from disk.",
          kExecIn, Count(kExecIn), kExecSuccessOut, Count(kExecSuccessOut), nullptr, 0 },
        { "UI.SetVisible", "Set UI Visible", "UI", "Shows or hides the whole UI layer.",
          kSetUiVisibleIn, Count(kSetUiVisibleIn), kExecOut, Count(kExecOut),
          kSetUiVisibleParams, Count(kSetUiVisibleParams) },
        { "UI.SetInputEnabled", "Set UI Input Enabled", "UI",
          "Lets the UI take mouse and keyboard input, or not.",
          kSetUiInputIn, Count(kSetUiInputIn), kExecOut, Count(kExecOut),
          kSetUiInputParams, Count(kSetUiInputParams) },
        { "UI.SetText", "Set UI Text", "UI", "Replaces the contents of an element.",
          kSetTextIn, Count(kSetTextIn), kExecSuccessOut, Count(kExecSuccessOut),
          kSetTextParams, Count(kSetTextParams) },
        { "UI.SetProperty", "Set UI Property", "UI", "Sets one RCSS property on an element.",
          kSetPropertyIn, Count(kSetPropertyIn), kExecSuccessOut, Count(kExecSuccessOut),
          kSetPropertyParams, Count(kSetPropertyParams) },
        { "UI.SetClass", "Set UI Class", "UI", "Adds or removes a class on an element.",
          kSetClassIn, Count(kSetClassIn), kExecSuccessOut, Count(kExecSuccessOut),
          kSetClassParams, Count(kSetClassParams) },
        { "UI.SetElementVisible", "Set UI Element Visible", "UI", "Shows or hides a single element.",
          kSetElementVisibleIn, Count(kSetElementVisibleIn), kExecSuccessOut, Count(kExecSuccessOut),
          kSetElementVisibleParams, Count(kSetElementVisibleParams) },

        { "Game.Stop", "Stop Game", "Game", "Leaves play mode, as Escape does.",
          kExecIn, Count(kExecIn), nullptr, 0, nullptr, 0 },
        { "Game.GetPlayTime", "Get Play Time", "Game", "Seconds since the play session started.",
          nullptr, 0, kPlayTimeOut, Count(kPlayTimeOut), nullptr, 0 },
        { "Debug.Print", "Print String", "Game", "Writes a line to the debug output and the editor log.",
          kPrintIn, Count(kPrintIn), kExecOut, Count(kExecOut), kPrintParams, Count(kPrintParams) }
    };
}

namespace NodeGraphCatalog
{
    const NodeGraphNodeType* Types(std::size_t& count)
    {
        count = sizeof(kTypes) / sizeof(kTypes[0]);
        return kTypes;
    }

    const NodeGraphNodeType* Find(const char* id)
    {
        if (id == nullptr)
        {
            return nullptr;
        }

        for (const NodeGraphNodeType& type : kTypes)
        {
            if (std::strcmp(type.Id, id) == 0)
            {
                return &type;
            }
        }

        return nullptr;
    }

    const NodeGraphPin* FindInput(const NodeGraphNodeType& type, const char* pinName)
    {
        const int index = IndexOfInput(type, pinName);
        return index < 0 ? nullptr : &type.Inputs[index];
    }

    const NodeGraphParam* FindParam(const NodeGraphNodeType& type, const char* key)
    {
        if (key == nullptr)
        {
            return nullptr;
        }

        for (unsigned i = 0; i < type.ParamCount; ++i)
        {
            if (std::strcmp(type.Params[i].Key, key) == 0)
            {
                return &type.Params[i];
            }
        }

        return nullptr;
    }

    int IndexOfInput(const NodeGraphNodeType& type, const char* pinName)
    {
        if (pinName == nullptr)
        {
            return -1;
        }

        for (unsigned i = 0; i < type.InputCount; ++i)
        {
            if (std::strcmp(type.Inputs[i].Name, pinName) == 0)
            {
                return static_cast<int>(i);
            }
        }

        return -1;
    }

    int IndexOfOutput(const NodeGraphNodeType& type, const char* pinName)
    {
        if (pinName == nullptr)
        {
            return -1;
        }

        for (unsigned i = 0; i < type.OutputCount; ++i)
        {
            if (std::strcmp(type.Outputs[i].Name, pinName) == 0)
            {
                return static_cast<int>(i);
            }
        }

        return -1;
    }

    const char* PinKindId(NodePinKind kind)
    {
        switch (kind)
        {
        case NodePinKind::Exec: return "exec";
        case NodePinKind::Bool: return "bool";
        case NodePinKind::Number: return "number";
        case NodePinKind::String: return "string";
        case NodePinKind::Any: return "any";
        }

        return "any";
    }

    const char* PinKindName(NodePinKind kind)
    {
        switch (kind)
        {
        case NodePinKind::Exec: return "Exec";
        case NodePinKind::Bool: return "Bool";
        case NodePinKind::Number: return "Number";
        case NodePinKind::String: return "String";
        case NodePinKind::Any: return "Any";
        }

        return "Any";
    }

    NodePinKind PinKindFromId(const char* id)
    {
        if (id != nullptr)
        {
            if (std::strcmp(id, "exec") == 0) return NodePinKind::Exec;
            if (std::strcmp(id, "bool") == 0) return NodePinKind::Bool;
            if (std::strcmp(id, "number") == 0) return NodePinKind::Number;
            if (std::strcmp(id, "string") == 0) return NodePinKind::String;
        }

        return NodePinKind::Any;
    }

    unsigned PinKindColor(NodePinKind kind)
    {
        switch (kind)
        {
        case NodePinKind::Exec: return 0xf0f0f0;
        case NodePinKind::Bool: return 0xc0392b;
        case NodePinKind::Number: return 0x4fc3a1;
        case NodePinKind::String: return 0xd66fd6;
        case NodePinKind::Any: return 0x9aa0a6;
        }

        return 0x9aa0a6;
    }

    bool IsEventType(const NodeGraphNodeType& type)
    {
        // A function entry has the same shape as an event - no exec input, one exec output
        // - but the runtime fires events itself and only ever reaches a function through a
        // Call node, so the two must not be confused.
        if (std::strcmp(type.Category, "Functions") == 0)
        {
            return false;
        }

        for (unsigned i = 0; i < type.InputCount; ++i)
        {
            if (type.Inputs[i].Kind == NodePinKind::Exec)
            {
                return false;
            }
        }

        return type.OutputCount > 0 && type.Outputs[0].Kind == NodePinKind::Exec;
    }
}
