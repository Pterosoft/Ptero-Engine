#include "System/NodeGraphRuntime.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace
{
    // One exec step gets this many node executions. A gate wired back into itself is a
    // mistake nothing can detect statically, and a frozen editor is a much worse failure
    // than a graph that stops early and says so.
    constexpr int kMaxStepsPerExecution = 20000;
    // How many exec hops one chain may nest. Both execution and value evaluation recurse,
    // so this is a stack limit, not a work limit - the step budget above is far too large
    // to be reached before the stack would run out. Real graphs chain a handful of nodes
    // deep; anything near this is a loop that will not terminate on its own.
    constexpr int kMaxExecDepth = 192;
    constexpr int kMaxValueDepth = 128;
    constexpr int kMaxLogLines = 200;
    // Separate from the step budget: a For Loop with a wild Last value should be cut off
    // at the loop rather than burning the whole budget on one node.
    constexpr int kMaxLoopIterations = 10000;

    // Every early return out of ExecuteNode has to unwind the depth counter, and there
    // are a dozen of them.
    class DepthGuard
    {
    public:
        explicit DepthGuard(int& depth)
            : mDepth(depth)
        {
            ++mDepth;
        }

        ~DepthGuard() { --mDepth; }

        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;

    private:
        int& mDepth;
    };

    bool LooksNumeric(const std::string& text)
    {
        if (text.empty())
        {
            return false;
        }

        char* end = nullptr;
        std::strtod(text.c_str(), &end);
        return end != nullptr && *end == '\0';
    }

    std::string TrimTrailingZeroes(std::string text)
    {
        if (text.find('.') == std::string::npos)
        {
            return text;
        }

        while (!text.empty() && text.back() == '0')
        {
            text.pop_back();
        }

        if (!text.empty() && text.back() == '.')
        {
            text.pop_back();
        }

        return text.empty() ? "0" : text;
    }
}

NodeGraphValue NodeGraphValue::FromBool(bool value)
{
    NodeGraphValue result;
    result.ValueKind = Kind::Bool;
    result.Boolean = value;
    result.Number = value ? 1.0 : 0.0;
    return result;
}

NodeGraphValue NodeGraphValue::FromNumber(double value)
{
    NodeGraphValue result;
    result.ValueKind = Kind::Number;
    result.Number = value;
    result.Boolean = value != 0.0;
    return result;
}

NodeGraphValue NodeGraphValue::FromString(std::string value)
{
    NodeGraphValue result;
    result.ValueKind = Kind::String;
    result.Text = std::move(value);
    return result;
}

NodeGraphValue NodeGraphValue::Parse(const std::string& text, NodePinKind kind)
{
    switch (kind)
    {
    case NodePinKind::Bool:
        return FromBool(text == "true" || text == "1");
    case NodePinKind::Number:
        return FromNumber(text.empty() ? 0.0 : std::strtod(text.c_str(), nullptr));
    case NodePinKind::String:
        return FromString(text);
    default:
        break;
    }

    // Wildcard pins have no declared type, so the literal decides: this is what lets a
    // variable node accept "true", "3.5" or "menu.rml" from the same text field.
    if (text == "true" || text == "false")
    {
        return FromBool(text == "true");
    }

    if (LooksNumeric(text))
    {
        return FromNumber(std::strtod(text.c_str(), nullptr));
    }

    return FromString(text);
}

NodeGraphValue NodeGraphValue::Parse(const std::string& text, NodeGraphVariableType type)
{
    return Parse(text, NodeGraphVariablePinKind(type)).CoerceTo(type);
}

NodeGraphValue NodeGraphValue::CoerceTo(NodeGraphVariableType type) const
{
    switch (type)
    {
    case NodeGraphVariableType::Bool:
        return FromBool(AsBool());
    case NodeGraphVariableType::Int:
        // Truncate rather than round: an Int counter fed by a division should behave the
        // way the same expression would in C++.
        return FromNumber(static_cast<double>(static_cast<long long>(AsNumber())));
    case NodeGraphVariableType::Float:
        return FromNumber(static_cast<double>(static_cast<float>(AsNumber())));
    case NodeGraphVariableType::Double:
        return FromNumber(AsNumber());
    case NodeGraphVariableType::String:
    case NodeGraphVariableType::Text:
        return FromString(AsString());
    }

    return *this;
}

