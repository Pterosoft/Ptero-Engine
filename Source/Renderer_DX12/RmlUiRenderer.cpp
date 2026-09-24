#include "pch.h"

#include "RmlUiRenderer.h"
#include "../../Data/UI/Farkle/FarkleMenuController.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Debugger.h>

#include "System/DataFiles.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <chrono>
#include <filesystem>
#include <memory>
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
        // The repository's Data folder, or a packaged game's virtual one (DataFiles.h).
        const std::filesystem::path dataDirectory = DataFiles::FindDataDirectory();
        return dataDirectory.empty() ? std::filesystem::path(L"Data") : dataDirectory;
    }

    // The menus lay out in vh, but ninepatch edges are drawn in dp: they are tuned for 1080
    // lines, and scaling the dp ratio with the target keeps them in proportion to the rest
    // of the layout at any resolution (Data/UI/Farkle/menus.rcss).
    float DensityRatioFor(UINT height)
    {
        return (std::max)(0.25f, static_cast<float>(height) / 1080.0f);
    }

    // RmlUi 6.3 hands @font-face src paths over verbatim, not relative to the stylesheet,
    // so the Farkle stylesheet names its fonts relative to the Data root
    // ("UI/Farkle/Fonts/...") and they are resolved here. Every other path RmlUi opens is
    // already absolute.
    std::filesystem::path Utf8ToPath(const Rml::String& text)
    {
        std::filesystem::path path = std::filesystem::u8path(text);
        if (path.is_relative())
            path = FindDataDirectory() / path;
        return path;
    }

    // Every file RmlUi opens - documents, style sheets, fonts, templates - comes through
    // here, so a packaged game reads them out of UI.ppak and Fonts.ppak. On disk it is the
    // same as RmlUi's default fopen-based interface.
    class DataFilesRmlFileInterface final : public Rml::FileInterface
    {
    public:
        Rml::FileHandle Open(const Rml::String& path) override
        {
            auto file = std::make_unique<OpenFile>();
            if (!DataFiles::ReadBytes(Utf8ToPath(path), file->Bytes))
                return 0;
            return reinterpret_cast<Rml::FileHandle>(file.release());
        }

        void Close(Rml::FileHandle file) override
        {
            delete reinterpret_cast<OpenFile*>(file);
        }

        size_t Read(void* buffer, size_t size, Rml::FileHandle handle) override
        {
            OpenFile* file = reinterpret_cast<OpenFile*>(handle);
            const size_t available = file->Bytes.size() - file->Position;
            const size_t count = (std::min)(size, available);
            if (count > 0)
                std::memcpy(buffer, file->Bytes.data() + file->Position, count);
            file->Position += count;
            return count;
        }

        bool Seek(Rml::FileHandle handle, long offset, int origin) override
        {
            OpenFile* file = reinterpret_cast<OpenFile*>(handle);
            long long base = 0;
            if (origin == SEEK_CUR) base = static_cast<long long>(file->Position);
            else if (origin == SEEK_END) base = static_cast<long long>(file->Bytes.size());
            const long long target = base + offset;
            if (target < 0 || target > static_cast<long long>(file->Bytes.size()))
                return false;
            file->Position = static_cast<size_t>(target);
            return true;
        }

        size_t Tell(Rml::FileHandle handle) override
        {
            return reinterpret_cast<OpenFile*>(handle)->Position;
        }

        size_t Length(Rml::FileHandle handle) override
        {
            return reinterpret_cast<OpenFile*>(handle)->Bytes.size();
        }

    private:
        struct OpenFile
        {
            std::vector<std::uint8_t> Bytes;
            size_t Position = 0;
        };
    };

    DataFilesRmlFileInterface gRmlFileInterface;

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

class RmlUiRenderer::ActionListener final : public Rml::EventListener
{
public:
    explicit ActionListener(RmlUiRenderer& owner) : mOwner(owner) {}
    void ProcessEvent(Rml::Event& event) override
    {
        if (event.GetType() == "keydown")
        {
            const auto key = event.GetParameter<int>("key_identifier", 0);
            if (key != Rml::Input::KI_RETURN && key != Rml::Input::KI_SPACE) return;
        }
        auto* element = event.GetTargetElement();
        while (element && element->GetTagName() != "button") element = element->GetParentNode();
        if (!element || element->HasAttribute("disabled") || element->GetId().empty()) return;
        if (mOwner.mGameActions.size() < 32) mOwner.mGameActions.push_back(element->GetId());
        // The node graph gets its own copy: it and the game module both react to buttons,
        // and a shared queue would hand each click to whichever polled first.
        if (mOwner.mGraphClicks.size() < 32) mOwner.mGraphClicks.push_back(element->GetId());
        if (event.GetType() == "keydown") event.StopPropagation();
    }
private:
    RmlUiRenderer& mOwner;
};

