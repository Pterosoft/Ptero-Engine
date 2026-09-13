// ---------------------------------------------------------------------------
// The UI Editor.
//
// A viewer for RmlUi documents: load one from Data/UI, see it drawn at a real
// size, click it, and watch what the click actually did. That last part is the
// reason the panel exists. A document that renders correctly and responds to
// nothing is the normal failure, and the screen looks identical whether the
// button has no handler or the click never reached it. The event trace tells
// those apart by looking.
//
// The preview is the same RmlUi target the game composites over the viewport,
// shown through QtUi::Image - a native child surface that QtViewportRenderer
// blits into, because the Qt draw list cannot paint a live GPU texture. Mouse
// position is mapped from that surface into UI pixels, so hit testing is the
// real thing and not an approximation.
// ---------------------------------------------------------------------------

#include "pch.h"
#include "Editor.h"

#include "DX12SceneRenderer.h"
#include "RmlUiRenderer.h"
#include "System/PteroLog.h"

#include "../QtUi/QtUi.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    constexpr const char* kCategory = "UI";

    // Fixed preview sizes, never derived from the available content region.
    // Sizing a QtUi widget from GetContentRegionAvail feeds the window's own
    // minimum size back into its content and the panel grows without bound
    // every frame - the bug the G-Buffer debug window had.
    struct PreviewSize
    {
        const char* Label;
        float Width;
        float Height;
    };
    constexpr PreviewSize kPreviewSizes[] = {
        { "480 x 270",  480.0f, 270.0f },
        { "640 x 360",  640.0f, 360.0f },
        { "960 x 540",  960.0f, 540.0f },
        { "1280 x 720", 1280.0f, 720.0f },
    };
    constexpr int kPreviewSizeCount = static_cast<int>(std::size(kPreviewSizes));
}

