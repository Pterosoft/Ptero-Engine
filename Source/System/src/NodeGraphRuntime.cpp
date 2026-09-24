#include "System/NodeGraphRuntime.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <utility>

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

    constexpr double kPi = 3.14159265358979323846;
    constexpr double kDegreesToRadians = kPi / 180.0;
    constexpr double kRadiansToDegrees = 180.0 / kPi;
    // A frame's worth of clicks. The UI queues at most this many anyway.
    constexpr int kMaxUiClicksPerTick = 32;

    // Reshapes a 0..1 progress value. Names match the catalogue's easing list.
    double ApplyEasing(double t, const std::string& easing)
    {
        t = (std::min)((std::max)(t, 0.0), 1.0);
        if (easing == "Linear") return t;
        if (easing == "Smooth") return t * t * (3.0 - 2.0 * t);
        if (easing == "Ease In") return t * t;
        if (easing == "Ease Out") return 1.0 - (1.0 - t) * (1.0 - t);
        if (easing == "Ease In Out")
            return t < 0.5 ? 2.0 * t * t : 1.0 - (-2.0 * t + 2.0) * (-2.0 * t + 2.0) * 0.5;
        // Smoother, and anything unrecognised: zero velocity and acceleration at both
        // ends, which is what keeps a camera move from starting or stopping with a jolt.
        return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
    }

    // Names from the catalogue's key list to Win32 virtual-key codes; 0 when unknown.
    int VirtualKeyFromName(const std::string& name)
    {
        if (name.size() == 1)
        {
            const char c = name[0];
            if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
                return c;
        }

        if (name.size() >= 2 && name.size() <= 3 && name[0] == 'F')
        {
            const int number = std::atoi(name.c_str() + 1);
            if (number >= 1 && number <= 12)
                return VK_F1 + number - 1;
        }

        static const std::pair<const char*, int> kNamed[] = {
            { "Space", VK_SPACE }, { "Enter", VK_RETURN }, { "Escape", VK_ESCAPE }, { "Tab", VK_TAB },
            { "Backspace", VK_BACK }, { "Up", VK_UP }, { "Down", VK_DOWN }, { "Left", VK_LEFT },
            { "Right", VK_RIGHT }, { "Shift", VK_SHIFT }, { "Ctrl", VK_CONTROL }, { "Alt", VK_MENU },
            { "Left Mouse", VK_LBUTTON }, { "Right Mouse", VK_RBUTTON }, { "Middle Mouse", VK_MBUTTON }
        };
        for (const auto& [label, key] : kNamed)
        {
            if (name == label)
                return key;
        }

        return 0;
    }

    // "1234567.891" -> "1,234,567.89" with two decimals and grouping on.
    std::string FormatNumber(double value, int decimals, bool grouping)
    {
        decimals = (std::min)((std::max)(decimals, 0), 6);
        char buffer[64] = {};
        std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, value);
        std::string text = buffer;
        if (!grouping)
            return text;

        const std::size_t start = (!text.empty() && text[0] == '-') ? 1 : 0;
        std::size_t integerEnd = text.find('.');
        if (integerEnd == std::string::npos)
            integerEnd = text.size();

        for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(integerEnd) - 3;
             i > static_cast<std::ptrdiff_t>(start); i -= 3)
        {
            text.insert(static_cast<std::size_t>(i), ",");
        }

        return text;
    }

    std::uint64_t EntityIdFromValue(const NodeGraphValue& value)
    {
        const double number = value.AsNumber();
        return number >= 1.0 ? static_cast<std::uint64_t>(number) : 0;
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
    case NodePinKind::Entity:
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
    mCameraHeld = false;
    mPlaylistActive = false;
    mPlaylistTrack = -1;

    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    mRandomState = static_cast<unsigned>(counter.QuadPart ^ (counter.QuadPart >> 32)) | 1u;

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

    // The host advanced the video before this tick, so an end reached this frame is
    // reported in the same frame.
    if (mHost != nullptr && mHost->ConsumeVideoFinished())
    {
        FireEvents("Event.VideoFinished");
    }

    FireInputEvents();

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
            FireEntry(static_cast<int>(i), 0);
        }
    }

    // Tweens advance with the delays, before Tick, so a graph reading an entity's
    // position on Tick sees where it will be drawn this frame.
    AdvanceTweens(deltaSeconds);

    FireEvents("Event.Tick");

    UpdatePlaylist();

    // Last, so a held camera wins over the pose the game module wrote this frame.
    if (mCameraHeld && mHost != nullptr)
    {
        mHost->SetCamera(mCameraPose);
    }
}