bool NodeGraphValue::AsBool() const
{
    switch (ValueKind)
    {
    case Kind::Bool: return Boolean;
    case Kind::Number: return Number != 0.0;
    case Kind::String: return !Text.empty() && Text != "false" && Text != "0";
    }

    return false;
}

double NodeGraphValue::AsNumber() const
{
    switch (ValueKind)
    {
    case Kind::Bool: return Boolean ? 1.0 : 0.0;
    case Kind::Number: return Number;
    case Kind::String: return Text.empty() ? 0.0 : std::strtod(Text.c_str(), nullptr);
    }

    return 0.0;
}

std::string NodeGraphValue::AsString() const
{
    switch (ValueKind)
    {
    case Kind::Bool:
        return Boolean ? "true" : "false";
    case Kind::Number:
    {
        std::ostringstream builder;
        builder.precision(6);
        builder << std::fixed << Number;
        return TrimTrailingZeroes(builder.str());
    }
    case Kind::String:
        return Text;
    }

    return std::string();
}

long long NodeGraphRuntime::PortKey(int nodeIndex, int portIndex)
{
    return (static_cast<long long>(nodeIndex) << 32) | static_cast<unsigned>(portIndex);
}

void NodeGraphRuntime::Start(const NodeGraphDocument& document)
{
    Stop();

    mDocument = document;
    mPlayTimeSeconds = 0.0;
    mDeltaSeconds = 0.0f;
    mStopRequested = false;
    mLog.clear();

    BuildIndex();
    ResetVariables();

    mIsRunning = true;

    if (mNodes.empty())
    {
        return;
    }

    FireEvents("Event.GameStart");
}

void NodeGraphRuntime::Tick(float deltaSeconds)
{
    if (!mIsRunning)
    {
        return;
    }

    mDeltaSeconds = deltaSeconds;
    mPlayTimeSeconds += deltaSeconds;

    // Delays resume before this frame's tick events so a graph that delays by zero-ish
    // amounts stays in step with the frame it was scheduled from.
    for (std::size_t i = 0; i < mNodes.size(); ++i)
    {
        NodeState& state = mNodes[i].State;
        if (!state.DelayActive)
        {
            continue;
        }

        state.DelayRemaining -= static_cast<double>(deltaSeconds);
        if (state.DelayRemaining <= 0.0)
        {
            state.DelayActive = false;
            mStepsRemaining = kMaxStepsPerExecution;
            mExecDepth = 0;
            FireExec(static_cast<int>(i), 0);
        }
    }

    FireEvents("Event.Tick");
}

void NodeGraphRuntime::Stop()
{
    if (mIsRunning)
    {
        FireEvents("Event.GameStop");
    }

    mIsRunning = false;
    mNodes.clear();
    mIndexById.clear();
    mExecTargets.clear();
    mDataSources.clear();
    mVariables.clear();
    mFunctions.clear();
    mValueCache.clear();
    mEvaluationStack.clear();
    mDocument.Clear();
    mStopRequested = false;
}

bool NodeGraphRuntime::ConsumeStopRequest()
{
    const bool requested = mStopRequested;
    mStopRequested = false;
    return requested;
}

