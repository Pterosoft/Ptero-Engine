#pragma once

#include "System/NodeGraphDocument.h"

#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

// Everything a running graph can do to the engine.
//
// The runtime never touches the renderer, RmlUi or the game host directly: it calls
// through this interface so a graph can be executed - and unit-tested - without a live
// engine behind it. DX12SceneRenderer supplies the real implementation.
class NodeGraphHost
{
public:
    virtual ~NodeGraphHost() = default;

    virtual bool ShowUiDocument(const std::string& fileName) = 0;
    virtual void CloseUiDocument() = 0;
    virtual bool ReloadUiDocument() = 0;
    virtual void SetUiVisible(bool visible) = 0;
    virtual void SetUiInputEnabled(bool enabled) = 0;
    virtual bool SetUiElementText(const std::string& elementId, const std::string& text) = 0;
    virtual bool SetUiElementProperty(
        const std::string& elementId,
        const std::string& property,
        const std::string& value) = 0;
    virtual bool SetUiElementClass(
        const std::string& elementId,
        const std::string& className,
        bool enabled) = 0;
    virtual bool SetUiElementVisible(const std::string& elementId, bool visible) = 0;
};

// One value flowing along a data connection. Graphs are loosely typed on purpose - a
// number reaching a string port is converted rather than refused - because the editor
// already prevents the connections that would be genuinely meaningless.
struct NodeGraphValue
{
    enum class Kind
    {
        Bool,
        Number,
        String
    };

    Kind ValueKind = Kind::Number;
    bool Boolean = false;
    double Number = 0.0;
    std::string Text;

    static NodeGraphValue FromBool(bool value);
    static NodeGraphValue FromNumber(double value);
    static NodeGraphValue FromString(std::string value);
    // Parses the text form used by node parameters and variable defaults.
    static NodeGraphValue Parse(const std::string& text, NodePinKind kind);
    static NodeGraphValue Parse(const std::string& text, NodeGraphVariableType type);

    // Narrows this value to a declared variable type, so a variable never changes shape
    // depending on which branch happened to write it - an Int stays whole, a Float stays
    // at single precision.
    NodeGraphValue CoerceTo(NodeGraphVariableType type) const;

    bool AsBool() const;
    double AsNumber() const;
    std::string AsString() const;
};

// Interprets a NodeGraphDocument for the duration of one play session.
//
// Execution is the blueprint model rather than the data-flow one the QtNodes scene uses
// for drawing: white exec connections push control forward from an event, and value
// connections are pulled on demand by whichever node needs them. Values are re-read every
// time a node executes - a graph that reads a variable twice in a row must see the second
// write - and memoised only for the duration of that one node, so a diamond feeding two
// inputs of the same node still evaluates its shared branch once.
class NodeGraphRuntime
{
public:
    void SetHost(NodeGraphHost* host) { mHost = host; }

    // Takes a copy of the document, so the editor may keep changing the graph while a
    // play session is running without the runtime seeing half an edit.
    void Start(const NodeGraphDocument& document);
    void Tick(float deltaSeconds);
    void Stop();

    bool IsRunning() const { return mIsRunning; }

    // Set by the Stop Game node. The engine polls this after Tick instead of the node
    // calling straight back into the game host, which would tear the runtime down from
    // inside its own execution.
    bool ConsumeStopRequest();

    // Most recent lines written by Print String nodes and by the runtime itself, oldest
    // first. Bounded, so a graph printing every frame cannot grow without limit.
    const std::deque<std::string>& GetLog() const { return mLog; }
    void ClearLog() { mLog.clear(); }

    float GetPlayTimeSeconds() const { return static_cast<float>(mPlayTimeSeconds); }

private:
    struct NodeState
    {
        // Gate open, Do Once already fired, Flip Flop is on A.
        bool Flag = false;
        // Do N counter, For Loop index.
        int Counter = 0;
        // Result of the last UI call made by this node.
        bool Success = false;
        // Value last written by a Set Variable node.
        NodeGraphValue LastValue;
        bool DelayActive = false;
        double DelayRemaining = 0.0;
    };

    struct RuntimeNode
    {
        const NodeGraphNodeType* Type = nullptr;
        const NodeGraphNode* Data = nullptr;
        NodeState State;
    };

    struct DataSource
    {
        int NodeIndex = -1;
        int PortIndex = -1;
    };

    void BuildIndex();
    void ResetVariables();

    void FireEvents(const char* eventTypeId);
    void FireExec(int nodeIndex, int outPortIndex);
    void ExecuteNode(int nodeIndex, int inPortIndex);

    NodeGraphValue ReadInput(int nodeIndex, int inPortIndex);
    NodeGraphValue EvaluateOutput(int nodeIndex, int outPortIndex);
    NodeGraphValue ParamValue(int nodeIndex, const char* key, NodePinKind kind) const;

    NodeGraphValue* FindVariableSlot(const std::string& name);

    void Log(std::string message);

    // Combines a node index and a port index into one map key.
    static long long PortKey(int nodeIndex, int portIndex);

    NodeGraphHost* mHost = nullptr;
    NodeGraphDocument mDocument;
    std::vector<RuntimeNode> mNodes;
    std::unordered_map<int, int> mIndexById;
    std::unordered_map<long long, std::vector<DataSource>> mExecTargets;
    std::unordered_map<long long, DataSource> mDataSources;
    std::unordered_map<std::string, NodeGraphValue> mVariables;
    // Function name to the index of its Function Entry node. Built once at Start, so a
    // Call node costs a hash lookup rather than a scan of the graph.
    std::unordered_map<std::string, int> mFunctions;

    // Memo for one node execution, cleared whenever a node runs, so a value read twice
    // while that node gathers its inputs is computed once but still refreshes next time.
    std::unordered_map<long long, NodeGraphValue> mValueCache;
    // Nodes currently being evaluated, so a cycle in the value graph is broken instead of
    // recursing until the stack runs out.
    std::vector<int> mEvaluationStack;

    std::deque<std::string> mLog;
    double mPlayTimeSeconds = 0.0;
    float mDeltaSeconds = 0.0f;
    // Budget for one exec step. A gate feeding itself is a mistake the editor cannot
    // detect, and a hung editor is a far worse failure than a truncated graph.
    int mStepsRemaining = 0;
    // Guards the stack rather than the clock. Execution is recursive, so the step budget
    // alone would let a self-feeding loop pile up thousands of frames and overflow long
    // before it ran out of steps; this caps how deep one chain may go.
    int mExecDepth = 0;
    bool mIsRunning = false;
    bool mStopRequested = false;
    unsigned mRandomState = 0x1234abcdu;
};
