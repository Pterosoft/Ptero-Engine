#pragma once

#include "DX12Helper.h"
#include "RmlUiRenderInterface.h"
#include "RmlUiSystemInterface.h"

#include "../QtUi/UiTypes.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Rml {
class Context;
class ElementDocument;
} // namespace Rml

// Owns the engine's RmlUi integration: library lifetime, the UI context, the documents
// loaded into it, and the off-screen target the UI is drawn to.
//
// The UI renders to its own RGBA8 target rather than into the scene image. The scene's
// final colour comes out of whichever post-processing pass ran last - a compute output
// with no render-target view - so drawing over it would mean bolting an RTV onto every
// stage of that chain. A dedicated target keeps the UI independent of the post chain and
// composites the same way for the editor viewport today and a swap chain later.
class RmlUiRenderer
{
public:
    RmlUiRenderer();
    ~RmlUiRenderer();

    RmlUiRenderer(const RmlUiRenderer&) = delete;
    RmlUiRenderer& operator=(const RmlUiRenderer&) = delete;

    bool Initialize(UINT width, UINT height, ID3D12GraphicsCommandList* commandList);
    void Shutdown();
    bool Resize(UINT width, UINT height);

    // Updates the context and draws it into the UI target. Safe to call every frame even
    // with nothing loaded; it becomes a clear.
    void Render(ID3D12GraphicsCommandList* commandList);

    // Loads a UTF-8 path relative to Data/UI (e.g. Farkle/farkle.rml) and shows it.
    // Subdirectories are supported. Any previously loaded document is
    // closed first, so this doubles as the reload path while authoring.
    bool LoadDocument(const std::string& fileName);
    void CloseDocument();
    bool ReloadDocument();
    // Buffered IDs: game actions are consumed on Update, never during DOM dispatch.
    std::string PollGameAction();
    void ApplyGameUiCommand(int operation, const char* id, const char* value);

    // Called when a button is clicked or first hovered, so the host can play UI audio.
    // Hover and click are the one part of the game's sound that the game module cannot
    // drive: it only ever sees the actions a click produced, and never sees hover at
    // all. An empty callback (the default) disables UI sound. Set while a play session
    // is running; the UI Editor's preview is silent.
    void SetUiSoundCallback(std::function<void(bool isClick)> callback) { mUiSoundCallback = std::move(callback); }

    // Element edits addressed by RML id, for game code and for the node graph's UI nodes.
    // Each returns false when there is no document loaded or no element with that id, so a
    // typo in a graph shows up as a failed Success pin rather than as silence.
    bool SetElementText(const std::string& elementId, const std::string& text);
    bool SetElementProperty(const std::string& elementId, const std::string& property, const std::string& value);
    bool SetElementClass(const std::string& elementId, const std::string& className, bool enabled);
    bool SetElementVisible(const std::string& elementId, bool visible);

    const std::string& GetLoadedDocumentName() const { return mLoadedDocumentName; }

    // Mouse state in UI pixels, relative to the top-left of the UI target. `hasFocus` is
    // false when the pointer is somewhere else in the editor, which sends RmlUi a mouse
    // leave so hover states do not stick.
    void SetMouseState(bool hasFocus, float x, float y, bool leftDown, bool rightDown, bool middleDown);

    // Polls the keyboard and forwards key and text events. Called once per frame while
    // the UI has input focus.
    void UpdateKeyboardInput();

    void SetInputEnabled(bool enabled) { mInputEnabled = enabled; }
    bool IsInputEnabled() const { return mInputEnabled; }

    void SetVisible(bool visible) { mIsVisible = visible; }
    bool IsVisible() const { return mIsVisible; }

    // Separate from SetVisible, which means "the game wants this on screen" and
    // is what decides whether the UI composites over the viewport. The preview
    // flag only makes the pass run, so the UI Editor has something to show
    // without the document appearing over the scene as a side effect.
    void SetPreviewActive(bool active) { mPreviewActive = active; }
    bool IsPreviewActive() const { return mPreviewActive; }

    bool IsInitialized() const { return mIsInitialized; }

    // Event tracing, for the UI Editor. A document that looks right but does
    // nothing is the normal failure, and nothing on screen distinguishes "the
    // button has no handler" from "the click never reached the button". With
    // tracing on, a listener sits on the document in the capture phase and
    // records what actually arrived, so the two are told apart by looking.
    //
    // Off by default: it costs a listener call per event, and a game has no use
    // for it.
    struct TracedEvent
    {
        double      TimeSeconds = 0.0;  // since tracing was enabled
        std::string Type;               // "click", "mousedown", ...
        std::string ElementId;          // id attribute, empty if unset
        std::string ElementTag;         // "button", "div", ...
    };

