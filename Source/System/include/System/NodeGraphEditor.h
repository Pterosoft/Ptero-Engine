#pragma once

#include "System/NodeGraphDocument.h"

#include <string>

// The engine's handle on the Node Graph window.
//
// The window itself is written directly against Qt and the QtNodes SDK; this header is
// what the rest of the editor talks to, so nothing outside NodeGraphEditorWindow.cpp has
// to include either. Every function is safe to call before the window exists - and before
// Qt is initialised - which is what lets a level be loaded during startup and the graph
// simply be waiting when the user first opens the window.
//
// All of these must be called from the thread that owns the Qt UI, which for this editor
// means the main thread. Scene loading happens on a worker, so the loaded graph is handed
// over in Editor::UpdateSceneLoading rather than by the worker itself.
namespace NodeGraphEditor
{
    void Show();
    void Hide();
    bool IsVisible();

    // Destroys the window. Called from Editor::Shutdown, before QtUi tears Qt down.
    void Shutdown();

    // The graph the level owns. Reading it is how the level serializer and the runtime
    // get at the current state, including edits the user has not saved yet.
    const NodeGraphDocument& Document();

    // Replaces the graph, e.g. when a level is opened or a new one is created. Resets the
    // revision baseline: this is a load, not an edit.
    void SetDocument(const NodeGraphDocument& document);

    // Bumped on every user edit. The editor compares it against the value from the last
    // level save to decide whether the level has unsaved graph changes.
    unsigned Revision();

    // Last .nodegraph the window imported from or exported to, empty when the graph has
    // only ever lived inside the level.
    const std::string& ExportPath();
}