void NodeGraphRuntime::FireEntry(int nodeIndex, int outPortIndex)
{
    mStepsRemaining = kMaxStepsPerExecution;
    mExecDepth = 0;
    FireExec(nodeIndex, outPortIndex);
}

void NodeGraphRuntime::FireInputEvents()
{
    if (mHost == nullptr)
    {
        return;
    }

    // Drain first and fire afterwards: a handler that loads another document clears the
    // UI's queue, and the clicks already taken must still be delivered.
    std::vector<std::string> clicks;
    std::string elementId;
    while (static_cast<int>(clicks.size()) < kMaxUiClicksPerTick && mHost->PollUiClick(elementId))
    {
        clicks.push_back(elementId);
    }

    for (const std::string& clicked : clicks)
    {
        for (std::size_t i = 0; i < mNodes.size(); ++i)
        {
            if (mNodes[i].Data->TypeId != "Event.UiButtonClicked")
            {
                continue;
            }

            const std::string* filter = mNodes[i].Data->FindParam("Element Id");
            if (filter != nullptr && !filter->empty() && *filter != clicked)
            {
                continue;
            }

            mNodes[i].State.LastValue = NodeGraphValue::FromString(clicked);
            FireEntry(static_cast<int>(i), 0);
        }
    }

    for (std::size_t i = 0; i < mNodes.size(); ++i)
    {
        const std::string& typeId = mNodes[i].Data->TypeId;
        const bool pressed = typeId == "Event.KeyPressed";
        if (!pressed && typeId != "Event.KeyReleased")
        {
            continue;
        }

        const int key = VirtualKeyFromName(ParamValue(static_cast<int>(i), "Key", NodePinKind::String).AsString());
        NodeState& state = mNodes[i].State;
        const bool down = key != 0 && mHost->IsKeyDown(key);
        const bool wasDown = state.KeyWasDown;
        state.KeyWasDown = down;

        if (pressed ? (down && !wasDown) : (!down && wasDown))
        {
            FireEntry(static_cast<int>(i), 0);
        }
    }
}

void NodeGraphRuntime::AdvanceTweens(float deltaSeconds)
{
    for (std::size_t i = 0; i < mNodes.size(); ++i)
    {
        NodeState& state = mNodes[i].State;
        if (!state.TweenActive)
        {
            continue;
        }

        const int nodeIndex = static_cast<int>(i);
        const bool camera = mNodes[i].Data->TypeId == "Camera.MoveTo";
        state.TweenElapsed += static_cast<double>(deltaSeconds);
        const double t = state.TweenDuration > 0.0 ? (std::min)(state.TweenElapsed / state.TweenDuration, 1.0) : 1.0;
        const double eased = ApplyEasing(t, ParamValue(nodeIndex, "Easing", NodePinKind::String).AsString());

        double pose[5]{};
        for (int a = 0; a < 5; ++a)
        {
            pose[a] = state.TweenFrom[a] + (state.TweenTo[a] - state.TweenFrom[a]) * eased;
        }

        if (camera)
        {
            // Pitch/yaw were unwrapped when the move began (To = From + shortest delta),
            // so a straight blend already turns the short way round.
            mCameraHeld = true;
            mCameraPose.Position[0] = pose[0];
            mCameraPose.Position[1] = pose[1];
            mCameraPose.Position[2] = pose[2];
            mCameraPose.Pitch = pose[3];
            // Wrapped back into -180..180 so the unwrapped target (350 -> 370, say) never
            // leaks out through Get Camera or into the engine camera.
            mCameraPose.Yaw = std::remainder(pose[4], 2.0 * kPi);
        }
        else if (mHost != nullptr)
        {
            // The arc runs on raw time, not the eased curve, so the peak stays mid-flight
            // whatever easing the horizontal motion uses.
            const double arc = ParamValue(nodeIndex, "Arc Height", NodePinKind::Number).AsNumber();
            pose[2] += arc * std::sin(kPi * t);

            NodeGraphTransform transform;
            if (!mHost->GetEntityTransform(state.TweenEntity, transform))
            {
                // The entity went away mid-move; there is nothing left to arrive.
                state.TweenActive = false;
                continue;
            }

            for (int a = 0; a < 3; ++a)
            {
                transform.Position[a] = pose[a];
            }
            mHost->SetEntityTransform(state.TweenEntity, transform);
        }

        if (t >= 1.0)
        {
            state.TweenActive = false;
            FireEntry(nodeIndex, 1);
        }
    }
}