void NodeGraphRuntime::BuildIndex()
{
    mNodes.clear();
    mIndexById.clear();
    mExecTargets.clear();
    mDataSources.clear();
    mFunctions.clear();
    mValueCache.clear();
    mEvaluationStack.clear();

    mNodes.reserve(mDocument.Nodes.size());
    for (const NodeGraphNode& node : mDocument.Nodes)
    {
        const NodeGraphNodeType* type = NodeGraphCatalog::Find(node.TypeId.c_str());
        if (type == nullptr)
        {
            Log("Skipped unknown node type '" + node.TypeId + "'.");
            continue;
        }

        mIndexById[node.Id] = static_cast<int>(mNodes.size());

        RuntimeNode runtimeNode;
        runtimeNode.Type = type;
        runtimeNode.Data = &node;
        mNodes.push_back(runtimeNode);
    }

    // Nodes hold state that depends on their parameters, so seed it once the whole set
    // is known rather than while the vector is still growing.
    for (std::size_t i = 0; i < mNodes.size(); ++i)
    {
        RuntimeNode& runtimeNode = mNodes[i];
        const std::string& typeId = runtimeNode.Data->TypeId;

        if (typeId == "Function.Entry")
        {
            const std::string* name = runtimeNode.Data->FindParam("Function");
            if (name == nullptr || name->empty())
            {
                Log("A Function Entry has no name and can never be called.");
            }
            else if (!mFunctions.emplace(*name, static_cast<int>(i)).second)
            {
                Log("Two Function Entry nodes are both called '" + *name + "'; the first one wins.");
            }
        }

        if (typeId == "Flow.Gate")
        {
            const std::string* startClosed = runtimeNode.Data->FindParam("StartClosed");
            runtimeNode.State.Flag = startClosed == nullptr || *startClosed != "true";
        }
        else if (typeId == "Flow.DoOnce")
        {
            const std::string* startClosed = runtimeNode.Data->FindParam("StartClosed");
            runtimeNode.State.Flag = startClosed != nullptr && *startClosed == "true";
        }
    }

    for (const NodeGraphConnection& connection : mDocument.Connections)
    {
        const auto fromIt = mIndexById.find(connection.FromNode);
        const auto toIt = mIndexById.find(connection.ToNode);
        if (fromIt == mIndexById.end() || toIt == mIndexById.end())
        {
            continue;
        }

        const RuntimeNode& fromNode = mNodes[fromIt->second];
        const RuntimeNode& toNode = mNodes[toIt->second];
        if (connection.FromPort < 0 || static_cast<unsigned>(connection.FromPort) >= fromNode.Type->OutputCount ||
            connection.ToPort < 0 || static_cast<unsigned>(connection.ToPort) >= toNode.Type->InputCount)
        {
            continue;
        }

        DataSource target;
        target.NodeIndex = toIt->second;
        target.PortIndex = connection.ToPort;

        if (fromNode.Type->Outputs[connection.FromPort].Kind == NodePinKind::Exec)
        {
            mExecTargets[PortKey(fromIt->second, connection.FromPort)].push_back(target);
        }
        else
        {
            DataSource source;
            source.NodeIndex = fromIt->second;
            source.PortIndex = connection.FromPort;
            mDataSources[PortKey(toIt->second, connection.ToPort)] = source;
        }
    }
}

void NodeGraphRuntime::ResetVariables()
{
    mVariables.clear();
    for (const NodeGraphVariable& variable : mDocument.Variables)
    {
        mVariables[variable.Name] = NodeGraphValue::Parse(variable.DefaultValue, variable.Type);
    }
}

void NodeGraphRuntime::FireEvents(const char* eventTypeId)
{
    for (std::size_t i = 0; i < mNodes.size(); ++i)
    {
        if (mNodes[i].Data->TypeId != eventTypeId)
        {
            continue;
        }

        mStepsRemaining = kMaxStepsPerExecution;
        mExecDepth = 0;
        FireExec(static_cast<int>(i), 0);
    }
}

void NodeGraphRuntime::FireExec(int nodeIndex, int outPortIndex)
{
    const auto it = mExecTargets.find(PortKey(nodeIndex, outPortIndex));
    if (it == mExecTargets.end())
    {
        return;
    }

    // Copy: executing a target may not change the graph today, but iterating a container
    // that a future node type could touch is not worth the risk.
    const std::vector<DataSource> targets = it->second;
    for (const DataSource& target : targets)
    {
        ExecuteNode(target.NodeIndex, target.PortIndex);
    }
}

