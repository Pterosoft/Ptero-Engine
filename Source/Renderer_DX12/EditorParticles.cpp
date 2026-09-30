// ---------------------------------------------------------------------------
// Particle effects in the editor.
//
// A particle effect is authored once, in the Particle Editor, and saved as a
// .particle file; an emitter placed in the level only names that file. So the
// Particle Editor owns the effect's settings, and the Properties panel of an
// emitter only picks which effect it plays (plus Enabled, which is per emitter).
//
// Edits in the Particle Editor are pushed onto every emitter using the effect as
// they are made, so the viewport is the preview. They are not a scene change -
// the level stores only the path - which is why none of this marks the scene
// dirty except switching an emitter to another effect.
//
// Emitters from before effect files existed keep their settings inline in the
// level. Their Properties still edit those settings directly, and offer to save
// them out as an effect.
// ---------------------------------------------------------------------------

#include "pch.h"
#include "Editor.h"
#include "ParticleEffects.h"

#include "System/DataFiles.h"
#include "System/PteroLog.h"

#include <commdlg.h>

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace
{
    constexpr const char* kCategory = "Particles";

    // Data-relative paths as the level stores them: forward slashes, any case.
    bool SamePath(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            char x = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
            char y = static_cast<char>(std::tolower(static_cast<unsigned char>(b[i])));
            if (x == '\\') x = '/';
            if (y == '\\') y = '/';
            if (x != y)
                return false;
        }
        return true;
    }

    std::string DisplayName(const std::string& relativePath)
    {
        return std::filesystem::path(relativePath).stem().string();
    }

    // A name typed into the editor, made into a file name: no folders, nothing Windows
    // refuses, and no extension (one is added).
    std::string SanitizeEffectName(std::string name)
    {
        name.erase(std::remove_if(name.begin(), name.end(), [](char c) {
            return c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' ||
                   c == '<' || c == '>' || c == '|';
        }), name.end());
        const auto first = name.find_first_not_of(" .");
        const auto last = name.find_last_not_of(" .");
        name = first == std::string::npos ? std::string{} : name.substr(first, last - first + 1);
        const std::string extension = ParticleEffects::kExtension;
        if (name.size() > extension.size() &&
            _stricmp(name.c_str() + name.size() - extension.size(), extension.c_str()) == 0)
            name.resize(name.size() - extension.size());
        return name;
    }

    std::string EffectPathForName(const std::string& name)
    {
        return std::string(ParticleEffects::kFolder) + "/" + name + ParticleEffects::kExtension;
    }

    // Picks a .particle file inside Data and returns it Data-relative.
    bool BrowseForParticleEffect(std::string& inOutRelativePath)
    {
        const std::filesystem::path data = DataFiles::FindDataDirectory();
        if (data.empty())
            return false;

        const std::string initialDirectory = (data / ParticleEffects::kFolder).string();
        char fileBuffer[MAX_PATH] = {};
        OPENFILENAMEA dialog{};
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = QtUi::HostHandle();
        dialog.lpstrTitle = "Select Particle Effect";
        dialog.lpstrFilter = "Particle Effect\0*.particle\0All Files\0*.*\0";
        dialog.lpstrFile = fileBuffer;
        dialog.nMaxFile = MAX_PATH;
        dialog.lpstrInitialDir = initialDirectory.c_str();
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER | OFN_NOCHANGEDIR;
        if (QtUi::OpenFileName(&dialog) != TRUE)
            return false;

        std::error_code error;
        const std::filesystem::path relative = std::filesystem::relative(fileBuffer, data, error);
        if (error || relative.empty() || relative.native().starts_with(L".."))
        {
            PTERO_LOG_WARNING(kCategory, "%s is outside the Data folder, so a level cannot refer to it.", fileBuffer);
            return false;
        }
        inOutRelativePath = relative.generic_string();
        return true;
    }
}