// UI audio. Sits alongside ActionListener rather than inside it because it answers a
// different question: ActionListener reports what the player asked the game to do, this
// reports that the player touched a control at all - including hovers, which never reach
// the game module, and including clicks on controls the game has disabled.
class RmlUiRenderer::SoundListener final : public Rml::EventListener
{
public:
    explicit SoundListener(RmlUiRenderer& owner) : mOwner(owner) {}

    void ProcessEvent(Rml::Event& event) override
    {
        if (!mOwner.mUiSoundCallback) return;

        auto* element = event.GetTargetElement();
        while (element && element->GetTagName() != "button") element = element->GetParentNode();

        const bool click = event.GetType() == "click";
        if (!element || element->HasAttribute("disabled"))
        {
            // The pointer left every button, so the next one it enters is a fresh hover.
            if (!click) mHovered = nullptr;
            return;
        }

        if (!click)
        {
            // mouseover also fires for the spans inside a button, and again on the way
            // back out to it. Only the first entry into a given button makes a sound.
            if (mHovered == element) return;
            mHovered = element;
        }
        mOwner.mUiSoundCallback(click);
    }

    void Reset() { mHovered = nullptr; }

private:
    RmlUiRenderer& mOwner;
    const Rml::Element* mHovered = nullptr; // Compared, never dereferenced.
};

std::string RmlUiRenderer::PollGameAction()
{
    if (mGameActions.empty()) return {};
    std::string result = std::move(mGameActions.front());
    mGameActions.erase(mGameActions.begin());
    return result;
}

bool RmlUiRenderer::PollGraphClick(std::string& elementId)
{
    if (mGraphClicks.empty()) return false;
    elementId = std::move(mGraphClicks.front());
    mGraphClicks.erase(mGraphClicks.begin());
    return true;
}

int RmlUiRenderer::GetFarkleWinningScore() const
{
    return mFarkleMenus ? std::stoi(mFarkleMenus->Saved().at("max-score")) : 3000;
}

FarkleMenuController& RmlUiRenderer::GetFarkleMenus()
{
    if (!mFarkleMenus) mFarkleMenus = std::make_unique<FarkleMenuController>();
    return *mFarkleMenus;
}

void RmlUiRenderer::ApplyGameUiCommand(int operation, const char* id, const char* value)
{
    if (!mDocument) return;
    auto* element = mDocument->GetElementById(id);
    if (!element) return;
    switch (operation)
    {
    case 0: SetElementText(id, value); break;
    case 1: element->SetInnerRML(value); break;
    case 2: element->SetClass(value, true); break;
    case 3: element->SetClass(value, false); break;
    case 4: element->RemoveAttribute("disabled"); break;
    case 5: element->SetAttribute("disabled", true); break;
    }
    // Keep tab navigation inside an open pause/help dialog.
    if (std::string(id) == "dialog" && std::string(value) == "hidden")
    {
        auto* root = mDocument->GetElementById("game-content");
        if (root) root->SetProperty("display", operation == 3 ? "none" : "block");
        auto* focus = mDocument->GetElementById(operation == 3 ? "close-button" : "roll-button");
        if (focus) focus->Focus();
    }
}

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
    Rml::SetFileInterface(&gRmlFileInterface);

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
    if (mContext != nullptr)
        mContext->SetDensityIndependentPixelRatio(DensityRatioFor(mHeight));
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

