#include "pch.h"

#include "RmlUiRenderer.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Debugger.h>

#include <algorithm>
#include <cwctype>
#include <chrono>
#include <filesystem>
#include <vector>

using Microsoft::WRL::ComPtr;

extern "C"
{
    ID3D12Device* __stdcall DX12Context_GetDevice();
    bool __stdcall DX12Context_AllocateSrvDescriptor(
        D3D12_CPU_DESCRIPTOR_HANDLE* cpuHandle,
        D3D12_GPU_DESCRIPTOR_HANDLE* gpuHandle);
    HWND __stdcall DX12Context_GetWindowHandle();
    ID3D12DescriptorHeap* __stdcall DX12Context_GetSrvDescriptorHeap();
}

namespace
{
    // R8G8B8A8 rather than the scene's float format: RmlUi works in sRGB-ish 8-bit colour
    // and the result is composited as a plain image, so the extra precision buys nothing.
    constexpr DXGI_FORMAT UiTargetFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    std::filesystem::path FindDataDirectory()
    {
        wchar_t modulePath[MAX_PATH] = {};
        if (GetModuleFileNameW(nullptr, modulePath, MAX_PATH) == 0)
            return L"Data";

        std::filesystem::path directory = std::filesystem::path(modulePath).parent_path();
        for (int attempt = 0; attempt < 6; ++attempt)
        {
            const std::filesystem::path candidate = directory / L"Data";
            if (std::filesystem::exists(candidate))
                return candidate;

            const std::filesystem::path parent = directory.parent_path();
            if (parent == directory)
                break;

            directory = parent;
        }

        return L"Data";
    }

    std::wstring FindShaderDirectory()
    {
        const std::filesystem::path shaderDirectory = FindDataDirectory() / L"Shaders";
        return shaderDirectory.wstring() + L"\\";
    }

    std::string WideToUtf8(const std::wstring& text)
    {
        if (text.empty())
            return {};

        const int required = WideCharToMultiByte(
            CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (required <= 0)
            return {};

        std::string result(static_cast<size_t>(required), '\0');
        WideCharToMultiByte(
            CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), result.data(), required, nullptr, nullptr);
        return result;
    }

    // The keys forwarded to RmlUi. Printable characters arrive separately as text input,
    // so this table only covers navigation and editing keys plus the modifiers RmlUi
    // reports back to documents.
    struct KeyMapping
    {
        int VirtualKey;
        Rml::Input::KeyIdentifier Identifier;
    };

    constexpr KeyMapping KeyMappings[] = {
        { VK_BACK,   Rml::Input::KI_BACK },
        { VK_TAB,    Rml::Input::KI_TAB },
        { VK_RETURN, Rml::Input::KI_RETURN },
        { VK_ESCAPE, Rml::Input::KI_ESCAPE },
        { VK_SPACE,  Rml::Input::KI_SPACE },
        { VK_PRIOR,  Rml::Input::KI_PRIOR },
        { VK_NEXT,   Rml::Input::KI_NEXT },
        { VK_END,    Rml::Input::KI_END },
        { VK_HOME,   Rml::Input::KI_HOME },
        { VK_LEFT,   Rml::Input::KI_LEFT },
        { VK_UP,     Rml::Input::KI_UP },
        { VK_RIGHT,  Rml::Input::KI_RIGHT },
        { VK_DOWN,   Rml::Input::KI_DOWN },
        { VK_INSERT, Rml::Input::KI_INSERT },
        { VK_DELETE, Rml::Input::KI_DELETE },
        { VK_F1,     Rml::Input::KI_F1 },
        { VK_F2,     Rml::Input::KI_F2 },
        { VK_F3,     Rml::Input::KI_F3 },
        { VK_F4,     Rml::Input::KI_F4 },
        { VK_F5,     Rml::Input::KI_F5 },
    };
}

// Sits on the document in the capture phase, so it sees an event on its way
// down to the target whether or not anything further along handles it. That is
// the distinction the UI Editor exists to show: a click that reaches the button
// and does nothing is a missing handler, a click that never appears here is an
// input or hit-testing problem.
class RmlUiRenderer::TraceListener final : public Rml::EventListener
{
public:
    explicit TraceListener(RmlUiRenderer& owner) : mOwner(owner) {}

    void ProcessEvent(Rml::Event& event) override
    {
        TracedEvent traced;
        traced.TimeSeconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - mStarted).count();
        traced.Type = event.GetType();

        if (Rml::Element* element = event.GetTargetElement())
        {
            traced.ElementId = element->GetId();
            traced.ElementTag = element->GetTagName();
        }