const std::vector<std::string>& Editor::ParticleEffectFiles(bool rescan)
{
    if (rescan || !mParticleEffectFilesScanned)
    {
        mParticleEffectFiles = ParticleEffects::List();
        mParticleEffectFilesScanned = true;
    }
    return mParticleEffectFiles;
}

void Editor::PushParticleEffectToEntities(const std::string& path, const ParticleSystemComponent& effect)
{
    if (path.empty())
        return;
    for (Entity& entity : mEntities)
        if (entity.ParticleSystem.has_value() && SamePath(entity.ParticleSystem->ParticlePath, path))
            CopyParticleEffect(effect, *entity.ParticleSystem);
}

void Editor::ResyncParticleEffects()
{
    std::vector<std::string> problems;
    ParticleEffects::LoadAll(mEntities, &problems);
    for (const std::string& problem : problems)
        PTERO_LOG_WARNING(kCategory, "%s", problem.c_str());
    if (mParticleEditor.Dirty)
        PushParticleEffectToEntities(mParticleEditor.Path, mParticleEditor.Effect);
}

void Editor::OpenParticleEditor(const std::string& requestedPath)
{
    // A copy first: callers pass entries of the effect list, and the rescan below
    // replaces that list, which left this reading a freed string - clicking an effect in
    // the Particle Editor's list then silently did nothing.
    const std::string relativePath = requestedPath;
    mShowParticleEditor = true;
    ParticleEffectFiles(true);
    if (relativePath.empty() || SamePath(relativePath, mParticleEditor.Path))
        return;

    if (mParticleEditor.Dirty)
    {
        mParticleEditor.PendingAction = ParticleEditorState::Pending::Open;
        mParticleEditor.PendingPath = relativePath;
        return;
    }
    OpenParticleEditorFile(relativePath);
}

void Editor::OpenParticleEditorFile(const std::string& relativePath)
{
    ParticleSystemComponent effect;
    std::string error;
    if (!ParticleEffects::Load(relativePath, effect, &error))
    {
        mParticleEditor.Status = error;
        mParticleEditor.StatusIsError = true;
        return;
    }

    mParticleEditor.Path = relativePath;
    mParticleEditor.Effect = effect;
    mParticleEditor.Dirty = false;
    mParticleEditor.AssignEntityId = 0;
    strncpy_s(mParticleEditor.SaveAsName, DisplayName(relativePath).c_str(), _TRUNCATE);
    mParticleEditor.Status.clear();
    mParticleEditor.StatusIsError = false;
}

void Editor::NewParticleEditorEffect(const ParticleSystemComponent& from, std::uint64_t assignToEntityId,
                                     const std::string& suggestedName)
{
    mParticleEditor.Path.clear();
    mParticleEditor.Effect = from;
    mParticleEditor.Effect.Enabled = true;
    mParticleEditor.Effect.ParticlePath.clear();
    // Nothing to lose yet unless it carries an emitter's own settings over (the emitter is
    // pointed at the file on the first Save); the first edit makes it unsaved. Starting
    // every new effect dirty meant switching to another effect from an untouched one
    // asked about saving it.
    mParticleEditor.Dirty = assignToEntityId != 0;
    mParticleEditor.AssignEntityId = assignToEntityId;
    strncpy_s(mParticleEditor.SaveAsName, SanitizeEffectName(suggestedName).c_str(), _TRUNCATE);
    mParticleEditor.Status = "New effect. Give it a name and press Save.";
    mParticleEditor.StatusIsError = false;
    mShowParticleEditor = true;
}