void RmlUiRenderer::SetStatisticsOverlay(bool visible, const std::string& text)
{
    if (!mIsInitialized || mContext == nullptr)
        return;

    if (!visible)
    {
        if (mStatisticsDocument != nullptr && mStatisticsDocument->IsVisible())
            mStatisticsDocument->Hide();
        return;
    }

    if (mStatisticsDocument == nullptr)
    {
        if (mStatisticsDocumentFailed)
            return;
        const std::filesystem::path path = FindDataDirectory() / L"UI" / L"stats-overlay.rml";
        mStatisticsDocument = mContext->LoadDocument(WideToUtf8(path.wstring()));
        if (mStatisticsDocument == nullptr)
        {
            // Not retried every frame: a missing file would otherwise be reloaded 60 times a second.
            mStatisticsDocumentFailed = true;
            mLastError = "RmlUiRenderer: could not load UI/stats-overlay.rml.";
            return;
        }
        mStatisticsText.clear();
    }

    if (text != mStatisticsText)
    {
        mStatisticsText = text;
        std::string rml;
        for (const char c : text)
        {
            switch (c)
            {
            case '\n': rml += "<br/>"; break;
            case '<': rml += "&lt;"; break;
            case '>': rml += "&gt;"; break;
            case '&': rml += "&amp;"; break;
            default: rml += c; break;
            }
        }
        if (Rml::Element* element = mStatisticsDocument->GetElementById("stats-text"))
            element->SetInnerRML(rml);
    }

    if (!mStatisticsDocument->IsVisible())
        mStatisticsDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
    // A page loaded after the overlay would otherwise cover it.
    mStatisticsDocument->PullToFront();
}

void RmlUiRenderer::Shutdown()
{
    if (mFarkleMenus) mFarkleMenus->Detach();
    mDocument = nullptr;
    mStatisticsDocument = nullptr;
    mStatisticsDocumentFailed = false;
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
    if (!DataFiles::IsDirectory(fontDirectory))
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
        if (!DataFiles::IsFile(path))
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
    {
        mContext->SetDimensions(Rml::Vector2i(static_cast<int>(mWidth), static_cast<int>(mHeight)));
        mContext->SetDensityIndependentPixelRatio(DensityRatioFor(mHeight));
    }

    return true;
}

void RmlUiRenderer::Render(ID3D12GraphicsCommandList* commandList)
{
    if (!mIsInitialized || commandList == nullptr || !mOutputTexture)
        return;

    // Navigation is deferred until DOM event dispatch has finished.
    if (mFarkleMenus) {
        const auto page = mFarkleMenus->TakePage();
        if (!page.empty()) LoadDocument(page);
    }
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

    const std::filesystem::path relativePath = std::filesystem::u8path(fileName).lexically_normal();
    const std::filesystem::path documentPath = FindDataDirectory() / L"UI" / relativePath;
    if (!DataFiles::IsFile(documentPath))
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
    GetFarkleMenus().Attach(mDocument);
    if (!mActionListener) mActionListener = std::make_unique<ActionListener>(*this);
    mDocument->AddEventListener("click", mActionListener.get());
    mDocument->AddEventListener("keydown", mActionListener.get());
    // Capture phase: a button that swallows its own click still makes a sound.
    if (!mSoundListener) mSoundListener = std::make_unique<SoundListener>(*this);
    mSoundListener->Reset();
    mDocument->AddEventListener("click", mSoundListener.get(), true);
    mDocument->AddEventListener("mouseover", mSoundListener.get(), true);
    mLoadedDocumentName = WideToUtf8(relativePath.generic_wstring());
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
    if (!DataFiles::IsDirectory(uiDirectory))
        return documents;

    // Preserve paths relative to Data/UI: separate folders may contain documents
    // with the same filename. The listing never throws, so an inaccessible folder
    // cannot take down the editor while it refreshes the picker.
    for (const std::filesystem::path& file : DataFiles::ListFiles(uiDirectory, true))
    {
        std::wstring lowered = file.extension().wstring();
        std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (lowered != L".rml")
            continue;
        documents.push_back(WideToUtf8(file.lexically_relative(uiDirectory).generic_wstring()));
    }

    std::sort(documents.begin(), documents.end());
    return documents;
}

void RmlUiRenderer::CloseDocument()
{
    if (mFarkleMenus) mFarkleMenus->Detach();
    mGameActions.clear();
    mGraphClicks.clear();
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

    // The presentation HWND is a native child of either the editor or Play window.
    if (GetAncestor(GetForegroundWindow(), GA_ROOT) != GetAncestor(DX12Context_GetWindowHandle(), GA_ROOT))
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