        auto& events = mOwner.mTracedEvents;
        events.push_back(std::move(traced));
        if (events.size() > kMaxTracedEvents)
            events.erase(events.begin(), events.begin() + (events.size() - kMaxTracedEvents));
    }

private:
    RmlUiRenderer& mOwner;
    std::chrono::steady_clock::time_point mStarted = std::chrono::steady_clock::now();
};

RmlUiRenderer::RmlUiRenderer() = default;

RmlUiRenderer::~RmlUiRenderer()
{
    Shutdown();
}

bool RmlUiRenderer::Initialize(UINT width, UINT height, ID3D12GraphicsCommandList* commandList)
{
    if (mIsInitialized)
        return true;

    mWidth = (std::max)(width, 1u);
    mHeight = (std::max)(height, 1u);

    if (!mRenderInterface.Initialize(UiTargetFormat, commandList))
    {
        mLastError = mRenderInterface.GetLastErrorMessage();
        return false;
    }

    if (!CreateRenderTarget(mWidth, mHeight))
        return false;


    const std::filesystem::path dataDirectory = FindDataDirectory();
    mRenderInterface.SetDocumentDirectory((dataDirectory / L"UI").wstring());

    Rml::SetSystemInterface(&mSystemInterface);
    Rml::SetRenderInterface(&mRenderInterface);

    if (!Rml::Initialise())
    {
        mLastError = "RmlUiRenderer: Rml::Initialise failed.";
        ReleaseRenderTarget();
        mRenderInterface.Shutdown();
        return false;
    }

    mOwnsRmlLifetime = true;

    if (!LoadFonts())
    {
        // A UI without fonts still renders backgrounds and images, so this is reported
        // but not treated as fatal.
        OutputDebugStringA((mLastError + "\n").c_str());
    }

    mContext = Rml::CreateContext("main", Rml::Vector2i(static_cast<int>(mWidth), static_cast<int>(mHeight)));
    if (mContext == nullptr)
    {
        mLastError = "RmlUiRenderer: failed to create the RmlUi context.";
        Shutdown();
        return false;
    }

    // The debugger plugin is RmlUi's element inspector: it draws into the same context,
    // so it costs nothing while hidden and needs no separate rendering path.
    mDebuggerAvailable = Rml::Debugger::Initialise(mContext);
    if (mDebuggerAvailable)
        Rml::Debugger::SetVisible(false);

    mIsInitialized = true;
    return true;
}

void RmlUiRenderer::SetDebuggerVisible(bool visible)
{
    if (!mDebuggerAvailable)
        return;

    Rml::Debugger::SetVisible(visible);
}

bool RmlUiRenderer::IsDebuggerVisible() const
{
    return mDebuggerAvailable && Rml::Debugger::IsVisible();
}

void RmlUiRenderer::Shutdown()
{
    mDocument = nullptr;
    mLoadedDocumentName.clear();

    if (mContext != nullptr)
    {
        Rml::RemoveContext(mContext->GetName());
        mContext = nullptr;
    }

    if (mOwnsRmlLifetime)
    {
        // Must run before the render interface goes away: shutdown releases every texture
        // and compiled geometry through it.
        Rml::Shutdown();
        mOwnsRmlLifetime = false;
        mDebuggerAvailable = false;
    }

    ReleaseRenderTarget();
    mRenderInterface.Shutdown();
    mIsInitialized = false;
}

bool RmlUiRenderer::LoadFonts()
{
    const std::filesystem::path fontDirectory = FindDataDirectory() / L"Fonts";
    std::error_code errorCode;
    if (!std::filesystem::exists(fontDirectory, errorCode))
    {
        mLastError = "RmlUiRenderer: font directory not found.";
        return false;
    }

    // The face names come from the font files themselves, so documents refer to them with
    // the family name rather than a path.
    const wchar_t* fontFiles[] = {
        L"PlayfairDisplay-Regular.ttf",
        L"PlayfairDisplay-Bold.ttf",
        L"PlayfairDisplay-Italic.ttf",
        L"PlayfairDisplay-BoldItalic.ttf",
    };

    bool loadedAny = false;
    for (const wchar_t* fontFile : fontFiles)
    {
        const std::filesystem::path path = fontDirectory / fontFile;
        if (!std::filesystem::exists(path, errorCode))
            continue;

        if (Rml::LoadFontFace(WideToUtf8(path.wstring())))
            loadedAny = true;
    }

    if (!loadedAny)
    {
        mLastError = "RmlUiRenderer: no font faces could be loaded from Data/Fonts.";
        return false;
    }

    return true;
}

