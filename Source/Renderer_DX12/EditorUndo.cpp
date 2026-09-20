// ---------------------------------------------------------------------------
// Undo and redo.
//
// Snapshot-based rather than command-based, which is the right trade for this
// editor: the level is one `std::vector<Entity>` of strings, POD and shared_ptr
// handles - the mesh data itself is shared, never copied - so a whole-scene
// snapshot costs microseconds, while a command system would need an undoable
// counterpart for all 182 places that currently mutate the scene, and would be
// wrong in exactly the places nobody remembered to write one for.
//
// The recording point is `MarkSceneChanged()`, which every mutation already
// called (as `mSceneDirty = true`) to light the unsaved-changes flag. That is
// what makes the coverage complete without auditing each site: if an edit marks
// the level dirty, it is undoable, and if it does not, it was never going to be
// saved either.
//
// Since that call arrives *after* the change, the snapshot cannot be taken
// there. Instead a baseline of the last committed state is kept alongside, and
// a change pushes the baseline - the state as it was before - then refreshes
// it. The stack therefore holds pre-edit states without any call site having to
// know it is being recorded.
// ---------------------------------------------------------------------------

#include "pch.h"
#include "Editor.h"

#include "System/PteroLog.h"

#include <algorithm>
#include <utility>

namespace
{
    constexpr const char* kCategory = "Editor";
}

Editor::SceneUndoState Editor::CaptureSceneState() const
{
    SceneUndoState state;
    state.Entities = mEntities;
    state.SelectedEntityIndex = mSelectedEntityIndex;
    state.SelectedEntityIndices = mSelectedEntityIndices;
    return state;
}

void Editor::ApplySceneState(const SceneUndoState& state)
{
    mEntities = state.Entities;
    mSelectedEntityIndex = state.SelectedEntityIndex;
    mSelectedEntityIndices = state.SelectedEntityIndices;

    // The state was captured from a scene that may have had more entities than
    // this one does, and a stale index would be read straight out of bounds by
    // the inspector.
    const int entityCount = static_cast<int>(mEntities.size());
    if (mSelectedEntityIndex >= entityCount)
        mSelectedEntityIndex = entityCount > 0 ? entityCount - 1 : -1;
    mSelectedEntityIndices.erase(
        std::remove_if(mSelectedEntityIndices.begin(), mSelectedEntityIndices.end(),
                       [entityCount](int index) { return index < 0 || index >= entityCount; }),
        mSelectedEntityIndices.end());

    // Any drag in progress refers to entities that have just been replaced.
    mViewportSelection = ViewportSelectionState{};
    mManualGizmo = ManualGizmoState{};
}

void Editor::MarkSceneChanged()
{
    mSceneDirty = true;

    // Undo and redo rewrite the scene themselves; recording that would make
    // every undo push a redo-shaped step and the two would never converge.
    if (mApplyingUndoState)
        return;

    const auto now = std::chrono::steady_clock::now();

    // A gizmo drag or a slider held down calls this every frame. Without
    // coalescing, one drag would fill the entire history with sixty
    // indistinguishable steps and push the state worth returning to off the end
    // of it. Edits closer together than the window are treated as one: the
    // baseline still advances every frame, so the *next* distinct edit records
    // the finished result and nothing is lost at either end of the drag.
    if (now - mLastSceneChangeTime >= kUndoCoalesceWindow)
    {
        mUndoStack.push_back(std::move(mUndoBaseline));
        while (mUndoStack.size() > kMaxUndoSteps)
            mUndoStack.pop_front();

        // A new edit after an undo is a new branch of history; the redo path it
        // would have returned to no longer exists.
        mRedoStack.clear();
    }

    mLastSceneChangeTime = now;
    mUndoBaseline = CaptureSceneState();
}

bool Editor::Undo()
{
    if (mUndoStack.empty())
        return false;

    mRedoStack.push_back(CaptureSceneState());
    while (mRedoStack.size() > kMaxUndoSteps)
        mRedoStack.pop_front();

    {
        const UndoApplyGuard guard(*this);
        ApplySceneState(mUndoStack.back());
    }
    mUndoStack.pop_back();

    mUndoBaseline = CaptureSceneState();
    // Deliberately in the past, so the next edit always records a step rather
    // than coalescing into whatever was being dragged before the undo.
    mLastSceneChangeTime = {};
    mSceneDirty = true;

    PTERO_LOG_INFO(kCategory, "Undo. %llu step%s left, %llu to redo.",
                   static_cast<unsigned long long>(mUndoStack.size()),
                   mUndoStack.size() == 1 ? "" : "s",
                   static_cast<unsigned long long>(mRedoStack.size()));
    return true;
}

bool Editor::Redo()
{
    if (mRedoStack.empty())
        return false;

    mUndoStack.push_back(CaptureSceneState());
    while (mUndoStack.size() > kMaxUndoSteps)
        mUndoStack.pop_front();

    {
        const UndoApplyGuard guard(*this);
        ApplySceneState(mRedoStack.back());
    }
    mRedoStack.pop_back();

    mUndoBaseline = CaptureSceneState();
    mLastSceneChangeTime = {};
    mSceneDirty = true;

    PTERO_LOG_INFO(kCategory, "Redo. %llu step%s left, %llu to undo.",
                   static_cast<unsigned long long>(mRedoStack.size()),
                   mRedoStack.size() == 1 ? "" : "s",
                   static_cast<unsigned long long>(mUndoStack.size()));
    return true;
}

void Editor::ResetUndoHistory()
{
    // Called when the scene is replaced wholesale - a load, a new level, a
    // reset. Undoing across that boundary would restore entities from a level
    // the user is no longer editing.
    mUndoStack.clear();
    mRedoStack.clear();
    mUndoBaseline = CaptureSceneState();
    mLastSceneChangeTime = {};
}