void NodeGraphRuntime::UpdatePlaylist()
{
    if (!mPlaylistActive || mHost == nullptr || mHost->IsMusicPlaying())
    {
        return;
    }

    PlayNextPlaylistTrack();
}

void NodeGraphRuntime::PlayNextPlaylistTrack()
{
    if (mPlaylistCount <= 0)
    {
        mPlaylistActive = false;
        return;
    }

    int next = 0;
    if (!mPlaylistShuffle)
    {
        next = (mPlaylistTrack + 1) % mPlaylistCount;
    }
    else if (mPlaylistCount > 1 && mPlaylistTrack >= 0)
    {
        // Draw from the tracks other than the one that just ended, so nothing repeats
        // back to back: pick among N-1 and skip past the excluded index.
        next = static_cast<int>(NextRandom() * (mPlaylistCount - 1));
        if (next >= mPlaylistTrack)
        {
            ++next;
        }
    }
    else
    {
        next = static_cast<int>(NextRandom() * mPlaylistCount);
    }

    next = (std::min)(next, mPlaylistCount - 1);
    const std::string track = mPlaylistPrefix + std::to_string(next + 1);
    if (!mHost->PlayMusic(track))
    {
        // A missing track would otherwise be retried every frame forever.
        Log("Play Music Playlist: '" + track + "' could not be played; the playlist stopped.");
        mPlaylistActive = false;
        return;
    }

    mPlaylistTrack = next;
}

double NodeGraphRuntime::NextRandom()
{
    mRandomState = mRandomState * 1664525u + 1013904223u;
    return static_cast<double>(mRandomState >> 8) / static_cast<double>(1u << 24);
}

NodeGraphCameraPose NodeGraphRuntime::CurrentCamera()
{
    if (mCameraHeld || mHost == nullptr)
    {
        return mCameraPose;
    }

    return mHost->GetCamera();
}