void NodeGraphRuntime::ExecuteNode(int nodeIndex, int inPortIndex)
{
    if (nodeIndex < 0 || static_cast<std::size_t>(nodeIndex) >= mNodes.size())
    {
        return;
    }

    if (--mStepsRemaining < 0)
    {
        if (mStepsRemaining == -1)
        {
            Log("Execution budget exhausted - the graph probably contains a loop that never ends.");
        }

        return;
    }

    if (mExecDepth >= kMaxExecDepth)
    {
        Log("Execution nested more than " + std::to_string(kMaxExecDepth) +
            " nodes deep - stopping this chain before it overflows the stack.");
        return;
    }

    const DepthGuard depthGuard(mExecDepth);

    RuntimeNode& node = mNodes[nodeIndex];
    NodeState& state = node.State;
    const std::string& typeId = node.Data->TypeId;

    // Values are pulled fresh for every node execution; the cache only spares repeated
    // work inside this one node's inputs.
    mValueCache.clear();

    if (typeId == "Flow.Branch")
    {
        FireExec(nodeIndex, ReadInput(nodeIndex, 1).AsBool() ? 0 : 1);
        return;
    }

    if (typeId == "Flow.Delay")
    {
        if (state.DelayActive)
        {
            return;
        }

        const double duration = ReadInput(nodeIndex, 1).AsNumber();
        if (duration <= 0.0)
        {
            FireExec(nodeIndex, 0);
            return;
        }

        state.DelayActive = true;
        state.DelayRemaining = duration;
        return;
    }

    if (typeId == "Flow.Gate")
    {
        switch (inPortIndex)
        {
        case 1: state.Flag = true; return;
        case 2: state.Flag = false; return;
        case 3: state.Flag = !state.Flag; return;
        default: break;
        }

        if (state.Flag)
        {
            FireExec(nodeIndex, 0);
        }

        return;
    }

    if (typeId == "Flow.Sequence")
    {
        for (unsigned port = 0; port < node.Type->OutputCount; ++port)
        {
            FireExec(nodeIndex, static_cast<int>(port));
        }

        return;
    }

    if (typeId == "Flow.DoOnce")
    {
        if (inPortIndex == 1)
        {
            state.Flag = false;
            return;
        }

        if (!state.Flag)
        {
            state.Flag = true;
            FireExec(nodeIndex, 0);
        }

        return;
    }

    if (typeId == "Flow.DoN")
    {
        if (inPortIndex == 1)
        {
            state.Counter = 0;
            return;
        }

        const int limit = static_cast<int>(ReadInput(nodeIndex, 2).AsNumber());
        if (state.Counter < limit)
        {
            ++state.Counter;
            FireExec(nodeIndex, 0);
        }

        return;
    }

    if (typeId == "Flow.FlipFlop")
    {
        state.Flag = !state.Flag;
        FireExec(nodeIndex, state.Flag ? 0 : 1);
        return;
    }

    if (typeId == "Flow.ForLoop")
    {
        const int first = static_cast<int>(ReadInput(nodeIndex, 1).AsNumber());
        const int last = static_cast<int>(ReadInput(nodeIndex, 2).AsNumber());

        int iterations = 0;
        for (int index = first; index <= last; ++index)
        {
            if (++iterations > kMaxLoopIterations)
            {
                Log("For Loop stopped after " + std::to_string(kMaxLoopIterations) + " iterations.");
                break;
            }

            state.Counter = index;
            FireExec(nodeIndex, 0);

            if (mStepsRemaining < 0)
            {
                break;
            }
        }

        FireExec(nodeIndex, 2);
        return;
    }

    if (typeId == "Flow.Switch")
    {
        const int index = static_cast<int>(ReadInput(nodeIndex, 1).AsNumber());
        const int caseCount = static_cast<int>(node.Type->OutputCount) - 1;
        FireExec(nodeIndex, (index >= 0 && index < caseCount) ? index : caseCount);
        return;
    }

    if (typeId == "Variable.Set")
    {
        const std::string* name = node.Data->FindParam("Variable");
        NodeGraphValue value = ReadInput(nodeIndex, 1);

        if (name != nullptr && !name->empty())
        {
            if (NodeGraphValue* slot = FindVariableSlot(*name))
            {
                // Coerce to the declared type so a variable never changes shape
                // depending on which branch happened to write it.
                if (const NodeGraphVariable* declaration = mDocument.FindVariable(*name))
                {
                    value = value.CoerceTo(declaration->Type);
                }

                *slot = value;
            }
            else
            {
                Log("Set Variable: '" + *name + "' is not declared in this graph.");
            }
        }

        state.LastValue = value;
        FireExec(nodeIndex, 0);
        return;
    }

    if (typeId == "Function.Call")
    {
        const std::string* name = node.Data->FindParam("Function");
        if (name == nullptr || name->empty())
        {
            Log("Call Function has no function selected.");
        }
        else
        {
            const auto entry = mFunctions.find(*name);
            if (entry == mFunctions.end())
            {
                Log("Call Function: '" + *name + "' is not defined in this graph.");
            }
            else
            {
                // The body runs to completion before the caller continues, so a Return -
                // which fires nothing - simply ends it and control lands back here. The
                // depth guard in this function is what stops a function that calls itself.
                FireExec(entry->second, 0);
            }
        }

        FireExec(nodeIndex, 0);
        return;
    }

    // Function.Return needs no case: it has no outputs, so the generic fall-through at the
    // end of this function fires nothing and the chain ends, which is exactly a return.

    if (typeId == "Debug.Print")
    {
        Log(ReadInput(nodeIndex, 1).AsString());
        FireExec(nodeIndex, 0);
        return;
    }

    if (typeId == "Game.Stop")
    {
        mStopRequested = true;
        return;
    }

    if (typeId.compare(0, 3, "UI.") == 0)
    {
        state.Success = false;

        if (mHost == nullptr)
        {
            Log("UI node ran with no host attached.");
        }
        else if (typeId == "UI.ShowDocument")
        {
            state.Success = mHost->ShowUiDocument(ReadInput(nodeIndex, 1).AsString());
        }
        else if (typeId == "UI.CloseDocument")
        {
            mHost->CloseUiDocument();
            state.Success = true;
        }
        else if (typeId == "UI.ReloadDocument")
        {
            state.Success = mHost->ReloadUiDocument();
        }
        else if (typeId == "UI.SetVisible")
        {
            mHost->SetUiVisible(ReadInput(nodeIndex, 1).AsBool());
            state.Success = true;
        }
        else if (typeId == "UI.SetInputEnabled")
        {
            mHost->SetUiInputEnabled(ReadInput(nodeIndex, 1).AsBool());
            state.Success = true;
        }
        else if (typeId == "UI.SetText")
        {
            state.Success = mHost->SetUiElementText(
                ReadInput(nodeIndex, 1).AsString(),
                ReadInput(nodeIndex, 2).AsString());
        }
        else if (typeId == "UI.SetProperty")
        {
            state.Success = mHost->SetUiElementProperty(
                ReadInput(nodeIndex, 1).AsString(),
                ReadInput(nodeIndex, 2).AsString(),
                ReadInput(nodeIndex, 3).AsString());
        }
        else if (typeId == "UI.SetClass")
        {
            state.Success = mHost->SetUiElementClass(
                ReadInput(nodeIndex, 1).AsString(),
                ReadInput(nodeIndex, 2).AsString(),
                ReadInput(nodeIndex, 3).AsBool());
        }
        else if (typeId == "UI.SetElementVisible")
        {
            state.Success = mHost->SetUiElementVisible(
                ReadInput(nodeIndex, 1).AsString(),
                ReadInput(nodeIndex, 2).AsBool());
        }

        FireExec(nodeIndex, 0);
        return;
    }

    // Anything else with an exec input simply passes control on, which keeps a node that
    // was added to the catalogue but not yet taught to the runtime from breaking a graph.
    if (node.Type->OutputCount > 0 && node.Type->Outputs[0].Kind == NodePinKind::Exec)
    {
        FireExec(nodeIndex, 0);
    }
}