bool Editor::SaveParticleEditorEffect(const std::string& relativePath)
{
    std::string error;
    if (!ParticleEffects::Save(relativePath, mParticleEditor.Effect, &error))
    {
        mParticleEditor.Status = error;
        mParticleEditor.StatusIsError = true;
        return false;
    }

    mParticleEditor.Path = relativePath;
    mParticleEditor.Dirty = false;
    strncpy_s(mParticleEditor.SaveAsName, DisplayName(relativePath).c_str(), _TRUNCATE);

    // A legacy emitter that asked for its inline settings to become this effect now
    // points at the file. That one is a real scene change: the level stores the path.
    if (mParticleEditor.AssignEntityId != 0)
    {
        for (Entity& entity : mEntities)
        {
            if (entity.Id != mParticleEditor.AssignEntityId || !entity.ParticleSystem.has_value())
                continue;
            entity.ParticleSystem->ParticlePath = relativePath;
            MarkSceneChanged();
            break;
        }
        mParticleEditor.AssignEntityId = 0;
    }

    PushParticleEffectToEntities(relativePath, mParticleEditor.Effect);
    ParticleEffectFiles(true);
    mParticleEditor.Status = "Saved " + relativePath + ".";
    mParticleEditor.StatusIsError = false;
    return true;
}

void Editor::RunPendingParticleEditorAction()
{
    const auto action = mParticleEditor.PendingAction;
    mParticleEditor.PendingAction = ParticleEditorState::Pending::None;
    switch (action)
    {
    case ParticleEditorState::Pending::Open:
        OpenParticleEditorFile(mParticleEditor.PendingPath);
        break;
    case ParticleEditorState::Pending::New:
        NewParticleEditorEffect(mParticleEditor.PendingFrom, mParticleEditor.PendingAssignEntityId,
                                mParticleEditor.PendingName);
        break;
    case ParticleEditorState::Pending::Close:
        mShowParticleEditor = false;
        mParticleEditorWasOpen = false;
        break;
    case ParticleEditorState::Pending::None:
        break;
    }
}

void Editor::DrawParticleEditorPrompt()
{
    if (mParticleEditor.PendingAction == ParticleEditorState::Pending::None)
        return;

    // Drawn inside the Particle Editor, above the effect, rather than in a window of its
    // own: a separate window could open behind the editor, and every click in the effect
    // list then seemed to do nothing while it waited for an answer nobody could see.
    const std::string name = mParticleEditor.Path.empty() ? std::string("The new effect")
                                                          : "\"" + DisplayName(mParticleEditor.Path) + "\"";
    std::string next;
    switch (mParticleEditor.PendingAction)
    {
    case ParticleEditorState::Pending::Open:  next = "before opening \"" + DisplayName(mParticleEditor.PendingPath) + "\""; break;
    case ParticleEditorState::Pending::New:   next = "before starting a new effect"; break;
    case ParticleEditorState::Pending::Close: next = "before closing the editor"; break;
    case ParticleEditorState::Pending::None:  break;
    }
    QtUi::SeparatorText("Unsaved changes");
    QtUi::TextColored(UiVec4(1.0f, 0.75f, 0.35f, 1.0f), "%s has unsaved changes. Save or discard them %s?",
                      name.c_str(), next.c_str());
    if (mParticleEditor.Path.empty())
        QtUi::TextDisabled("It is saved under the name typed below.");

    if (QtUi::Button("Save##psprompt"))
    {
        const std::string effectName = SanitizeEffectName(mParticleEditor.SaveAsName);
        const std::string target = !mParticleEditor.Path.empty() ? mParticleEditor.Path
                                   : effectName.empty()          ? std::string{}
                                                                 : EffectPathForName(effectName);
        if (target.empty())
        {
            mParticleEditor.Status = "Type a name for the effect before saving it.";
            mParticleEditor.StatusIsError = true;
            mParticleEditor.PendingAction = ParticleEditorState::Pending::None;
            mShowParticleEditor = true;
        }
        else if (SaveParticleEditorEffect(target))
        {
            RunPendingParticleEditorAction();
        }
    }
    QtUi::SameLine();
    if (QtUi::Button("Discard##psprompt"))
    {
        // Put every emitter back on what the file says before moving on.
        mParticleEditor.Dirty = false;
        mParticleEditor.AssignEntityId = 0;
        if (!mParticleEditor.Path.empty())
            ParticleEffects::Load(mParticleEditor.Path, mParticleEditor.Effect);
        ResyncParticleEffects();
        RunPendingParticleEditorAction();
    }
    QtUi::SameLine();
    if (QtUi::Button("Cancel##psprompt"))
    {
        mParticleEditor.PendingAction = ParticleEditorState::Pending::None;
        mShowParticleEditor = true;
    }
}