bool RmlUiRenderer::CreateRenderTarget(UINT width, UINT height)
{
    ID3D12Device* device = DX12Context_GetDevice();
    if (device == nullptr)
    {
        mLastError = "RmlUiRenderer: DX12 device unavailable.";
        return false;
    }

    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = width;
    textureDesc.Height = height;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = UiTargetFormat;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue{};
    clearValue.Format = UiTargetFormat;

    const auto heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    if (FAILED(device->CreateCommittedResource(
            &heapProperties,
            D3D12_HEAP_FLAG_NONE,
            &textureDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
            &clearValue,
            IID_PPV_ARGS(&mOutputTexture))))
    {
        mLastError = "RmlUiRenderer: failed to create the UI render target.";
        return false;
    }

    mOutputTexture->SetName(L"RmlUiTarget");

    if (!mRtvHeap)
    {
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = 1;
        if (FAILED(device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&mRtvHeap))))
        {
            mLastError = "RmlUiRenderer: failed to create the UI RTV heap.";
            return false;
        }

        mRtvHandle = mRtvHeap->GetCPUDescriptorHandleForHeapStart();
    }

    device->CreateRenderTargetView(mOutputTexture.Get(), nullptr, mRtvHandle);

    // The SRV slot is allocated once and re-pointed on resize; the shared heap has no
    // free list, so releasing and reallocating on every resize would drain it.
    if (!mSrvAllocated)
    {
        if (!DX12Context_AllocateSrvDescriptor(&mOutputSrvCpuHandle, &mOutputSrvGpuHandle))
        {
            mLastError = "RmlUiRenderer: failed to allocate the UI SRV descriptor.";
            return false;
        }

        mSrvAllocated = true;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Format = UiTargetFormat;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(mOutputTexture.Get(), &srvDesc, mOutputSrvCpuHandle);

    mOutputTextureId = static_cast<UiTextureID>(mOutputSrvGpuHandle.ptr);
    return true;
}

void RmlUiRenderer::ReleaseRenderTarget()
{
    mOutputTexture.Reset();
    mOutputTextureId = UiTextureID_Invalid;
}

bool RmlUiRenderer::Resize(UINT width, UINT height)
{
    width = (std::max)(width, 1u);
    height = (std::max)(height, 1u);

    if (!mIsInitialized || (width == mWidth && height == mHeight))
        return true;

    mWidth = width;
    mHeight = height;

    mOutputTexture.Reset();
    if (!CreateRenderTarget(mWidth, mHeight))
        return false;

    if (mContext != nullptr)
        mContext->SetDimensions(Rml::Vector2i(static_cast<int>(mWidth), static_cast<int>(mHeight)));

    return true;
}