NodeGraphValue NodeGraphRuntime::ReadInput(int nodeIndex, int inPortIndex)
{
    if (nodeIndex < 0 || static_cast<std::size_t>(nodeIndex) >= mNodes.size())
    {
        return NodeGraphValue::FromNumber(0.0);
    }

    const RuntimeNode& node = mNodes[nodeIndex];
    if (inPortIndex < 0 || static_cast<unsigned>(inPortIndex) >= node.Type->InputCount)
    {
        return NodeGraphValue::FromNumber(0.0);
    }

    const NodeGraphPin& pin = node.Type->Inputs[inPortIndex];

    const auto it = mDataSources.find(PortKey(nodeIndex, inPortIndex));
    if (it != mDataSources.end())
    {
        return EvaluateOutput(it->second.NodeIndex, it->second.PortIndex);
    }

    return ParamValue(nodeIndex, pin.Name, pin.Kind);
}

NodeGraphValue NodeGraphRuntime::ParamValue(int nodeIndex, const char* key, NodePinKind kind) const
{
    const RuntimeNode& node = mNodes[nodeIndex];

    if (const std::string* stored = node.Data->FindParam(key))
    {
        return NodeGraphValue::Parse(*stored, kind);
    }

    if (const NodeGraphParam* declared = NodeGraphCatalog::FindParam(*node.Type, key))
    {
        return NodeGraphValue::Parse(declared->DefaultValue, kind);
    }

    return kind == NodePinKind::String ? NodeGraphValue::FromString(std::string())
                                       : NodeGraphValue::FromNumber(0.0);
}