void Editor::DrawParticleEditorWindow()
{
    // The unsaved-changes question lives in the editor, so a pending one brings it back.
    if (!mShowParticleEditor && mParticleEditor.PendingAction != ParticleEditorState::Pending::None)
        mShowParticleEditor = true;
    if (!mShowParticleEditor)
    {
        mParticleEditorWasOpen = false;
        return;
    }

    ParticleEditorState& state = mParticleEditor;
    const auto requestNew = [this, &state](const ParticleSystemComponent& from, std::uint64_t assignId,
                                           const std::string& name) {
        if (state.Dirty)
        {
            state.PendingAction = ParticleEditorState::Pending::New;
            state.PendingFrom = from;
            state.PendingAssignEntityId = assignId;
            state.PendingName = name;
            return;
        }
        NewParticleEditorEffect(from, assignId, name);
    };

    QtUi::SetNextWindowSize(UiVec2(920.0f, 760.0f), QtUiCond_FirstUseEver);
    if (QtUi::Begin("Particle Editor", &mShowParticleEditor))
    {
        // --- Effect list -----------------------------------------------------
        if (QtUi::BeginChild("ParticleEffectList", UiVec2(240.0f, 0.0f), true))
        {
            QtUi::SetNextItemIcon("plus");
            if (QtUi::Button("New Effect##psedit"))
                requestNew(ParticleSystemComponent{}, 0, "NewEffect");
            QtUi::SameLine();
            if (QtUi::Button("Refresh##psedit"))
                ParticleEffectFiles(true);

            QtUi::Separator();
            // Opened after the loop: opening rescans the list being iterated.
            std::string clicked;
            const std::vector<std::string>& files = ParticleEffectFiles();
            if (files.empty())
                QtUi::TextDisabled("No .particle files in Data yet.");
            for (const std::string& file : files)
            {
                const bool current = SamePath(file, state.Path);
                const std::string label = DisplayName(file) + (current && state.Dirty ? " *" : "") + "##" + file;
                QtUi::SetNextItemIcon("particles");
                if (QtUi::Selectable(label.c_str(), current) && !current)
                    clicked = file;
                QtUi::SetItemTooltip("%s", file.c_str());
            }
            if (!clicked.empty())
                OpenParticleEditor(clicked);
        }
        QtUi::EndChild();

        QtUi::SameLine();

        // --- The effect ------------------------------------------------------
        if (QtUi::BeginChild("ParticleEffectDetails", UiVec2(0.0f, 0.0f), true))
        {
            DrawParticleEditorPrompt();

            const bool saved = !state.Path.empty();
            QtUi::SeparatorText(saved ? (DisplayName(state.Path) + (state.Dirty ? " *" : "")).c_str()
                                      : "New effect (not saved)");
            if (saved)
            {
                int users = 0;
                for (const Entity& entity : mEntities)
                    if (entity.ParticleSystem.has_value() && SamePath(entity.ParticleSystem->ParticlePath, state.Path))
                        ++users;
                QtUi::TextDisabled("%s  -  used by %d emitter%s in this level", state.Path.c_str(), users,
                                   users == 1 ? "" : "s");
            }

            QtUi::InputText("Name##psedit", state.SaveAsName, sizeof(state.SaveAsName));
            QtUi::SetItemTooltip("File name for Save As (and for the first Save of a new effect). "
                                 "Effects are saved in Data/Particles.");

            const std::string typedName = SanitizeEffectName(state.SaveAsName);
            const std::string saveAsTarget = typedName.empty() ? std::string{} : EffectPathForName(typedName);
            const auto saveAs = [&]() {
                if (saveAsTarget.empty())
                {
                    state.Status = "Type a name for the effect first.";
                    state.StatusIsError = true;
                    return;
                }
                // Never write over a different effect by accident; saving onto the effect
                // that is open is just a Save.
                if (!SamePath(saveAsTarget, state.Path) &&
                    DataFiles::Exists(ParticleEffects::AbsolutePath(saveAsTarget)))
                {
                    state.Status = saveAsTarget + " already exists. Pick another name, or open that effect.";
                    state.StatusIsError = true;
                    return;
                }
                SaveParticleEditorEffect(saveAsTarget);
            };

            QtUi::SetNextItemIcon("save");
            if (QtUi::Button("Save##psedit"))
            {
                if (saved)
                    SaveParticleEditorEffect(state.Path);
                else
                    saveAs();
            }
            QtUi::SameLine();
            QtUi::SetNextItemIcon("save-as");
            if (QtUi::Button("Save As##psedit"))
                saveAs();
            QtUi::SameLine();
            QtUi::BeginDisabled(!saved || !state.Dirty);
            QtUi::SetNextItemIcon("undo");
            if (QtUi::Button("Revert##psedit"))
            {
                OpenParticleEditorFile(state.Path);
                ResyncParticleEffects();
            }
            QtUi::EndDisabled();
            QtUi::SameLine();

            Entity* selected = GetSelectedEntity();
            const bool canAssign = saved && selected != nullptr && selected->ParticleSystem.has_value() &&
                                   !SamePath(selected->ParticleSystem->ParticlePath, state.Path);
            QtUi::BeginDisabled(!canAssign);
            if (QtUi::Button("Assign to Selected##psedit") && canAssign)
            {
                ParticleSystemComponent& target = *selected->ParticleSystem;
                target.ParticlePath = state.Path;
                CopyParticleEffect(state.Effect, target);
                MarkSceneChanged();
            }
            QtUi::EndDisabled();
            QtUi::SetItemTooltip("Make the selected Particle System emitter play this effect.");

            if (!state.Status.empty())
            {
                if (state.StatusIsError)
                    QtUi::TextColored(UiVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", state.Status.c_str());
                else
                    QtUi::TextDisabled("%s", state.Status.c_str());
            }
            if (state.Dirty && saved)
                QtUi::TextDisabled("Unsaved changes show on the level's emitters; Save writes them to the file.");

            if (DrawParticleEffectControls(state.Effect))
            {
                state.Dirty = true;
                PushParticleEffectToEntities(state.Path, state.Effect);
            }
        }
        QtUi::EndChild();
    }
    QtUi::End();

    // Closing the window must not quietly drop edits the level is already showing.
    if (!mShowParticleEditor && state.Dirty &&
        state.PendingAction == ParticleEditorState::Pending::None)
    {
        mShowParticleEditor = true;
        state.PendingAction = ParticleEditorState::Pending::Close;
    }
    mParticleEditorWasOpen = mShowParticleEditor;
}

void Editor::DrawParticleSystemProperties(Entity& entity)
{
    if (!QtUi::CollapsingHeader("Particle System", QtUiTreeNodeFlags_DefaultOpen))
        return;

    ParticleSystemComponent& ps = *entity.ParticleSystem;
    if (QtUi::Checkbox("Enabled##ps", &ps.Enabled))
        MarkSceneChanged();

    // Switching an emitter to another effect: take the file's settings, or the Particle
    // Editor's unsaved ones when that effect is open there.
    const auto useEffect = [&](const std::string& path) {
        std::string error;
        ParticleSystemComponent loaded = ps;
        if (!ParticleEffects::Load(path, loaded, &error))
        {
            PTERO_LOG_WARNING(kCategory, "%s", error.c_str());
            return;
        }
        ps = loaded;
        ps.ParticlePath = path;
        if (mParticleEditor.Dirty && SamePath(mParticleEditor.Path, path))
            CopyParticleEffect(mParticleEditor.Effect, ps);
        MarkSceneChanged();
    };

    QtUi::SeparatorText("Effect");
    if (!ps.ParticlePath.empty())
    {
        const std::vector<std::string>& files = ParticleEffectFiles();
        std::vector<std::string> labels;
        labels.reserve(files.size() + 1);
        int current = -1;
        for (std::size_t i = 0; i < files.size(); ++i)
        {
            labels.push_back(DisplayName(files[i]));
            if (SamePath(files[i], ps.ParticlePath))
                current = static_cast<int>(i);
        }
        const bool missing = current < 0;
        if (missing)
        {
            labels.push_back(DisplayName(ps.ParticlePath) + " (missing)");
            current = static_cast<int>(labels.size()) - 1;
        }
        std::vector<const char*> items;
        for (const std::string& label : labels)
            items.push_back(label.c_str());

        int chosen = current;
        if (QtUi::Combo("Effect##ps", &chosen, items.data(), static_cast<int>(items.size())) &&
            chosen != current && chosen < static_cast<int>(files.size()))
            useEffect(files[chosen]);
        QtUi::SetItemTooltip("%s", ps.ParticlePath.c_str());

        QtUi::SetNextItemIcon("particles");
        if (QtUi::Button("Edit Effect...##ps"))
            OpenParticleEditor(ps.ParticlePath);
        QtUi::SameLine();
        if (QtUi::Button("Browse...##pseffect"))
        {
            std::string path = ps.ParticlePath;
            if (BrowseForParticleEffect(path))
            {
                ParticleEffectFiles(true);
                useEffect(path);
            }
        }
        QtUi::SameLine();
        if (QtUi::Button("Refresh##pseffect"))
            ParticleEffectFiles(true);

        if (missing)
            QtUi::TextColored(UiVec4(1.0f, 0.45f, 0.35f, 1.0f),
                              "%s was not found; this emitter runs on default settings.", ps.ParticlePath.c_str());
        else if (mParticleEditor.Dirty && SamePath(mParticleEditor.Path, ps.ParticlePath))
            QtUi::TextColored(UiVec4(1.0f, 0.7f, 0.3f, 1.0f),
                              "Showing unsaved changes from the Particle Editor.");

        QtUi::TextDisabled("%s  |  %.0f per second  |  %.2f s lifetime",
                           ps.MaterialPath.empty() ? "no material" : ps.MaterialPath.c_str(),
                           ps.Burst ? static_cast<float>(ps.BurstCount) / (std::max)(ps.BurstInterval, 0.01f)
                                    : ps.SpawnRate,
                           ps.Lifetime);
        return;
    }

    // A legacy emitter: its settings live in the level.
    QtUi::TextWrapped("This emitter's settings are stored in the level, from before particle effects had "
                      "their own files. Save them as an effect to reuse them and edit them in the Particle Editor.");
    QtUi::SetNextItemIcon("save-as");
    if (QtUi::Button("Save as Particle Effect...##ps"))
    {
        const auto request = [&](const ParticleSystemComponent& from) {
            if (mParticleEditor.Dirty)
            {
                mParticleEditor.PendingAction = ParticleEditorState::Pending::New;
                mParticleEditor.PendingFrom = from;
                mParticleEditor.PendingAssignEntityId = entity.Id;
                mParticleEditor.PendingName = entity.Name;
                mShowParticleEditor = true;
                return;
            }
            NewParticleEditorEffect(from, entity.Id, entity.Name);
        };
        request(ps);
        ParticleEffectFiles(true);
    }
    QtUi::SameLine();
    if (QtUi::Button("Use Effect File...##ps"))
    {
        std::string path;
        if (BrowseForParticleEffect(path))
        {
            ParticleEffectFiles(true);
            useEffect(path);
        }
    }

    if (QtUi::TreeNode("Inline Settings##ps"))
    {
        if (DrawParticleEffectControls(ps))
            MarkSceneChanged();
        QtUi::TreePop();
    }
}
