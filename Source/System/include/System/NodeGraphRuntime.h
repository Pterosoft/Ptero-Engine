#pragma once

#include "System/NodeGraphDocument.h"

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

// An entity's pose as the host stores it: metres, radians, and a scale of 1 for the
// authored size. The nodes present rotations in degrees and convert at the boundary.
struct NodeGraphTransform
{
    double Position[3]{};
    double Rotation[3]{};
    double Scale[3]{ 1.0, 1.0, 1.0 };
};

// The game camera in the engine's left-handed Z-up space, angles in radians:
//   forward = (sin(Yaw) * cos(Pitch), cos(Yaw) * cos(Pitch), sin(Pitch))
struct NodeGraphCameraPose
{
    double Position[3]{};
    double Pitch = 0.0;
    double Yaw = 0.0;
};

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

    // The full-screen video layer. `fit` is "Letterbox", "Fill" or "Stretch".
    virtual bool PlayVideo(const std::string& fileName, bool loop, const std::string& fit) = 0;
    virtual void PauseVideo() = 0;
    virtual void ResumeVideo() = 0;
    virtual void StopVideo() = 0;
    virtual void SeekVideo(double seconds) = 0;
    virtual void SetVideoLooping(bool loop) = 0;
    virtual void SetVideoVolume(double volume) = 0;
    virtual bool IsVideoPlaying() = 0;
    virtual double GetVideoTime() = 0;
    virtual double GetVideoDuration() = 0;
    // True once after a video reached its end without looping; the runtime polls this
    // every tick to fire On Video Finished.
    virtual bool ConsumeVideoFinished() = 0;

    // Entities, addressed by their persistent Entity::Id. 0 is never an entity, and every
    // call on an id the running level does not contain fails rather than guessing.
    virtual std::uint64_t FindEntityByName(const std::string& name) = 0;
    virtual bool GetEntityName(std::uint64_t entityId, std::string& name) = 0;
    virtual bool GetEntityTransform(std::uint64_t entityId, NodeGraphTransform& transform) = 0;
    virtual bool SetEntityTransform(std::uint64_t entityId, const NodeGraphTransform& transform) = 0;
    // `property` is one of the names in the catalogue's entity property list. Booleans
    // travel as 1 and 0. False when the entity lacks the component that owns it.
    virtual bool GetEntityProperty(std::uint64_t entityId, const std::string& property, double& value) = 0;
    virtual bool SetEntityProperty(std::uint64_t entityId, const std::string& property, double value) = 0;
    // Starts or stops the entity's Audio Emitter.
    virtual bool SetEntityAudioPlaying(std::uint64_t entityId, bool playing) = 0;

    // The camera the frame will render with. SetCamera is applied as-is; the runtime is
    // what re-applies a held pose every frame over the game module's own camera.
    virtual NodeGraphCameraPose GetCamera() = 0;
    virtual void SetCamera(const NodeGraphCameraPose& pose) = 0;

    // FMOD events by bare name or full path, as the game module's audio calls take them.
    // Not PlaySound: <windows.h> defines that as a macro.
    virtual void PlayOneShot(const std::string& eventName) = 0;
    virtual bool PlayMusic(const std::string& eventName) = 0;
    virtual void StopMusic() = 0;
    virtual bool IsMusicPlaying() = 0;

    // Win32 virtual-key code. False whenever the game window does not have focus, so
    // typing in another window never drives the game.
    virtual bool IsKeyDown(int virtualKey) = 0;
    // One buffered button activation per call, oldest first; false when there are none.
    virtual bool PollUiClick(std::string& elementId) = 0;
    virtual void ToggleFullscreen() = 0;
    virtual bool IsStandalone() = 0;
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
        // Key state seen by an On Key Pressed/Released node last frame, for edge detection.
        bool KeyWasDown = false;
        // Move Entity To / Move Camera To. From and To hold a position (entity) or a
        // position plus pitch and yaw in radians (camera).
        bool TweenActive = false;
        double TweenElapsed = 0.0;
        double TweenDuration = 0.0;
        std::uint64_t TweenEntity = 0;
        double TweenFrom[5]{};
        double TweenTo[5]{};
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
    // Fires one node that is an entry point, with a fresh step budget and depth.
    void FireEntry(int nodeIndex, int outPortIndex);
    void FireInputEvents();
    void AdvanceTweens(float deltaSeconds);
    void UpdatePlaylist();
    void PlayNextPlaylistTrack();
    void FireExec(int nodeIndex, int outPortIndex);
    void ExecuteNode(int nodeIndex, int inPortIndex);

    NodeGraphValue ReadInput(int nodeIndex, int inPortIndex);
    NodeGraphValue EvaluateOutput(int nodeIndex, int outPortIndex);
    NodeGraphValue ParamValue(int nodeIndex, const char* key, NodePinKind kind) const;

    NodeGraphValue* FindVariableSlot(const std::string& name);

    // Reads the node's Entity pin (or its picker) as an id, 0 when nothing is chosen.
    std::uint64_t ReadEntity(int nodeIndex);
    NodeGraphValue EvaluateEntityOutput(int nodeIndex, int outPortIndex);
    void ExecuteEntityNode(int nodeIndex);
    // The pose Get Camera reports: the held one while the graph owns the camera, since
    // the renderer's camera still shows the game module's pose until the tick ends.
    NodeGraphCameraPose CurrentCamera();

    // Uniform in [0, 1).
    double NextRandom();

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
    // Reseeded at Start, so two play sessions do not roll the same numbers.
    unsigned mRandomState = 0x1234abcdu;

    // Set Camera / Move Camera To hold the camera until Release Camera. The pose is
    // re-applied at the end of every tick because the game module writes its own camera
    // each frame before the graph runs.
    bool mCameraHeld = false;
    NodeGraphCameraPose mCameraPose;

    // Play Music Playlist. Lives on the runtime rather than a node: there is one music
    // channel, and whichever node last started music owns it.
    bool mPlaylistActive = false;
    std::string mPlaylistPrefix;
    int mPlaylistCount = 0;
    bool mPlaylistShuffle = true;
    int mPlaylistTrack = -1;
};
