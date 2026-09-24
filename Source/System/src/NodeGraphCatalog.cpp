#include "System/NodeGraphCatalog.h"

#include <cstring>

namespace
{
    constexpr NodePinKind Exec = NodePinKind::Exec;
    constexpr NodePinKind Bool = NodePinKind::Bool;
    constexpr NodePinKind Num = NodePinKind::Number;
    constexpr NodePinKind Str = NodePinKind::String;
    constexpr NodePinKind Any = NodePinKind::Any;
    constexpr NodePinKind Ent = NodePinKind::Entity;

    using Pin = NodeGraphPin;
    using Param = NodeGraphParam;

    // Shared option lists. The runtime matches these by name, so a label changed here
    // must change in NodeGraphRuntime.cpp too.
    constexpr const char* kKeyItems =
        "Space|Enter|Escape|Tab|Backspace|Up|Down|Left|Right|Shift|Ctrl|Alt|"
        "Left Mouse|Right Mouse|Middle Mouse|"
        "F1|F2|F3|F4|F5|F6|F7|F8|F9|F10|F11|F12|"
        "A|B|C|D|E|F|G|H|I|J|K|L|M|N|O|P|Q|R|S|T|U|V|W|X|Y|Z|"
        "0|1|2|3|4|5|6|7|8|9";
    constexpr const char* kEasingItems = "Linear|Smooth|Smoother|Ease In|Ease Out|Ease In Out";
    constexpr const char* kEntityPropertyItems =
        "Light Intensity|Light Radius|Light Color R|Light Color G|Light Color B|"
        "Light Temperature|Light Uses Temperature|Light Casts Shadows|"
        "Particles Enabled|Particle Spawn Rate|Rain Enabled|Rain Intensity";

    // ---- Events -----------------------------------------------------------------

    constexpr Pin kEventOut[] = { { "Then", Exec } };
    constexpr Pin kTickOut[] = { { "Then", Exec }, { "Delta Seconds", Num } };

    constexpr Param kKeyParams[] = { { "Key", "Key", NodeParamKind::Enum, "Space", kKeyItems } };

    constexpr Pin kUiClickedOut[] = { { "Then", Exec }, { "Element Id", Str } };
    constexpr Param kUiClickedParams[] = { { "Element Id", "Button", NodeParamKind::String, "", nullptr } };

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
    constexpr Param kRandomIntegerParams[] = {
        { "Min", "Min", NodeParamKind::Number, "1", nullptr },
        { "Max", "Max", NodeParamKind::Number, "6", nullptr }
    };

    constexpr Pin kRandomChanceIn[] = { { "Probability", Num } };
    constexpr Param kRandomChanceParams[] = { { "Probability", "Probability", NodeParamKind::Number, "0.5", nullptr } };

    constexpr Pin kNumIn[] = { { "Value", Num } };
    constexpr Param kNumParams[] = { { "Value", "Value", NodeParamKind::Number, "0", nullptr } };

    constexpr Pin kEaseIn[] = { { "Alpha", Num } };
    constexpr Param kEaseParams[] = {
        { "Alpha", "Alpha", NodeParamKind::Number, "0.5", nullptr },
        { "Easing", "Easing", NodeParamKind::Enum, "Smoother", kEasingItems }
    };