void Editor::DrawUiEditorPanel()
{
    if (!QtUi::Begin("UI Editor", &mShowUiEditorPanel))
    {
        QtUi::End();
        return;
    }

    if (mSceneRenderer == nullptr)
    {
        QtUi::TextDisabled("No renderer.");
        QtUi::End();
        return;
    }

    RmlUiRenderer& ui = mSceneRenderer->GetRmlUiRenderer();

    if (!ui.IsInitialized())
    {
        QtUi::TextColored(UiVec4{ 0.95f, 0.45f, 0.4f, 1.0f }, "RmlUi failed to initialize.");
        QtUi::TextWrapped("%s", ui.GetLastErrorMessage() ? ui.GetLastErrorMessage() : "No reason was reported.");
        QtUi::End();
        return;
    }

    // ------------------------------------------------------------- document
    // Rescanned every frame the panel is open, so a file written a second ago
    // is in the list without reopening anything. The directory holds a handful
    // of documents; this is not a cost worth caching against.
    const std::vector<std::string> documents = RmlUiRenderer::ListAvailableDocuments();

    if (documents.empty())
    {
        QtUi::TextDisabled("No .rml documents found under Data/UI.");
    }
    else
    {
        std::vector<const char*> items;
        items.reserve(documents.size());
        for (const std::string& document : documents)
            items.push_back(document.c_str());

        // Keep the combo pointing at whatever is actually loaded, so an
        // external load (game code, a node graph) is reflected here.
        const std::string& loaded = ui.GetLoadedDocumentName();
        if (!loaded.empty())
        {
            const auto it = std::find(documents.begin(), documents.end(), loaded);
            if (it != documents.end())
                mUiEditorDocumentIndex = static_cast<int>(std::distance(documents.begin(), it));
        }
        if (mUiEditorDocumentIndex >= static_cast<int>(documents.size()))
            mUiEditorDocumentIndex = 0;

        QtUi::SetNextItemWidth(240.0f);
        QtUi::Combo("Document", &mUiEditorDocumentIndex, items.data(), static_cast<int>(items.size()));

        QtUi::SameLine();
        if (QtUi::Button("Load"))
        {
            const std::string& chosen = documents[static_cast<std::size_t>(mUiEditorDocumentIndex)];
            if (ui.LoadDocument(chosen))
                PTERO_LOG_INFO(kCategory, "Loaded '%s'.", chosen.c_str());
            else
                PTERO_LOG_ERROR(kCategory, "%s",
                    ui.GetLastErrorMessage() ? ui.GetLastErrorMessage() : "Load failed with no reason reported.");
        }

        QtUi::SameLine();
        QtUi::BeginDisabled(ui.GetLoadedDocumentName().empty());
        if (QtUi::Button("Reload"))
        {
            // Documents and their .rcss are read from disk on load, so this is
            // the whole authoring loop: edit, reload, click.
            const std::string reloaded = ui.GetLoadedDocumentName();
            if (ui.ReloadDocument())
                PTERO_LOG_INFO(kCategory, "Reloaded '%s'.", reloaded.c_str());
            else
                PTERO_LOG_ERROR(kCategory, "%s",
                    ui.GetLastErrorMessage() ? ui.GetLastErrorMessage() : "Reload failed with no reason reported.");
        }
        QtUi::SameLine();
        if (QtUi::Button("Close"))
        {
            ui.CloseDocument();
            PTERO_LOG_INFO(kCategory, "Closed the document.");
        }
        QtUi::EndDisabled();
    }

    // -------------------------------------------------------------- controls
    QtUi::SetNextItemWidth(140.0f);
    std::vector<const char*> sizeLabels;
    sizeLabels.reserve(kPreviewSizeCount);
    for (const PreviewSize& size : kPreviewSizes)
        sizeLabels.push_back(size.Label);
    QtUi::Combo("Preview", &mUiEditorPreviewSizeIndex, sizeLabels.data(), kPreviewSizeCount);

    QtUi::SameLine();
    if (QtUi::Checkbox("Interactive", &mUiEditorInteractive) && !mUiEditorInteractive)
    {
        // Dropping focus rather than just ignoring input, so a hover or pressed
        // state cannot stay latched on an element the pointer has left.
        ui.SetMouseState(false, 0.0f, 0.0f, false, false, false);
    }

    if (ui.IsDebuggerAvailable())
    {
        QtUi::SameLine();
        bool debuggerVisible = ui.IsDebuggerVisible();
        if (QtUi::Checkbox("Inspector", &debuggerVisible))
            ui.SetDebuggerVisible(debuggerVisible);
        QtUi::SetItemTooltip("RmlUi's own element inspector: the DOM, computed styles and the box model.");
    }

    QtUi::SameLine();
    bool tracing = ui.IsEventTracingEnabled();
    if (QtUi::Checkbox("Trace events", &tracing))
        ui.SetEventTracingEnabled(tracing);
    QtUi::SetItemTooltip("Records the events that actually reach the document, so a dead button is "
                         "told apart from a click that never arrived.");

    // --------------------------------------------------------------- preview
    const PreviewSize& previewSize = kPreviewSizes[
        static_cast<std::size_t>(std::clamp(mUiEditorPreviewSizeIndex, 0, kPreviewSizeCount - 1))];

    const UiVec2 imageOrigin = QtUi::GetCursorScreenPos();
    QtUi::Image(ui.GetOutputTextureId(), UiVec2{ previewSize.Width, previewSize.Height });
    const bool previewHovered = QtUi::IsItemHovered();

    // Write mouse state only while the pointer is actually over the preview,
    // plus the one frame it leaves. The viewport's own overlay writes this too,
    // so an unconditional call here would clear the game UI's focus every frame
    // while a play session is running with this panel open.
    if (mUiEditorInteractive && (previewHovered || mUiEditorHadPointer))
    {
        // The UI target is sized to the render resolution, which is not the
        // size it is being shown at, so pointer coordinates are scaled into UI
        // pixels before RmlUi sees them. Without this the hit testing would be
        // right only at one particular preview size.
        const UiVec2 mouse = QtUi::GetIO().MousePos;
        const float scaleX = static_cast<float>(ui.GetWidth()) / previewSize.Width;
        const float scaleY = static_cast<float>(ui.GetHeight()) / previewSize.Height;

        ui.SetMouseState(
            previewHovered,
            (mouse.x - imageOrigin.x) * scaleX,
            (mouse.y - imageOrigin.y) * scaleY,
            previewHovered && QtUi::IsMouseDown(QtUiMouseButton_Left),
            previewHovered && QtUi::IsMouseDown(QtUiMouseButton_Right),
            previewHovered && QtUi::IsMouseDown(QtUiMouseButton_Middle));
    }
    mUiEditorHadPointer = mUiEditorInteractive && previewHovered;

    // ---------------------------------------------------------------- status
    if (ui.GetLoadedDocumentName().empty())
        QtUi::TextDisabled("No document loaded. Target %u x %u.", ui.GetWidth(), ui.GetHeight());
    else
        QtUi::TextDisabled("%s - target %u x %u, shown at %.0f x %.0f.",
                           ui.GetLoadedDocumentName().c_str(), ui.GetWidth(), ui.GetHeight(),
                           previewSize.Width, previewSize.Height);

    if (ui.GetLastErrorMessage() != nullptr)
        QtUi::TextColored(UiVec4{ 0.95f, 0.45f, 0.4f, 1.0f }, "%s", ui.GetLastErrorMessage());

    // -------------------------------------------------------------- elements
    // The same four calls game code and the node graph's UI nodes make, so
    // poking an element here proves the path they use, not a parallel one.
    if (QtUi::CollapsingHeader("Element"))
    {
        QtUi::SetNextItemWidth(200.0f);
        QtUi::InputText("Id", mUiEditorElementId, sizeof(mUiEditorElementId));

        const std::string elementId = mUiEditorElementId;
        QtUi::BeginDisabled(elementId.empty() || ui.GetLoadedDocumentName().empty());

        QtUi::SetNextItemWidth(200.0f);
        QtUi::InputText("Text", mUiEditorElementText, sizeof(mUiEditorElementText));
        QtUi::SameLine();
        if (QtUi::Button("Set text"))
        {
            const bool ok = ui.SetElementText(elementId, mUiEditorElementText);
            PTERO_LOG_INFO(kCategory, "SetElementText('%s') %s.", elementId.c_str(), ok ? "succeeded" : "found no such element");
        }

        QtUi::SetNextItemWidth(140.0f);
        QtUi::InputText("Property", mUiEditorPropertyName, sizeof(mUiEditorPropertyName));
        QtUi::SameLine();
        QtUi::SetNextItemWidth(140.0f);
        QtUi::InputText("Value", mUiEditorPropertyValue, sizeof(mUiEditorPropertyValue));
        QtUi::SameLine();
        if (QtUi::Button("Set property"))
        {
            const bool ok = ui.SetElementProperty(elementId, mUiEditorPropertyName, mUiEditorPropertyValue);
            PTERO_LOG_INFO(kCategory, "SetElementProperty('%s', '%s') %s.",
                           elementId.c_str(), mUiEditorPropertyName, ok ? "succeeded" : "found no such element");
        }

        QtUi::SetNextItemWidth(140.0f);
        QtUi::InputText("Class", mUiEditorClassName, sizeof(mUiEditorClassName));
        QtUi::SameLine();
        if (QtUi::Button("Add class"))
            ui.SetElementClass(elementId, mUiEditorClassName, true);
        QtUi::SameLine();
        if (QtUi::Button("Remove class"))
            ui.SetElementClass(elementId, mUiEditorClassName, false);

        if (QtUi::Button("Show"))
            ui.SetElementVisible(elementId, true);
        QtUi::SameLine();
        if (QtUi::Button("Hide"))
            ui.SetElementVisible(elementId, false);

        QtUi::EndDisabled();
    }

    // ----------------------------------------------------------- event trace
    if (QtUi::CollapsingHeader("Event trace"))
    {
        if (!ui.IsEventTracingEnabled())
        {
            QtUi::TextDisabled("Tracing is off. Turn it on, then click the preview.");
        }
        else
        {
            if (QtUi::Button("Clear trace"))
                ui.ClearTracedEvents();

            const std::vector<RmlUiRenderer::TracedEvent>& events = ui.GetTracedEvents();
            if (events.empty())
            {
                QtUi::TextDisabled("Nothing yet. A click that produces no line here never reached the document.");
            }
            else
            {
                // Newest first: the event you just caused is the one you want
                // to read, and it would otherwise be off the bottom.
                const std::size_t shown = (std::min)(events.size(), static_cast<std::size_t>(20));
                for (std::size_t i = 0; i < shown; ++i)
                {
                    const RmlUiRenderer::TracedEvent& traced = events[events.size() - 1 - i];
                    const std::string target = traced.ElementId.empty()
                        ? traced.ElementTag
                        : traced.ElementTag + "#" + traced.ElementId;
                    QtUi::Text("%7.2f  %-10s  %s", traced.TimeSeconds, traced.Type.c_str(), target.c_str());
                }
                if (events.size() > shown)
                    QtUi::TextDisabled("... and %llu earlier.",
                                       static_cast<unsigned long long>(events.size() - shown));
            }
        }
    }

    QtUi::End();
}