NodeGraphValue NodeGraphRuntime::EvaluateOutput(int nodeIndex, int outPortIndex)
{
    if (nodeIndex < 0 || static_cast<std::size_t>(nodeIndex) >= mNodes.size())
    {
        return NodeGraphValue::FromNumber(0.0);
    }

    const long long cacheKey = PortKey(nodeIndex, outPortIndex);
    const auto cached = mValueCache.find(cacheKey);
    if (cached != mValueCache.end())
    {
        return cached->second;
    }

    if (std::find(mEvaluationStack.begin(), mEvaluationStack.end(), nodeIndex) != mEvaluationStack.end())
    {
        Log("Value connections form a cycle; reading zero to break it.");
        return NodeGraphValue::FromNumber(0.0);
    }

    // The cycle check already bounds this by the node count, but a very long value chain
    // in a large graph would still recurse further than the stack can take.
    if (mEvaluationStack.size() >= static_cast<std::size_t>(kMaxValueDepth))
    {
        Log("Value connections nested more than " + std::to_string(kMaxValueDepth) +
            " deep; reading zero.");
        return NodeGraphValue::FromNumber(0.0);
    }

    mEvaluationStack.push_back(nodeIndex);

    RuntimeNode& node = mNodes[nodeIndex];
    const std::string& typeId = node.Data->TypeId;
    NodeGraphValue result = NodeGraphValue::FromNumber(0.0);

    if (typeId == "Value.Number" || typeId == "Value.Bool" || typeId == "Value.String")
    {
        result = ParamValue(nodeIndex, "Value", node.Type->Outputs[0].Kind);
    }
    else if (typeId == "Value.Append")
    {
        result = NodeGraphValue::FromString(
            ReadInput(nodeIndex, 0).AsString() + ReadInput(nodeIndex, 1).AsString());
    }
    else if (typeId == "Value.ToString")
    {
        result = NodeGraphValue::FromString(ReadInput(nodeIndex, 0).AsString());
    }
    else if (typeId == "Math.Add")
    {
        result = NodeGraphValue::FromNumber(ReadInput(nodeIndex, 0).AsNumber() + ReadInput(nodeIndex, 1).AsNumber());
    }
    else if (typeId == "Math.Subtract")
    {
        result = NodeGraphValue::FromNumber(ReadInput(nodeIndex, 0).AsNumber() - ReadInput(nodeIndex, 1).AsNumber());
    }
    else if (typeId == "Math.Multiply")
    {
        result = NodeGraphValue::FromNumber(ReadInput(nodeIndex, 0).AsNumber() * ReadInput(nodeIndex, 1).AsNumber());
    }
    else if (typeId == "Math.Divide")
    {
        const double divisor = ReadInput(nodeIndex, 1).AsNumber();
        result = NodeGraphValue::FromNumber(divisor == 0.0 ? 0.0 : ReadInput(nodeIndex, 0).AsNumber() / divisor);
    }
    else if (typeId == "Math.Clamp")
    {
        const double value = ReadInput(nodeIndex, 0).AsNumber();
        const double low = ReadInput(nodeIndex, 1).AsNumber();
        const double high = ReadInput(nodeIndex, 2).AsNumber();
        result = NodeGraphValue::FromNumber(low <= high ? (std::min)((std::max)(value, low), high) : value);
    }
    else if (typeId == "Math.Lerp")
    {
        const double a = ReadInput(nodeIndex, 0).AsNumber();
        const double b = ReadInput(nodeIndex, 1).AsNumber();
        const double alpha = ReadInput(nodeIndex, 2).AsNumber();
        result = NodeGraphValue::FromNumber(a + (b - a) * alpha);
    }
    else if (typeId == "Math.RandomRange")
    {
        const double low = ReadInput(nodeIndex, 0).AsNumber();
        const double high = ReadInput(nodeIndex, 1).AsNumber();
        mRandomState = mRandomState * 1664525u + 1013904223u;
        const double unit = static_cast<double>(mRandomState >> 8) / static_cast<double>(1u << 24);
        result = NodeGraphValue::FromNumber(low + (high - low) * unit);
    }
    else if (typeId == "Logic.And")
    {
        result = NodeGraphValue::FromBool(ReadInput(nodeIndex, 0).AsBool() && ReadInput(nodeIndex, 1).AsBool());
    }
    else if (typeId == "Logic.Or")
    {
        result = NodeGraphValue::FromBool(ReadInput(nodeIndex, 0).AsBool() || ReadInput(nodeIndex, 1).AsBool());
    }
    else if (typeId == "Logic.Not")
    {
        result = NodeGraphValue::FromBool(!ReadInput(nodeIndex, 0).AsBool());
    }
    else if (typeId == "Logic.Compare")
    {
        const double a = ReadInput(nodeIndex, 0).AsNumber();
        const double b = ReadInput(nodeIndex, 1).AsNumber();
        const std::string* op = node.Data->FindParam("Operator");
        const std::string comparison = op != nullptr ? *op : std::string("==");

        bool outcome = false;
        if (comparison == "==") outcome = a == b;
        else if (comparison == "!=") outcome = a != b;
        else if (comparison == ">") outcome = a > b;
        else if (comparison == ">=") outcome = a >= b;
        else if (comparison == "<") outcome = a < b;
        else if (comparison == "<=") outcome = a <= b;

        result = NodeGraphValue::FromBool(outcome);
    }
    else if (typeId == "Logic.Multiplexer")
    {
        const int selected = static_cast<int>(ReadInput(nodeIndex, 0).AsNumber());
        const int inputCount = static_cast<int>(node.Type->InputCount) - 1;
        const int clamped = (selected >= 0 && selected < inputCount) ? selected : 0;
        result = ReadInput(nodeIndex, clamped + 1);
    }
    else if (typeId == "Variable.Get")
    {
        const std::string* name = node.Data->FindParam("Variable");
        if (name != nullptr && !name->empty())
        {
            if (const NodeGraphValue* slot = FindVariableSlot(*name))
            {
                result = *slot;
            }
            else
            {
                Log("Get Variable: '" + *name + "' is not declared in this graph.");
            }
        }
    }
    else if (typeId == "Variable.Set")
    {
        result = node.State.LastValue;
    }
    else if (typeId == "Event.Tick")
    {
        result = NodeGraphValue::FromNumber(static_cast<double>(mDeltaSeconds));
    }
    else if (typeId == "Game.GetPlayTime")
    {
        result = NodeGraphValue::FromNumber(mPlayTimeSeconds);
    }
    else if (typeId == "Flow.DoN" || typeId == "Flow.ForLoop")
    {
        result = NodeGraphValue::FromNumber(static_cast<double>(node.State.Counter));
    }
    else if (typeId == "Flow.FlipFlop")
    {
        result = NodeGraphValue::FromBool(node.State.Flag);
    }
    else if (typeId.compare(0, 3, "UI.") == 0)
    {
        result = NodeGraphValue::FromBool(node.State.Success);
    }

    mEvaluationStack.pop_back();
    mValueCache[cacheKey] = result;
    return result;
}

NodeGraphValue* NodeGraphRuntime::FindVariableSlot(const std::string& name)
{
    const auto it = mVariables.find(name);
    return it == mVariables.end() ? nullptr : &it->second;
}

void NodeGraphRuntime::Log(std::string message)
{
    OutputDebugStringA(("[NodeGraph] " + message + "\n").c_str());

    mLog.push_back(std::move(message));
    while (mLog.size() > kMaxLogLines)
    {
        mLog.pop_front();
    }
}