    constexpr Param kLerpAngleParams[] = {
        { "A", "A", NodeParamKind::Number, "0", nullptr },
        { "B", "B", NodeParamKind::Number, "90", nullptr },
        { "Alpha", "Alpha", NodeParamKind::Number, "0.5", nullptr }
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

    constexpr Param kFormatNumberParams[] = {
        { "Value", "Value", NodeParamKind::Number, "0", nullptr },
        { "Decimals", "Decimals", NodeParamKind::Number, "0", nullptr },
        { "Grouping", "1,000s", NodeParamKind::Bool, "true", nullptr }
    };

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

    // ---- Video ------------------------------------------------------------------

    constexpr Pin kPlayVideoIn[] = { { "In", Exec }, { "Video", Str }, { "Loop", Bool } };
    constexpr Param kPlayVideoParams[] = {
        { "Video", "Video", NodeParamKind::String, "", nullptr },
        { "Loop", "Loop", NodeParamKind::Bool, "false", nullptr },
        { "Fit", "Fit", NodeParamKind::Enum, "Letterbox", "Letterbox|Fill|Stretch" },
        { "Volume", "Volume", NodeParamKind::Number, "1", nullptr }
    };

    constexpr Pin kSetVideoVolumeIn[] = { { "In", Exec }, { "Volume", Num } };
    constexpr Param kSetVideoVolumeParams[] = { { "Volume", "Volume", NodeParamKind::Number, "1", nullptr } };

    constexpr Pin kSeekVideoIn[] = { { "In", Exec }, { "Seconds", Num } };
    constexpr Param kSeekVideoParams[] = { { "Seconds", "Seconds", NodeParamKind::Number, "0", nullptr } };

    constexpr Pin kSetVideoLoopingIn[] = { { "In", Exec }, { "Loop", Bool } };
    constexpr Param kSetVideoLoopingParams[] = { { "Loop", "Loop", NodeParamKind::Bool, "true", nullptr } };

    constexpr Pin kVideoPlayingOut[] = { { "Playing", Bool } };
    constexpr Pin kSecondsOut[] = { { "Seconds", Num } };

    // ---- Input ------------------------------------------------------------------

    constexpr Pin kKeyDownOut[] = { { "Down", Bool } };

    // ---- Entity -----------------------------------------------------------------

    // Every entity node takes its target on an "Entity" pin whose unconnected default is
    // the picker on the node body, so the common case - one specific entity - needs no
    // extra node, and a Find Entity or Entity node can still be wired in when it varies.
    constexpr Param kEntityParam = { "Entity", "Entity", NodeParamKind::Entity, "", nullptr };

    constexpr Pin kEntityRefOut[] = { { "Entity", Ent } };
    constexpr Param kEntityRefParams[] = { kEntityParam };

    constexpr Pin kFindEntityIn[] = { { "Name", Str } };
    constexpr Pin kFindEntityOut[] = { { "Entity", Ent }, { "Found", Bool } };
    constexpr Param kFindEntityParams[] = { { "Name", "Name", NodeParamKind::String, "", nullptr } };

    constexpr Pin kEntityIn[] = { { "Entity", Ent } };
    constexpr Pin kEntityValidOut[] = { { "Valid", Bool } };
    constexpr Pin kEntityNameOut[] = { { "Name", Str } };
    constexpr Pin kVectorOut[] = { { "X", Num }, { "Y", Num }, { "Z", Num } };

    constexpr Pin kSetEntityVectorIn[] = { { "In", Exec }, { "Entity", Ent }, { "X", Num }, { "Y", Num }, { "Z", Num } };
    constexpr Param kSetEntityPositionParams[] = {
        kEntityParam,
        { "X", "X", NodeParamKind::Number, "0", nullptr },
        { "Y", "Y", NodeParamKind::Number, "0", nullptr },
        { "Z", "Z", NodeParamKind::Number, "0", nullptr }
    };
    constexpr Param kSetEntityScaleParams[] = {
        kEntityParam,
        { "X", "X", NodeParamKind::Number, "1", nullptr },
        { "Y", "Y", NodeParamKind::Number, "1", nullptr },
        { "Z", "Z", NodeParamKind::Number, "1", nullptr }
    };

    constexpr Pin kMoveEntityToIn[] = {
        { "In", Exec }, { "Entity", Ent }, { "X", Num }, { "Y", Num }, { "Z", Num }, { "Duration", Num }
    };
    constexpr Pin kLatentOut[] = { { "Then", Exec }, { "Completed", Exec } };
    constexpr Param kMoveEntityToParams[] = {
        kEntityParam,
        { "X", "X", NodeParamKind::Number, "0", nullptr },
        { "Y", "Y", NodeParamKind::Number, "0", nullptr },
        { "Z", "Z", NodeParamKind::Number, "0", nullptr },
        { "Duration", "Seconds", NodeParamKind::Number, "1", nullptr },
        { "Easing", "Easing", NodeParamKind::Enum, "Smoother", kEasingItems },
        { "Arc Height", "Arc height", NodeParamKind::Number, "0", nullptr }
    };

    constexpr Pin kGetEntityPropertyOut[] = { { "Value", Num }, { "Success", Bool } };
    constexpr Param kGetEntityPropertyParams[] = {
        kEntityParam,
        { "Property", "Property", NodeParamKind::Enum, "Light Intensity", kEntityPropertyItems }
    };

    constexpr Pin kSetEntityPropertyIn[] = { { "In", Exec }, { "Entity", Ent }, { "Value", Num } };
    constexpr Param kSetEntityPropertyParams[] = {
        kEntityParam,
        { "Property", "Property", NodeParamKind::Enum, "Light Intensity", kEntityPropertyItems },
        { "Value", "Value", NodeParamKind::Number, "800", nullptr }
    };

    constexpr Pin kEntityActionIn[] = { { "In", Exec }, { "Entity", Ent } };

    // ---- Camera -----------------------------------------------------------------

    constexpr Pin kCameraPoseOut[] = { { "X", Num }, { "Y", Num }, { "Z", Num }, { "Pitch", Num }, { "Yaw", Num } };
    constexpr Pin kSetCameraIn[] = {
        { "In", Exec }, { "X", Num }, { "Y", Num }, { "Z", Num }, { "Pitch", Num }, { "Yaw", Num }
    };
    constexpr Param kSetCameraParams[] = {
        { "X", "X", NodeParamKind::Number, "0", nullptr },
        { "Y", "Y", NodeParamKind::Number, "0", nullptr },
        { "Z", "Z", NodeParamKind::Number, "2", nullptr },
        { "Pitch", "Pitch", NodeParamKind::Number, "0", nullptr },
        { "Yaw", "Yaw", NodeParamKind::Number, "0", nullptr }
    };
    constexpr Pin kMoveCameraToIn[] = {
        { "In", Exec }, { "X", Num }, { "Y", Num }, { "Z", Num }, { "Pitch", Num }, { "Yaw", Num }, { "Duration", Num }
    };
    constexpr Param kMoveCameraToParams[] = {
        { "X", "X", NodeParamKind::Number, "0", nullptr },
        { "Y", "Y", NodeParamKind::Number, "0", nullptr },
        { "Z", "Z", NodeParamKind::Number, "2", nullptr },
        { "Pitch", "Pitch", NodeParamKind::Number, "0", nullptr },
        { "Yaw", "Yaw", NodeParamKind::Number, "0", nullptr },
        { "Duration", "Seconds", NodeParamKind::Number, "2", nullptr },
        { "Easing", "Easing", NodeParamKind::Enum, "Smoother", kEasingItems }
    };

    constexpr Pin kLookPointIn[] = { { "Plane Height", Num } };
    constexpr Pin kLookPointOut[] = { { "X", Num }, { "Y", Num }, { "Z", Num }, { "Hit", Bool } };
    constexpr Param kLookPointParams[] = { { "Plane Height", "Plane Z", NodeParamKind::Number, "0", nullptr } };

    // ---- Audio ------------------------------------------------------------------

    constexpr Pin kAudioEventIn[] = { { "In", Exec }, { "Event", Str } };
    constexpr Param kPlaySoundParams[] = { { "Event", "Event", NodeParamKind::String, "Click", nullptr } };
    constexpr Param kPlayMusicParams[] = { { "Event", "Event", NodeParamKind::String, "Music1", nullptr } };

    constexpr Pin kPlaylistIn[] = { { "In", Exec }, { "Prefix", Str }, { "Count", Num } };
    constexpr Param kPlaylistParams[] = {
        { "Prefix", "Prefix", NodeParamKind::String, "Music", nullptr },
        { "Count", "Tracks", NodeParamKind::Number, "5", nullptr },
        { "Shuffle", "Shuffle", NodeParamKind::Bool, "true", nullptr }
    };

    constexpr Pin kMusicPlayingOut[] = { { "Playing", Bool } };

    // ---- Game -------------------------------------------------------------------

    constexpr Pin kStandaloneOut[] = { { "Standalone", Bool } };

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
        { "Event.VideoFinished", "On Video Finished", "Events",
          "Fires when a video started by Play Video reaches its end without looping. The video has already been hidden.",
          nullptr, 0, kEventOut, Count(kEventOut), nullptr, 0 },
        { "Event.KeyPressed", "On Key Pressed", "Events",
          "Fires once when the key goes down while the game window has focus. Holding it does not repeat.",
          nullptr, 0, kEventOut, Count(kEventOut), kKeyParams, Count(kKeyParams) },
        { "Event.KeyReleased", "On Key Released", "Events",
          "Fires once when the key comes back up.",
          nullptr, 0, kEventOut, Count(kEventOut), kKeyParams, Count(kKeyParams) },
        { "Event.UiButtonClicked", "On UI Button Clicked", "Events",
          "Fires when a <button> in the game UI is clicked, or activated with Enter/Space. Button is its "
          "id; leave it empty to catch every button and branch on Element Id instead.",
          nullptr, 0, kUiClickedOut, Count(kUiClickedOut), kUiClickedParams, Count(kUiClickedParams) },

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
        { "Math.RandomInteger", "Random Integer", "Math",
          "A new whole number from Min to Max, both included, each time it is read.",
          kRandomIn, Count(kRandomIn), kNumOut, Count(kNumOut), kRandomIntegerParams, Count(kRandomIntegerParams) },
        { "Math.RandomChance", "Random Chance", "Math",
          "True with the given probability (0..1), rolled again each time it is read.",
          kRandomChanceIn, Count(kRandomChanceIn), kBoolOut, Count(kBoolOut),
          kRandomChanceParams, Count(kRandomChanceParams) },
        { "Math.Min", "Min", "Math", "The smaller of A and B.",
          kNumPairIn, Count(kNumPairIn), kNumOut, Count(kNumOut), kNumPairParams, Count(kNumPairParams) },
        { "Math.Max", "Max", "Math", "The larger of A and B.",
          kNumPairIn, Count(kNumPairIn), kNumOut, Count(kNumOut), kNumPairParams, Count(kNumPairParams) },
        { "Math.Modulo", "Modulo", "Math",
          "Remainder of A / B, always between 0 and B - so it wraps negative numbers too. 0 when B is zero.",
          kNumPairIn, Count(kNumPairIn), kNumOut, Count(kNumOut), kNumPairParams, Count(kNumPairParams) },
        { "Math.Abs", "Abs", "Math", "The value without its sign.",
          kNumIn, Count(kNumIn), kNumOut, Count(kNumOut), kNumParams, Count(kNumParams) },
        { "Math.Floor", "Floor", "Math", "Rounds down to a whole number.",
          kNumIn, Count(kNumIn), kNumOut, Count(kNumOut), kNumParams, Count(kNumParams) },
        { "Math.Sin", "Sin", "Math", "Sine of an angle in degrees.",
          kNumIn, Count(kNumIn), kNumOut, Count(kNumOut), kNumParams, Count(kNumParams) },
        { "Math.Cos", "Cos", "Math", "Cosine of an angle in degrees.",
          kNumIn, Count(kNumIn), kNumOut, Count(kNumOut), kNumParams, Count(kNumParams) },
        { "Math.Ease", "Ease", "Math",
          "Reshapes a 0..1 Alpha into a curve, for animating with Lerp. Smoother starts and stops without a jolt.",
          kEaseIn, Count(kEaseIn), kNumOut, Count(kNumOut), kEaseParams, Count(kEaseParams) },
        { "Math.LerpAngle", "Lerp Angle", "Math",
          "Blends between two angles in degrees the short way round, so 350 to 10 passes through 0, not 180.",
          kLerpIn, Count(kLerpIn), kNumOut, Count(kNumOut), kLerpAngleParams, Count(kLerpAngleParams) },

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
        { "Value.FormatNumber", "Format Number", "Values",
          "Number as display text: a fixed count of decimals, and thousands separated with commas (3,000).",
          kNumIn, Count(kNumIn), kStringOut, Count(kStringOut), kFormatNumberParams, Count(kFormatNumberParams) },

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

        { "Video.Play", "Play Video", "Video",
          "Plays a .webm from Data over the whole screen, under the game UI. Video is the path inside Data "
          "(\".webm\" optional) or just the file name. Replaces any video already playing.",
          kPlayVideoIn, Count(kPlayVideoIn), kExecSuccessOut, Count(kExecSuccessOut),
          kPlayVideoParams, Count(kPlayVideoParams) },
        { "Video.Pause", "Pause Video", "Video", "Holds the video on its current frame.",
          kExecIn, Count(kExecIn), kExecOut, Count(kExecOut), nullptr, 0 },
        { "Video.Resume", "Resume Video", "Video", "Continues a paused video.",
          kExecIn, Count(kExecIn), kExecOut, Count(kExecOut), nullptr, 0 },
        { "Video.Stop", "Stop Video", "Video", "Ends the video and hides it. On Video Finished does not fire.",
          kExecIn, Count(kExecIn), kExecOut, Count(kExecOut), nullptr, 0 },
        { "Video.Seek", "Seek Video", "Video", "Jumps to a time in the video, in seconds.",
          kSeekVideoIn, Count(kSeekVideoIn), kExecOut, Count(kExecOut),
          kSeekVideoParams, Count(kSeekVideoParams) },
        { "Video.SetLooping", "Set Video Looping", "Video", "Turns looping of the current video on or off.",
          kSetVideoLoopingIn, Count(kSetVideoLoopingIn), kExecOut, Count(kExecOut),
          kSetVideoLoopingParams, Count(kSetVideoLoopingParams) },
        { "Video.SetVolume", "Set Video Volume", "Video",
          "Loudness of the video's soundtrack, from 0 to 1. Kept for the videos played after it too.",
          kSetVideoVolumeIn, Count(kSetVideoVolumeIn), kExecOut, Count(kExecOut),
          kSetVideoVolumeParams, Count(kSetVideoVolumeParams) },
        { "Video.IsPlaying", "Is Video Playing", "Video", "True while a video is showing and not paused.",
          nullptr, 0, kVideoPlayingOut, Count(kVideoPlayingOut), nullptr, 0 },
        { "Video.GetTime", "Get Video Time", "Video", "Position of the current video, in seconds.",
          nullptr, 0, kSecondsOut, Count(kSecondsOut), nullptr, 0 },
        { "Video.GetDuration", "Get Video Duration", "Video", "Length of the current video, in seconds.",
          nullptr, 0, kSecondsOut, Count(kSecondsOut), nullptr, 0 },

        { "Input.IsKeyDown", "Is Key Down", "Input",
          "True while the key is held and the game window has focus.",
          nullptr, 0, kKeyDownOut, Count(kKeyDownOut), kKeyParams, Count(kKeyParams) },

        { "Entity.Reference", "Entity", "Entity",
          "One entity of the level, picked by id. Renaming or reordering entities does not re-point it.",
          nullptr, 0, kEntityRefOut, Count(kEntityRefOut), kEntityRefParams, Count(kEntityRefParams) },
        { "Entity.FindByName", "Find Entity", "Entity",
          "The first entity with exactly this name. For entities that are only known at runtime; "
          "prefer the Entity picker otherwise, since a rename breaks a name.",
          kFindEntityIn, Count(kFindEntityIn), kFindEntityOut, Count(kFindEntityOut),
          kFindEntityParams, Count(kFindEntityParams) },
        { "Entity.IsValid", "Is Entity Valid", "Entity", "True when the entity exists in the running level.",
          kEntityIn, Count(kEntityIn), kEntityValidOut, Count(kEntityValidOut),
          kEntityRefParams, Count(kEntityRefParams) },
        { "Entity.GetName", "Get Entity Name", "Entity", "The entity's name as shown in the Outliner.",
          kEntityIn, Count(kEntityIn), kEntityNameOut, Count(kEntityNameOut),
          kEntityRefParams, Count(kEntityRefParams) },
        { "Entity.GetPosition", "Get Entity Position", "Entity", "World position, in metres.",
          kEntityIn, Count(kEntityIn), kVectorOut, Count(kVectorOut), kEntityRefParams, Count(kEntityRefParams) },
        { "Entity.SetPosition", "Set Entity Position", "Entity", "Moves the entity to a world position, in metres.",
          kSetEntityVectorIn, Count(kSetEntityVectorIn), kExecSuccessOut, Count(kExecSuccessOut),
          kSetEntityPositionParams, Count(kSetEntityPositionParams) },
        { "Entity.AddPosition", "Move Entity By", "Entity",
          "Adds an offset to the entity's position. Multiply by Delta Seconds on Tick for a steady speed.",
          kSetEntityVectorIn, Count(kSetEntityVectorIn), kExecSuccessOut, Count(kExecSuccessOut),
          kSetEntityPositionParams, Count(kSetEntityPositionParams) },
        { "Entity.GetRotation", "Get Entity Rotation", "Entity", "Rotation about X, Y and Z, in degrees.",
          kEntityIn, Count(kEntityIn), kVectorOut, Count(kVectorOut), kEntityRefParams, Count(kEntityRefParams) },
        { "Entity.SetRotation", "Set Entity Rotation", "Entity", "Rotation about X, Y and Z, in degrees.",
          kSetEntityVectorIn, Count(kSetEntityVectorIn), kExecSuccessOut, Count(kExecSuccessOut),
          kSetEntityPositionParams, Count(kSetEntityPositionParams) },
        { "Entity.AddRotation", "Rotate Entity By", "Entity",
          "Adds to the entity's rotation, in degrees. Multiply by Delta Seconds on Tick for a steady spin.",
          kSetEntityVectorIn, Count(kSetEntityVectorIn), kExecSuccessOut, Count(kExecSuccessOut),
          kSetEntityPositionParams, Count(kSetEntityPositionParams) },
        { "Entity.GetScale", "Get Entity Scale", "Entity", "Scale along X, Y and Z; 1 is the authored size.",
          kEntityIn, Count(kEntityIn), kVectorOut, Count(kVectorOut), kEntityRefParams, Count(kEntityRefParams) },
        { "Entity.SetScale", "Set Entity Scale", "Entity", "Scale along X, Y and Z; 1 is the authored size.",
          kSetEntityVectorIn, Count(kSetEntityVectorIn), kExecSuccessOut, Count(kExecSuccessOut),
          kSetEntityScaleParams, Count(kSetEntityScaleParams) },
        { "Entity.MoveTo", "Move Entity To", "Entity",
          "Glides the entity to a position over Duration seconds. Then fires at once, Completed on arrival. "
          "Arc Height lifts it along the way, for a throw or a hop. Firing In again restarts from where it is.",
          kMoveEntityToIn, Count(kMoveEntityToIn), kLatentOut, Count(kLatentOut),
          kMoveEntityToParams, Count(kMoveEntityToParams) },
        { "Entity.GetProperty", "Get Entity Property", "Entity",
          "Reads a component property. Success is false when the entity lacks that component. "
          "Booleans read as 1 or 0.",
          kEntityIn, Count(kEntityIn), kGetEntityPropertyOut, Count(kGetEntityPropertyOut),
          kGetEntityPropertyParams, Count(kGetEntityPropertyParams) },
        { "Entity.SetProperty", "Set Entity Property", "Entity",
          "Changes a component property - a light's intensity or colour, a particle system's rate. "
          "Success is false when the entity lacks that component. Booleans take any non-zero as true.",
          kSetEntityPropertyIn, Count(kSetEntityPropertyIn), kExecSuccessOut, Count(kExecSuccessOut),
          kSetEntityPropertyParams, Count(kSetEntityPropertyParams) },
        { "Entity.PlayAudio", "Play Entity Audio", "Entity",
          "Starts the entity's Audio Emitter, positioned where the entity is.",
          kEntityActionIn, Count(kEntityActionIn), kExecSuccessOut, Count(kExecSuccessOut),
          kEntityRefParams, Count(kEntityRefParams) },
        { "Entity.StopAudio", "Stop Entity Audio", "Entity", "Stops the entity's Audio Emitter.",
          kEntityActionIn, Count(kEntityActionIn), kExecSuccessOut, Count(kExecSuccessOut),
          kEntityRefParams, Count(kEntityRefParams) },

        { "Camera.Get", "Get Camera", "Camera",
          "Where the game camera is, in metres, and where it looks: Pitch and Yaw in degrees.",
          nullptr, 0, kCameraPoseOut, Count(kCameraPoseOut), nullptr, 0 },
        { "Camera.Set", "Set Camera", "Camera",
          "Places the game camera and holds it there - the game module's own camera is overridden until "
          "Release Camera. Pitch and Yaw in degrees; Yaw 0 looks along +Y.",
          kSetCameraIn, Count(kSetCameraIn), kExecOut, Count(kExecOut), kSetCameraParams, Count(kSetCameraParams) },
        { "Camera.MoveTo", "Move Camera To", "Camera",
          "Flies the camera from where it is to a pose over Duration seconds, turning the short way round. "
          "Then fires at once, Completed on arrival. Holds the camera, as Set Camera does.",
          kMoveCameraToIn, Count(kMoveCameraToIn), kLatentOut, Count(kLatentOut),
          kMoveCameraToParams, Count(kMoveCameraToParams) },
        { "Camera.Release", "Release Camera", "Camera",
          "Hands the camera back to the game module after Set Camera or Move Camera To.",
          kExecIn, Count(kExecIn), kExecOut, Count(kExecOut), nullptr, 0 },
        { "Camera.LookPoint", "Camera Look Point", "Camera",
          "Where the centre of the screen meets a flat plane at height Plane Z: the spot the player is "
          "looking at. Hit is false when the camera looks away from the plane.",
          kLookPointIn, Count(kLookPointIn), kLookPointOut, Count(kLookPointOut),
          kLookPointParams, Count(kLookPointParams) },

        { "Audio.PlaySound", "Play Sound", "Audio",
          "Fires an FMOD event once. Event is its name (\"Click\") or full path; the folder does not matter.",
          kAudioEventIn, Count(kAudioEventIn), kExecOut, Count(kExecOut), kPlaySoundParams, Count(kPlaySoundParams) },
        { "Audio.PlayMusic", "Play Music", "Audio",
          "Plays an event on the single music channel, replacing the track or playlist already playing.",
          kAudioEventIn, Count(kAudioEventIn), kExecSuccessOut, Count(kExecSuccessOut),
          kPlayMusicParams, Count(kPlayMusicParams) },
        { "Audio.PlayPlaylist", "Play Music Playlist", "Audio",
          "Keeps the music channel playing Prefix1..PrefixN (Music1, Music2...), moving on whenever a track "
          "ends. Shuffled, a track never plays twice in a row.",
          kPlaylistIn, Count(kPlaylistIn), kExecSuccessOut, Count(kExecSuccessOut),
          kPlaylistParams, Count(kPlaylistParams) },
        { "Audio.StopMusic", "Stop Music", "Audio", "Stops the music channel and any playlist.",
          kExecIn, Count(kExecIn), kExecOut, Count(kExecOut), nullptr, 0 },
        { "Audio.IsMusicPlaying", "Is Music Playing", "Audio", "True while the music channel has a track.",
          nullptr, 0, kMusicPlayingOut, Count(kMusicPlayingOut), nullptr, 0 },

        { "Game.Stop", "Stop Game", "Game", "Leaves play mode, as Escape does.",
          kExecIn, Count(kExecIn), nullptr, 0, nullptr, 0 },
        { "Game.ToggleFullscreen", "Toggle Fullscreen", "Game", "Switches the game window in or out of fullscreen.",
          kExecIn, Count(kExecIn), kExecOut, Count(kExecOut), nullptr, 0 },
        { "Game.IsStandalone", "Is Standalone", "Game",
          "True in the packaged game, false when playing from the editor - for editor-only debug UI.",
          nullptr, 0, kStandaloneOut, Count(kStandaloneOut), nullptr, 0 },
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
        case NodePinKind::Entity: return "entity";
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
        case NodePinKind::Entity: return "Entity";
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
            if (std::strcmp(id, "entity") == 0) return NodePinKind::Entity;
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
        case NodePinKind::Entity: return 0x4a90e2;
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