std::uint64_t NodeGraphRuntime::ReadEntity(int nodeIndex)
{
    const RuntimeNode& node = mNodes[nodeIndex];
    const int pin = NodeGraphCatalog::IndexOfInput(*node.Type, "Entity");
    return EntityIdFromValue(pin >= 0 ? ReadInput(nodeIndex, pin)
                                      : ParamValue(nodeIndex, "Entity", NodePinKind::Entity));
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
    mCameraHeld = false;
    mPlaylistActive = false;
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

        FireEntry(static_cast<int>(i), 0);
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

    if (typeId.compare(0, 7, "Entity.") == 0)
    {
        ExecuteEntityNode(nodeIndex);
        return;
    }

    if (typeId == "Camera.Set" || typeId == "Camera.MoveTo" || typeId == "Camera.Release")
    {
        // Whichever camera node ran last owns the camera, so a move still in flight must
        // not carry on over a Set, a Release or a newer move.
        for (RuntimeNode& other : mNodes)
        {
            if (other.Data->TypeId == "Camera.MoveTo")
            {
                other.State.TweenActive = false;
            }
        }

        if (typeId == "Camera.Release")
        {
            mCameraHeld = false;
            FireExec(nodeIndex, 0);
            return;
        }

        NodeGraphCameraPose target;
        for (int a = 0; a < 3; ++a)
        {
            target.Position[a] = ReadInput(nodeIndex, 1 + a).AsNumber();
        }
        target.Pitch = ReadInput(nodeIndex, 4).AsNumber() * kDegreesToRadians;
        target.Yaw = ReadInput(nodeIndex, 5).AsNumber() * kDegreesToRadians;

        const double duration = typeId == "Camera.MoveTo" ? ReadInput(nodeIndex, 6).AsNumber() : 0.0;
        if (duration <= 0.0)
        {
            mCameraHeld = true;
            mCameraPose = target;
            if (mHost != nullptr)
            {
                mHost->SetCamera(mCameraPose);
            }

            FireExec(nodeIndex, 0);
            if (typeId == "Camera.MoveTo")
            {
                FireExec(nodeIndex, 1);
            }
            return;
        }

        const NodeGraphCameraPose from = CurrentCamera();
        state.TweenActive = true;
        state.TweenElapsed = 0.0;
        state.TweenDuration = duration;
        for (int a = 0; a < 3; ++a)
        {
            state.TweenFrom[a] = from.Position[a];
            state.TweenTo[a] = target.Position[a];
        }
        state.TweenFrom[3] = from.Pitch;
        state.TweenTo[3] = target.Pitch;
        state.TweenFrom[4] = from.Yaw;
        state.TweenTo[4] = from.Yaw + std::remainder(target.Yaw - from.Yaw, 2.0 * kPi);

        mCameraHeld = true;
        mCameraPose = from;
        FireExec(nodeIndex, 0);
        return;
    }

    if (typeId.compare(0, 6, "Audio.") == 0)
    {
        state.Success = false;

        if (mHost == nullptr)
        {
            Log("Audio node ran with no host attached.");
        }
        else if (typeId == "Audio.PlaySound")
        {
            mHost->PlayOneShot(ReadInput(nodeIndex, 1).AsString());
            state.Success = true;
        }
        else if (typeId == "Audio.PlayMusic")
        {
            mPlaylistActive = false;
            const std::string eventName = ReadInput(nodeIndex, 1).AsString();
            state.Success = mHost->PlayMusic(eventName);
            if (!state.Success)
            {
                Log("Play Music: '" + eventName + "' could not be played.");
            }
        }
        else if (typeId == "Audio.PlayPlaylist")
        {
            mPlaylistPrefix = ReadInput(nodeIndex, 1).AsString();
            mPlaylistCount = static_cast<int>(ReadInput(nodeIndex, 2).AsNumber());
            mPlaylistShuffle = ParamValue(nodeIndex, "Shuffle", NodePinKind::Bool).AsBool();
            mPlaylistTrack = -1;
            mPlaylistActive = true;
            PlayNextPlaylistTrack();
            state.Success = mPlaylistActive;
        }
        else if (typeId == "Audio.StopMusic")
        {
            mPlaylistActive = false;
            mHost->StopMusic();
            state.Success = true;
        }

        FireExec(nodeIndex, 0);
        return;
    }

    if (typeId == "Game.ToggleFullscreen")
    {
        if (mHost != nullptr)
        {
            mHost->ToggleFullscreen();
        }

        FireExec(nodeIndex, 0);
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

    if (typeId.compare(0, 6, "Video.") == 0)
    {
        state.Success = false;

        if (mHost == nullptr)
        {
            Log("Video node ran with no host attached.");
        }
        else if (typeId == "Video.Play")
        {
            const std::string fileName = ReadInput(nodeIndex, 1).AsString();
            // Volume before the video starts, so it never plays a frame at the old level.
            mHost->SetVideoVolume(ParamValue(nodeIndex, "Volume", NodePinKind::Number).AsNumber());
            state.Success = mHost->PlayVideo(
                fileName,
                ReadInput(nodeIndex, 2).AsBool(),
                ParamValue(nodeIndex, "Fit", NodePinKind::String).AsString());
            if (!state.Success)
            {
                Log("Play Video: '" + fileName + "' could not be played.");
            }
        }
        else if (typeId == "Video.Pause")
        {
            mHost->PauseVideo();
            state.Success = true;
        }
        else if (typeId == "Video.Resume")
        {
            mHost->ResumeVideo();
            state.Success = true;
        }
        else if (typeId == "Video.Stop")
        {
            mHost->StopVideo();
            state.Success = true;
        }
        else if (typeId == "Video.Seek")
        {
            mHost->SeekVideo(ReadInput(nodeIndex, 1).AsNumber());
            state.Success = true;
        }
        else if (typeId == "Video.SetLooping")
        {
            mHost->SetVideoLooping(ReadInput(nodeIndex, 1).AsBool());
            state.Success = true;
        }
        else if (typeId == "Video.SetVolume")
        {
            mHost->SetVideoVolume(ReadInput(nodeIndex, 1).AsNumber());
            state.Success = true;
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

void NodeGraphRuntime::ExecuteEntityNode(int nodeIndex)
{
    RuntimeNode& node = mNodes[nodeIndex];
    NodeState& state = node.State;
    const std::string& typeId = node.Data->TypeId;
    state.Success = false;

    if (mHost == nullptr)
    {
        Log("Entity node ran with no host attached.");
        FireExec(nodeIndex, 0);
        return;
    }

    const std::uint64_t entity = ReadEntity(nodeIndex);
    NodeGraphTransform transform;
    if (entity == 0 || !mHost->GetEntityTransform(entity, transform))
    {
        // Reported once per node execution, not silently: an unpicked or deleted entity is
        // the most likely mistake in an entity graph.
        Log(std::string(node.Type->Caption) + ": " +
            (entity == 0 ? std::string("no entity chosen.")
                         : "entity " + std::to_string(entity) + " is not in the level."));
        FireExec(nodeIndex, 0);
        return;
    }

    double vector[3]{};
    const bool takesVector = NodeGraphCatalog::IndexOfInput(*node.Type, "X") == 2;
    if (takesVector)
    {
        for (int a = 0; a < 3; ++a)
        {
            vector[a] = ReadInput(nodeIndex, 2 + a).AsNumber();
        }
    }

    if (typeId == "Entity.SetPosition" || typeId == "Entity.AddPosition")
    {
        const bool add = typeId == "Entity.AddPosition";
        for (int a = 0; a < 3; ++a)
        {
            transform.Position[a] = (add ? transform.Position[a] : 0.0) + vector[a];
        }
        state.Success = mHost->SetEntityTransform(entity, transform);
    }
    else if (typeId == "Entity.SetRotation" || typeId == "Entity.AddRotation")
    {
        const bool add = typeId == "Entity.AddRotation";
        for (int a = 0; a < 3; ++a)
        {
            transform.Rotation[a] = (add ? transform.Rotation[a] : 0.0) + vector[a] * kDegreesToRadians;
        }
        state.Success = mHost->SetEntityTransform(entity, transform);
    }
    else if (typeId == "Entity.SetScale")
    {
        for (int a = 0; a < 3; ++a)
        {
            transform.Scale[a] = vector[a];
        }
        state.Success = mHost->SetEntityTransform(entity, transform);
    }
    else if (typeId == "Entity.MoveTo")
    {
        // A newer move of the same entity takes over from one still in flight.
        for (RuntimeNode& other : mNodes)
        {
            if (other.Data->TypeId == "Entity.MoveTo" && other.State.TweenEntity == entity)
            {
                other.State.TweenActive = false;
            }
        }

        state.TweenEntity = entity;
        state.TweenElapsed = 0.0;
        state.TweenDuration = ReadInput(nodeIndex, 5).AsNumber();
        for (int a = 0; a < 3; ++a)
        {
            state.TweenFrom[a] = transform.Position[a];
            state.TweenTo[a] = vector[a];
        }
        state.TweenActive = true;
        state.Success = true;

        FireExec(nodeIndex, 0);
        // A zero duration still arrives through the tween, on the next tick, so Completed
        // is always a frame after Then and never re-enters this chain.
        return;
    }
    else if (typeId == "Entity.SetProperty")
    {
        const std::string property = ParamValue(nodeIndex, "Property", NodePinKind::String).AsString();
        state.Success = mHost->SetEntityProperty(entity, property, ReadInput(nodeIndex, 2).AsNumber());
        if (!state.Success)
        {
            Log("Set Entity Property: entity " + std::to_string(entity) + " has no '" + property + "'.");
        }
    }
    else if (typeId == "Entity.PlayAudio" || typeId == "Entity.StopAudio")
    {
        state.Success = mHost->SetEntityAudioPlaying(entity, typeId == "Entity.PlayAudio");
    }

    FireExec(nodeIndex, 0);
}

NodeGraphValue NodeGraphRuntime::EvaluateEntityOutput(int nodeIndex, int outPortIndex)
{
    const RuntimeNode& node = mNodes[nodeIndex];
    const std::string& typeId = node.Data->TypeId;

    // Nodes with an exec input report the Success of their last run on their value pin.
    if (node.Type->InputCount > 0 && node.Type->Inputs[0].Kind == NodePinKind::Exec)
    {
        return NodeGraphValue::FromBool(node.State.Success);
    }

    if (typeId == "Entity.Reference")
    {
        return NodeGraphValue::FromNumber(static_cast<double>(ReadEntity(nodeIndex)));
    }

    if (mHost == nullptr)
    {
        return NodeGraphValue::FromNumber(0.0);
    }

    if (typeId == "Entity.FindByName")
    {
        const std::uint64_t entity = mHost->FindEntityByName(ReadInput(nodeIndex, 0).AsString());
        return outPortIndex == 0 ? NodeGraphValue::FromNumber(static_cast<double>(entity))
                                 : NodeGraphValue::FromBool(entity != 0);
    }

    const std::uint64_t entity = ReadEntity(nodeIndex);

    if (typeId == "Entity.IsValid" || typeId == "Entity.GetName")
    {
        std::string name;
        const bool valid = entity != 0 && mHost->GetEntityName(entity, name);
        return typeId == "Entity.IsValid" ? NodeGraphValue::FromBool(valid) : NodeGraphValue::FromString(name);
    }

    if (typeId == "Entity.GetProperty")
    {
        double value = 0.0;
        const bool found = entity != 0 && mHost->GetEntityProperty(
            entity, ParamValue(nodeIndex, "Property", NodePinKind::String).AsString(), value);
        return outPortIndex == 0 ? NodeGraphValue::FromNumber(value) : NodeGraphValue::FromBool(found);
    }

    NodeGraphTransform transform;
    if (entity == 0 || !mHost->GetEntityTransform(entity, transform) || outPortIndex < 0 || outPortIndex > 2)
    {
        return NodeGraphValue::FromNumber(0.0);
    }

    if (typeId == "Entity.GetPosition")
        return NodeGraphValue::FromNumber(transform.Position[outPortIndex]);
    if (typeId == "Entity.GetRotation")
        return NodeGraphValue::FromNumber(transform.Rotation[outPortIndex] * kRadiansToDegrees);
    if (typeId == "Entity.GetScale")
        return NodeGraphValue::FromNumber(transform.Scale[outPortIndex]);

    return NodeGraphValue::FromNumber(0.0);
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
        result = NodeGraphValue::FromNumber(low + (high - low) * NextRandom());
    }
    else if (typeId == "Math.RandomInteger")
    {
        const double a = ReadInput(nodeIndex, 0).AsNumber();
        const double b = ReadInput(nodeIndex, 1).AsNumber();
        const double low = std::ceil((std::min)(a, b));
        const double high = std::floor((std::max)(a, b));
        const double span = high - low + 1.0;
        result = NodeGraphValue::FromNumber(
            span < 1.0 ? low : (std::min)(low + std::floor(NextRandom() * span), high));
    }
    else if (typeId == "Math.RandomChance")
    {
        result = NodeGraphValue::FromBool(NextRandom() < ReadInput(nodeIndex, 0).AsNumber());
    }
    else if (typeId == "Math.Min" || typeId == "Math.Max")
    {
        const double a = ReadInput(nodeIndex, 0).AsNumber();
        const double b = ReadInput(nodeIndex, 1).AsNumber();
        result = NodeGraphValue::FromNumber(typeId == "Math.Min" ? (std::min)(a, b) : (std::max)(a, b));
    }
    else if (typeId == "Math.Modulo")
    {
        const double a = ReadInput(nodeIndex, 0).AsNumber();
        const double b = ReadInput(nodeIndex, 1).AsNumber();
        double remainder = b == 0.0 ? 0.0 : std::fmod(a, b);
        // fmod keeps the sign of A; wrapping wants the sign of B, so -1 mod 6 is 5.
        if (remainder != 0.0 && ((remainder < 0.0) != (b < 0.0)))
        {
            remainder += b;
        }
        result = NodeGraphValue::FromNumber(remainder);
    }
    else if (typeId == "Math.Abs")
    {
        result = NodeGraphValue::FromNumber(std::abs(ReadInput(nodeIndex, 0).AsNumber()));
    }
    else if (typeId == "Math.Floor")
    {
        result = NodeGraphValue::FromNumber(std::floor(ReadInput(nodeIndex, 0).AsNumber()));
    }
    else if (typeId == "Math.Sin")
    {
        result = NodeGraphValue::FromNumber(std::sin(ReadInput(nodeIndex, 0).AsNumber() * kDegreesToRadians));
    }
    else if (typeId == "Math.Cos")
    {
        result = NodeGraphValue::FromNumber(std::cos(ReadInput(nodeIndex, 0).AsNumber() * kDegreesToRadians));
    }
    else if (typeId == "Math.Ease")
    {
        result = NodeGraphValue::FromNumber(ApplyEasing(
            ReadInput(nodeIndex, 0).AsNumber(), ParamValue(nodeIndex, "Easing", NodePinKind::String).AsString()));
    }
    else if (typeId == "Math.LerpAngle")
    {
        const double a = ReadInput(nodeIndex, 0).AsNumber();
        const double b = ReadInput(nodeIndex, 1).AsNumber();
        result = NodeGraphValue::FromNumber(a + std::remainder(b - a, 360.0) * ReadInput(nodeIndex, 2).AsNumber());
    }
    else if (typeId == "Value.FormatNumber")
    {
        result = NodeGraphValue::FromString(FormatNumber(
            ReadInput(nodeIndex, 0).AsNumber(),
            static_cast<int>(ParamValue(nodeIndex, "Decimals", NodePinKind::Number).AsNumber()),
            ParamValue(nodeIndex, "Grouping", NodePinKind::Bool).AsBool()));
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
    else if (typeId == "Video.IsPlaying")
    {
        result = NodeGraphValue::FromBool(mHost != nullptr && mHost->IsVideoPlaying());
    }
    else if (typeId == "Video.GetTime")
    {
        result = NodeGraphValue::FromNumber(mHost != nullptr ? mHost->GetVideoTime() : 0.0);
    }
    else if (typeId == "Video.GetDuration")
    {
        result = NodeGraphValue::FromNumber(mHost != nullptr ? mHost->GetVideoDuration() : 0.0);
    }
    else if (typeId.compare(0, 6, "Video.") == 0)
    {
        result = NodeGraphValue::FromBool(node.State.Success);
    }
    else if (typeId == "Event.UiButtonClicked")
    {
        result = NodeGraphValue::FromString(node.State.LastValue.AsString());
    }
    else if (typeId == "Input.IsKeyDown")
    {
        const int key = VirtualKeyFromName(ParamValue(nodeIndex, "Key", NodePinKind::String).AsString());
        result = NodeGraphValue::FromBool(key != 0 && mHost != nullptr && mHost->IsKeyDown(key));
    }
    else if (typeId.compare(0, 7, "Entity.") == 0)
    {
        result = EvaluateEntityOutput(nodeIndex, outPortIndex);
    }
    else if (typeId == "Camera.Get")
    {
        const NodeGraphCameraPose pose = CurrentCamera();
        switch (outPortIndex)
        {
        case 0: case 1: case 2: result = NodeGraphValue::FromNumber(pose.Position[outPortIndex]); break;
        case 3: result = NodeGraphValue::FromNumber(pose.Pitch * kRadiansToDegrees); break;
        case 4: result = NodeGraphValue::FromNumber(pose.Yaw * kRadiansToDegrees); break;
        default: break;
        }
    }
    else if (typeId == "Camera.LookPoint")
    {
        // Intersect the view ray with the horizontal plane: the point under the centre of
        // the screen, e.g. where to drop something so it lands in view.
        const NodeGraphCameraPose pose = CurrentCamera();
        const double planeZ = ReadInput(nodeIndex, 0).AsNumber();
        const double forward[3] = {
            std::sin(pose.Yaw) * std::cos(pose.Pitch),
            std::cos(pose.Yaw) * std::cos(pose.Pitch),
            std::sin(pose.Pitch)
        };
        const double distance = std::abs(forward[2]) > 1e-6 ? (planeZ - pose.Position[2]) / forward[2] : -1.0;
        const bool hit = distance > 0.0;
        if (outPortIndex == 3)
        {
            result = NodeGraphValue::FromBool(hit);
        }
        else if (outPortIndex >= 0 && outPortIndex < 3)
        {
            result = NodeGraphValue::FromNumber(
                hit ? pose.Position[outPortIndex] + forward[outPortIndex] * distance
                    : (outPortIndex == 2 ? planeZ : pose.Position[outPortIndex]));
        }
    }
    else if (typeId == "Audio.IsMusicPlaying")
    {
        result = NodeGraphValue::FromBool(mHost != nullptr && mHost->IsMusicPlaying());
    }
    else if (typeId.compare(0, 6, "Audio.") == 0)
    {
        result = NodeGraphValue::FromBool(node.State.Success);
    }
    else if (typeId == "Game.IsStandalone")
    {
        result = NodeGraphValue::FromBool(mHost != nullptr && mHost->IsStandalone());
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