    void SetEventTracingEnabled(bool enabled);
    bool IsEventTracingEnabled() const { return mEventTracingEnabled; }
    // Oldest first, capped at kMaxTracedEvents.
    const std::vector<TracedEvent>& GetTracedEvents() const { return mTracedEvents; }
    void ClearTracedEvents() { mTracedEvents.clear(); }

    // Documents found recursively under Data/UI, as sorted UTF-8 relative paths
    // with forward slashes (e.g. Farkle/farkle.rml). Rescanned on every
    // call: the point of the viewer is to pick up a file you just wrote.
    static std::vector<std::string> ListAvailableDocuments();

    // RmlUi's built-in element inspector, hosted in the same context as the documents.
    void SetDebuggerVisible(bool visible);
    bool IsDebuggerVisible() const;
    bool IsDebuggerAvailable() const { return mDebuggerAvailable; }

    UiTextureID GetOutputTextureId() const { return mOutputTextureId; }
    D3D12_GPU_DESCRIPTOR_HANDLE GetOutputGpuSrv() const { return mOutputSrvGpuHandle; }
    ID3D12Resource* GetOutputResource() const { return mOutputTexture.Get(); }


    UINT GetWidth() const { return mWidth; }
    UINT GetHeight() const { return mHeight; }

    const char* GetLastErrorMessage() const
    {
        return mLastError.empty() ? nullptr : mLastError.c_str();
    }

private:
    bool CreateRenderTarget(UINT width, UINT height);
    void ReleaseRenderTarget();

    bool LoadFonts();
    int GetKeyModifierState() const;

    RmlUiSystemInterface mSystemInterface;
    RmlUiRenderInterface mRenderInterface;

    Rml::Context* mContext = nullptr;
    Rml::ElementDocument* mDocument = nullptr;
    std::string mLoadedDocumentName;

    // The UI target holds premultiplied alpha, which is what the viewport overlay blit
    // expects; no conversion pass is needed because that blend is the engine's own.
    Microsoft::WRL::ComPtr<ID3D12Resource> mOutputTexture;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> mRtvHeap;
    D3D12_CPU_DESCRIPTOR_HANDLE mRtvHandle{};
    D3D12_CPU_DESCRIPTOR_HANDLE mOutputSrvCpuHandle{};
    D3D12_GPU_DESCRIPTOR_HANDLE mOutputSrvGpuHandle{};
    UiTextureID mOutputTextureId = UiTextureID_Invalid;
    bool mSrvAllocated = false;

    UINT mWidth = 0;
    UINT mHeight = 0;

    // Previous mouse state, so per-frame polling can be turned into the press and release
    // edges RmlUi expects.
    bool mHasMouseFocus = false;
    std::array<bool, 3> mMouseButtonDown{};

    // Previous keyboard state, indexed by virtual-key code. Mapped keys and text input
    // are tracked separately because the two ranges overlap - space is both a mapped key
    // and a printable character, and a shared table would let one edge swallow the other.
    std::array<bool, 256> mKeyDown{};
    std::array<bool, 256> mTextKeyDown{};

    // Event tracing state. The listener is owned here rather than by the
    // document, because a reload destroys the document and the trace has to
    // survive it - reloading and clicking again is the whole workflow.
    static constexpr std::size_t kMaxTracedEvents = 400;
    class TraceListener;
    class ActionListener;
    class SoundListener;
    std::unique_ptr<ActionListener> mActionListener;
    std::unique_ptr<SoundListener> mSoundListener;
    std::function<void(bool)> mUiSoundCallback;
    std::vector<std::string> mGameActions;
    std::unique_ptr<TraceListener> mTraceListener;
    std::vector<TracedEvent> mTracedEvents;
    bool mEventTracingEnabled = false;
    bool mPreviewActive = false;

    // Attaches or detaches the trace listener on the current document. Called
    // by SetEventTracingEnabled and again after every load, since a fresh
    // document starts with no listeners.
    void ApplyEventTracing();

    bool mInputEnabled = true;
    bool mIsVisible = true;
    bool mIsInitialized = false;
    bool mOwnsRmlLifetime = false;
    bool mDebuggerAvailable = false;
    std::string mLastError;
};