void RmlUiRenderer::Render(ID3D12GraphicsCommandList* commandList)
{
    if (!mIsInitialized || commandList == nullptr || !mOutputTexture)
        return;

    if (mContext != nullptr)
        mContext->Update();

    const auto toRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputTexture.Get(),
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList->ResourceBarrier(1, &toRenderTarget);

    // Transparent black: the UI target is composited over the scene image, so everything
    // the document does not cover has to read as fully see-through.
    const float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    commandList->ClearRenderTargetView(mRtvHandle, clearColor, 0, nullptr);
    commandList->OMSetRenderTargets(1, &mRtvHandle, FALSE, nullptr);

    if (mContext != nullptr && (mIsVisible || mPreviewActive))
    {
        mRenderInterface.BeginFrame(commandList, mWidth, mHeight);
        mContext->Render();
        mRenderInterface.EndFrame();
    }

        const auto toShaderResource = CD3DX12_RESOURCE_BARRIER::Transition(
        mOutputTexture.Get(),
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    commandList->ResourceBarrier(1, &toShaderResource);
}

bool RmlUiRenderer::LoadDocument(const std::string& fileName)
{
    if (!mIsInitialized || mContext == nullptr)
        return false;

    CloseDocument();

    const std::filesystem::path documentPath = FindDataDirectory() / L"UI" / std::filesystem::path(fileName);
    std::error_code errorCode;
    if (!std::filesystem::exists(documentPath, errorCode))
    {
        mLastError = "RmlUiRenderer: document not found: " + fileName;
        return false;
    }

    mDocument = mContext->LoadDocument(WideToUtf8(documentPath.wstring()));
    if (mDocument == nullptr)
    {
        mLastError = "RmlUiRenderer: failed to load document: " + fileName;
        return false;
    }

    mDocument->Show();
    mLoadedDocumentName = fileName;
    // A fresh document starts with no listeners, so tracing has to be re-armed
    // after every load and reload.
    ApplyEventTracing();
    mLastError.clear();
    return true;
}

// ---------------------------------------------------------------------------
// Event tracing
// ---------------------------------------------------------------------------

namespace
{
    // Pointer and activation events only. Tracing mousemove would bury every
    // click under a thousand lines of noise and answer no question anyone asks.
    const char* const kTracedEventTypes[] = {
        "click", "dblclick", "mousedown", "mouseup",
        "mouseover", "mouseout",
        "focus", "blur",
        "change", "submit",
        "keydown"
    };
}

void RmlUiRenderer::SetEventTracingEnabled(bool enabled)
{
    if (mEventTracingEnabled == enabled)
        return;

    mEventTracingEnabled = enabled;
    ApplyEventTracing();
}

void RmlUiRenderer::ApplyEventTracing()
{
    if (mDocument == nullptr)
        return;

    if (mEventTracingEnabled)
    {
        if (!mTraceListener)
            mTraceListener = std::make_unique<TraceListener>(*this);
        for (const char* type : kTracedEventTypes)
            mDocument->AddEventListener(type, mTraceListener.get(), true);
    }
    else if (mTraceListener)
    {
        for (const char* type : kTracedEventTypes)
            mDocument->RemoveEventListener(type, mTraceListener.get(), true);
    }
}

std::vector<std::string> RmlUiRenderer::ListAvailableDocuments()
{
    std::vector<std::string> documents;

    const std::filesystem::path uiDirectory = FindDataDirectory() / L"UI";
    std::error_code errorCode;
    if (!std::filesystem::is_directory(uiDirectory, errorCode))
        return documents;

    for (const auto& entry : std::filesystem::directory_iterator(uiDirectory, errorCode))
    {
        if (!entry.is_regular_file(errorCode))
            continue;
        std::filesystem::path extension = entry.path().extension();
        std::wstring lowered = extension.wstring();
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (lowered != L".rml")
            continue;
        documents.push_back(WideToUtf8(entry.path().filename().wstring()));
    }

    std::sort(documents.begin(), documents.end());
    return documents;
}

void RmlUiRenderer::CloseDocument()
{
    if (mDocument != nullptr)
    {
        mDocument->Close();
        mDocument = nullptr;
    }

    mLoadedDocumentName.clear();
}

bool RmlUiRenderer::ReloadDocument()
{
    if (mLoadedDocumentName.empty())
        return false;

    // Copy first: LoadDocument closes the current document, which clears the name.
    const std::string fileName = mLoadedDocumentName;
    return LoadDocument(fileName);
}

namespace
{
    // Shared by every element edit below. Records nothing on failure: the caller reports
    // it, and a graph polling an element every frame must not flood the error string.
    Rml::Element* FindElement(Rml::ElementDocument* document, const std::string& elementId)
    {
        if (document == nullptr || elementId.empty())
            return nullptr;

        return document->GetElementById(Rml::String(elementId.c_str()));
    }
}

bool RmlUiRenderer::SetElementText(const std::string& elementId, const std::string& text)
{
    Rml::Element* element = FindElement(mDocument, elementId);
    if (element == nullptr)
        return false;

    element->SetInnerRML(Rml::String(text.c_str()));
    return true;
}

bool RmlUiRenderer::SetElementProperty(
    const std::string& elementId,
    const std::string& property,
    const std::string& value)
{
    Rml::Element* element = FindElement(mDocument, elementId);
    if (element == nullptr)
        return false;

    return element->SetProperty(Rml::String(property.c_str()), Rml::String(value.c_str()));
}

bool RmlUiRenderer::SetElementClass(const std::string& elementId, const std::string& className, bool enabled)
{
    Rml::Element* element = FindElement(mDocument, elementId);
    if (element == nullptr || className.empty())
        return false;

    element->SetClass(Rml::String(className.c_str()), enabled);
    return true;
}

bool RmlUiRenderer::SetElementVisible(const std::string& elementId, bool visible)
{
    Rml::Element* element = FindElement(mDocument, elementId);
    if (element == nullptr)
        return false;

    // Showing removes the inline override rather than forcing "block", so an element the
    // stylesheet lays out as flex comes back the way it was authored.
    if (visible)
        element->RemoveProperty("display");
    else
        element->SetProperty("display", "none");

    return true;
}

int RmlUiRenderer::GetKeyModifierState() const
{
    int modifiers = 0;
    if ((GetKeyState(VK_SHIFT) & 0x8000) != 0)
        modifiers |= Rml::Input::KM_SHIFT;
    if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
        modifiers |= Rml::Input::KM_CTRL;
    if ((GetKeyState(VK_MENU) & 0x8000) != 0)
        modifiers |= Rml::Input::KM_ALT;
    if ((GetKeyState(VK_CAPITAL) & 0x0001) != 0)
        modifiers |= Rml::Input::KM_CAPSLOCK;
    if ((GetKeyState(VK_NUMLOCK) & 0x0001) != 0)
        modifiers |= Rml::Input::KM_NUMLOCK;
    return modifiers;
}

void RmlUiRenderer::SetMouseState(
    bool hasFocus,
    float x,
    float y,
    bool leftDown,
    bool rightDown,
    bool middleDown)
{
    if (!mIsInitialized || mContext == nullptr || !mInputEnabled)
        return;

    const int modifiers = GetKeyModifierState();

    if (!hasFocus)
    {
        if (mHasMouseFocus)
        {
            // Release anything still held before leaving, otherwise a button pressed
            // inside the UI stays latched down forever.
            for (int button = 0; button < 3; ++button)
            {
                if (mMouseButtonDown[button])
                {
                    mContext->ProcessMouseButtonUp(button, modifiers);
                    mMouseButtonDown[button] = false;
                }
            }

            mContext->ProcessMouseLeave();
            mHasMouseFocus = false;
        }

        return;
    }

    mHasMouseFocus = true;
    mContext->ProcessMouseMove(static_cast<int>(x), static_cast<int>(y), modifiers);

    const bool buttons[3] = { leftDown, rightDown, middleDown };
    for (int button = 0; button < 3; ++button)
    {
        if (buttons[button] == mMouseButtonDown[button])
            continue;

        if (buttons[button])
            mContext->ProcessMouseButtonDown(button, modifiers);
        else
            mContext->ProcessMouseButtonUp(button, modifiers);

        mMouseButtonDown[button] = buttons[button];
    }
}

void RmlUiRenderer::UpdateKeyboardInput()
{
    if (!mIsInitialized || mContext == nullptr || !mInputEnabled)
        return;

    // Only take keys while the editor is the foreground window, so a background editor
    // cannot steal input from whatever the user is actually typing into.
    if (GetForegroundWindow() != DX12Context_GetWindowHandle())
        return;

    const int modifiers = GetKeyModifierState();

    BYTE keyboardState[256] = {};
    const bool haveKeyboardState = GetKeyboardState(keyboardState) != FALSE;

    for (const KeyMapping& mapping : KeyMappings)
    {
        const bool isDown = (GetAsyncKeyState(mapping.VirtualKey) & 0x8000) != 0;
        if (isDown == mKeyDown[mapping.VirtualKey])
            continue;

        mKeyDown[mapping.VirtualKey] = isDown;
        if (isDown)
            mContext->ProcessKeyDown(mapping.Identifier, modifiers);
        else
            mContext->ProcessKeyUp(mapping.Identifier, modifiers);
    }

    if (!haveKeyboardState)
        return;

    // Printable characters: translate on the press edge through the active keyboard
    // layout so text input honours the user's layout instead of assuming US ASCII.
    for (int virtualKey = 0x20; virtualKey <= 0xDF; ++virtualKey)
    {
        const bool isDown = (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
        const bool wasDown = mTextKeyDown[virtualKey];
        mTextKeyDown[virtualKey] = isDown;

        if (!isDown || wasDown)
            continue;

        // Ctrl and Alt combinations are shortcuts, not text.
        if ((modifiers & (Rml::Input::KM_CTRL | Rml::Input::KM_ALT)) != 0)
            continue;

        wchar_t characters[8] = {};
        const UINT scanCode = MapVirtualKeyW(static_cast<UINT>(virtualKey), MAPVK_VK_TO_VSC);
        // Flag 0x4 asks ToUnicode not to disturb the layout's dead-key state, which
        // matters because this runs every frame rather than from a key message.
        const int written = ToUnicode(
            static_cast<UINT>(virtualKey), scanCode, keyboardState, characters,
            static_cast<int>(std::size(characters)), 0x4);

        for (int i = 0; i < written; ++i)
        {
            if (characters[i] >= L' ')
                mContext->ProcessTextInput(static_cast<Rml::Character>(characters[i]));
        }
    }
}
