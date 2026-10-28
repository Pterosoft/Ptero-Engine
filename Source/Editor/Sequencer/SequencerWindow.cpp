// Ptero Sequencer: the window.
//
// A dock along the bottom of the editor shell holding a toolbar, the track tree and the
// timeline. Everything it edits lives in a SequencerDocument (Qt-free, tested headless);
// this file is the view and the glue - toolbar, menus, dialogs - plus the SequencerEditor
// facade the level editor calls.
//
// Like the Node Graph window it has no Q_OBJECT: every connection is a lambda with a
// context object, so moc never runs on engine code.
//
// The preview writes animated values straight into the level's entities through a
// CinematicWorldHost - the same host a play session uses - so what the viewport shows
// while scrubbing is what the game will show. The document records each property's value
// before the first write and puts it back when the preview ends (the dock is closed, the
// level is saved, play starts), so a sequence never leaks into the authored level.

#include "SequencerEditor.h"
#include "SequencerTimeline.h"

#include "../../QtUi/QtUi.h"
#include "../../QtUi/QtUiTheme.h"
#include "../../Renderer_DX12/CinematicWorldHost.h"
#include "../../Renderer_DX12/Components.h"
#include "../../Renderer_DX12/EntityProperties.h"

#include "System/PteroLog.h"

#pragma warning(push)
#pragma warning(disable : 4996)
#include <QtWidgets/QtWidgets>
#pragma warning(pop)

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>

namespace
{
constexpr const char* kLogCategory = "Sequencer";

SequencerEditorHost* gHost = nullptr;
bool gCameraLock = false;

QIcon ThemeIcon(const char* name)
{
    return QtUiTheme::Icon(QString::fromUtf8(name));
}

// Picks one entity of the level by name, with a filter - levels have thousands, which no
// menu can list usefully.
std::uint64_t PickEntity(QWidget* parent, const QString& title)
{
    if (gHost == nullptr || gHost->Entities() == nullptr)
        return 0;
    gHost->EnsureEntityIds();

    QDialog dialog(parent);
    dialog.setWindowTitle(title);
    dialog.resize(360, 460);
    auto* layout = new QVBoxLayout(&dialog);
    auto* filter = new QLineEdit(&dialog);
    filter->setPlaceholderText(QStringLiteral("Filter by name"));
    filter->setClearButtonEnabled(true);
    auto* list = new QListWidget(&dialog);
    for (const Entity& entity : *gHost->Entities())
    {
        QString label = QString::fromStdString(entity.Name);
        if (entity.Camera)
            label += QStringLiteral("   [camera]");
        else if (entity.PointLight)
            label += QStringLiteral("   [light]");
        auto* item = new QListWidgetItem(label, list);
        item->setData(Qt::UserRole, QVariant::fromValue<qulonglong>(entity.Id));
    }
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(filter);
    layout->addWidget(list, 1);
    layout->addWidget(buttons);

    QObject::connect(filter, &QLineEdit::textChanged, list, [list](const QString& text) {
        for (int i = 0; i < list->count(); ++i)
            list->item(i)->setHidden(!list->item(i)->text().contains(text, Qt::CaseInsensitive));
    });
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog, [&dialog](QListWidgetItem*) { dialog.accept(); });
    if (list->count() > 0)
        list->setCurrentRow(0);
    filter->setFocus();

    if (dialog.exec() != QDialog::Accepted || list->currentItem() == nullptr || list->currentItem()->isHidden())
        return 0;
    return list->currentItem()->data(Qt::UserRole).value<qulonglong>();
}

const Entity* FindEntityById(std::uint64_t id)
{
    if (gHost == nullptr || gHost->Entities() == nullptr || id == 0)
        return nullptr;
    for (const Entity& entity : *gHost->Entities())
    {
        if (entity.Id == id)
            return &entity;
    }
    return nullptr;
}

class SequencerPanel final : public QMainWindow
{
public:
    explicit SequencerPanel(QWidget* parent)
        : QMainWindow(parent, Qt::Widget), mDocument(&mWorld)
    {
        setObjectName(QStringLiteral("pteroSequencerPanel"));
        mModel.Document = &mDocument;
        mModel.Host = gHost;

        BuildActions();
        BuildToolbar();
        BuildCentralWidget();
        BindModelActions();

        mDocument.OnPlayerCreated = [this](SequencePlayer& player) { AttachPlayer(player); };
        AttachPlayer(mDocument.Player());

        statusBar()->setSizeGripEnabled(false);
        mFileLabel = new QLabel(statusBar());
        statusBar()->addPermanentWidget(mFileLabel);
        SyncViewFromDocument();
        Refresh();
    }

    SequencerDocument& Document() { return mDocument; }
    bool IsRendering() const { return mRendering; }

    // ---- Facade support ------------------------------------------------------------

    void Tick(float deltaSeconds)
    {
        if (gHost == nullptr)
            return;
        SyncWorld();
        UpdateRenderProgress();
        if (gHost->IsPlaySessionActive())
            return;
        // While rendering, the render job sets the time, frame by frame.
        if (mRendering)
            return;

        const bool wasPlaying = mDocument.Player().IsPlaying();
        const std::uint64_t revision = mDocument.Revision();
        mDocument.Tick(deltaSeconds);
        if (wasPlaying || mDocument.Player().IsPlaying() || revision != mLastRevision)
            Refresh();
    }

    bool OpenFile(const QString& path)
    {
        if (!ConfirmDiscard())
            return false;
        std::string error;
        if (!mDocument.Load(path.toStdString(), error))
        {
            QMessageBox::warning(this, QStringLiteral("Open Sequence"),
                                 QStringLiteral("Could not open %1:\n%2").arg(path, QString::fromStdString(error)));
            return false;
        }
        if (!error.empty())
            ShowStatus(QStringLiteral("Opened with problems: ") + QString::fromStdString(error));
        else
            ShowStatus(QStringLiteral("Opened ") + QFileInfo(path).fileName());
        PTERO_LOG_INFO(kLogCategory, "Opened sequence '%s'.", path.toUtf8().constData());
        mModel.SelectedRows.clear();
        mModel.Invalidate();
        SyncViewFromDocument();
        Refresh();
        return true;
    }

    // Unsaved-edits prompt. True when it is fine to go ahead.
    bool ConfirmDiscard()
    {
        if (!mDocument.IsDirty())
            return true;
        const QString name = mDocument.Path().empty() ? QStringLiteral("The new sequence")
                                                      : QFileInfo(QString::fromStdString(mDocument.Path())).fileName();
        const auto answer = QMessageBox::question(
            this, QStringLiteral("Unsaved Sequence"),
            QStringLiteral("%1 has unsaved changes. Save them?").arg(name),
            QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
        if (answer == QMessageBox::Cancel)
            return false;
        if (answer == QMessageBox::Save)
            return Save(false);
        return true;
    }

    // The camera entity bound by `binding`, or 0.
    std::uint64_t CameraOfBinding(const Guid& bindingId)
    {
        const SequenceBinding* binding = mDocument.Sequence().FindBinding(bindingId);
        const Entity* entity = binding != nullptr && binding->Domain == "Entity"
            ? static_cast<const Entity*>(mWorld.ResolveBinding(*binding))
            : nullptr;
        return entity != nullptr && entity->Camera ? entity->Id : 0;
    }

    // The first camera the sequence binds, for a sequence with no cut at the playhead.
    Guid FirstCameraBinding()
    {
        for (const SequenceBinding& binding : mDocument.Sequence().Bindings)
        {
            if (CameraOfBinding(binding.Id) != 0)
                return binding.Id;
        }
        return Guid{};
    }

    bool HasCameraCuts() const
    {
        const CameraCutTrack* cuts = mDocument.Sequence().FindCameraCutTrack();
        return cuts != nullptr && !cuts->Muted && !cuts->Sections.empty();
    }

    std::uint64_t ActiveCutCamera(double& shakeTime)
    {
        if (!gCameraLock || mDocument.IsPreviewSuspended())
            return 0;
        const CinematicSequence& sequence = mDocument.Sequence();
        const double time = mDocument.Time();
        shakeTime = time;

        // A render looks through the camera its dialog chose.
        if (mRendering && mRenderCamera == RenderCamera::Viewport)
            return 0;
        if (mRendering && mRenderCamera == RenderCamera::Binding)
            return CameraOfBinding(mRenderCameraBinding);

        if (const CameraCutTrack* cuts = sequence.FindCameraCutTrack(); cuts != nullptr && !cuts->Muted)
        {
            const CameraCutSection* section = cuts->FindSectionAt(time);
            if (section == nullptr && !cuts->Sections.empty() && time >= cuts->Sections.back().End - 1e-9)
                section = &cuts->Sections.back();
            if (section != nullptr)
                return CameraOfBinding(section->CameraBinding);
        }

        // No cut covers the playhead - a sequence with one animated camera and no Camera
        // Cuts track is the common case. Look through the sequence's first camera rather
        // than leave Camera View showing the free viewport.
        return CameraOfBinding(FirstCameraBinding());
    }

    // ---- Render support ------------------------------------------------------------

    double PreviewAspect() const
    {
        const auto& metadata = mDocument.Sequence().Metadata;
        const auto width = metadata.find("Render.Width");
        const auto height = metadata.find("Render.Height");
        if (width != metadata.end() && height != metadata.end())
        {
            const double w = std::atof(width->second.c_str());
            const double h = std::atof(height->second.c_str());
            if (w > 0.0 && h > 0.0)
                return w / h;
        }
        return 16.0 / 9.0;
    }

    bool BeginRender()
    {
        if (mRendering || mDocument.IsPreviewSuspended() || (gHost != nullptr && gHost->IsPlaySessionActive()))
            return false;
        mRendering = true;
        mDocument.Player().Pause();
        // The render is what the cut cameras see; the lock is what makes the view follow them.
        mLockBeforeRender = gCameraLock;
        gCameraLock = true;
        Refresh();
        return true;
    }

    void SetRenderTime(double seconds)
    {
        if (mRendering)
            mDocument.SetTime(seconds);
    }

    void EndRender()
    {
        if (!mRendering)
            return;
        mRendering = false;
        gCameraLock = mLockBeforeRender;
        Refresh();
    }

    void SetCameraLockChecked(bool locked)
    {
        const QSignalBlocker blocker(mLockCameraAction);
        mLockCameraAction->setChecked(locked);
    }

    void OnSceneChanged()
    {
        // A camera component added, an entity renamed or deleted: rows may regroup.
        mModel.Invalidate();
        if (gHost != nullptr && !gHost->IsPlaySessionActive() && mDocument.OnWorldEdited())
            ShowStatus(QStringLiteral("Auto Key: keyed at %1").arg(TimeText(mDocument.Time())));
        Refresh();
    }

private:
    // ---- Construction --------------------------------------------------------------

    QAction* MakeAction(const QString& text, const char* icon, const QKeySequence& shortcut,
                        std::function<void()> trigger, bool checkable = false)
    {
        auto* action = new QAction(ThemeIcon(icon), text, this);
        action->setCheckable(checkable);
        if (!shortcut.isEmpty())
        {
            action->setShortcut(shortcut);
            // Only while the Sequencer has focus: Space, Delete and Ctrl+Z belong to the
            // viewport and the level everywhere else.
            action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            action->setToolTip(QStringLiteral("%1 (%2)").arg(text, shortcut.toString(QKeySequence::NativeText)));
        }
        else
        {
            action->setToolTip(text);
        }
        addAction(action);
        QObject::connect(action, &QAction::triggered, this, [this, trigger] {
            // Shortcuts too: nothing edits or plays the sequence while a render poses it.
            if (mRendering)
                return;
            trigger();
            Refresh();
        });
        return action;
    }

    void BuildActions()
    {
        mNewAction = MakeAction(QStringLiteral("New Sequence"), "new", QKeySequence(), [this] { NewSequence(); });
        mOpenAction = MakeAction(QStringLiteral("Open Sequence..."), "open", QKeySequence(), [this] { OpenSequence(); });
        mSaveAction = MakeAction(QStringLiteral("Save Sequence"), "save", QKeySequence(Qt::CTRL | Qt::Key_S), [this] { Save(false); });
        mSaveAsAction = MakeAction(QStringLiteral("Save Sequence As..."), "save-as", QKeySequence(), [this] { Save(true); });
        mUndoAction = MakeAction(QStringLiteral("Undo"), "undo", QKeySequence::Undo, [this] { mDocument.Undo(); });
        mRedoAction = MakeAction(QStringLiteral("Redo"), "redo", QKeySequence(Qt::CTRL | Qt::Key_Y), [this] { mDocument.Redo(); });

        mJumpStartAction = MakeAction(QStringLiteral("Jump to Start"), "jump-start", QKeySequence(Qt::Key_Home),
                                      [this] { Seek(mDocument.Sequence().PlaybackStart); });
        mPreviousFrameAction = MakeAction(QStringLiteral("Previous Frame"), "frame-prev", QKeySequence(Qt::Key_Left),
                                          [this] { StepFrames(-1); });
        mPlayFromStartAction = MakeAction(QStringLiteral("Play from Start"), "play-from-start", QKeySequence(Qt::SHIFT | Qt::Key_Space),
                                          [this] { mDocument.Player().PlayFromStart(); });
        mPlayAction = MakeAction(QStringLiteral("Play"), "play-start", QKeySequence(), [this] { mDocument.Player().Play(); });
        mPauseAction = MakeAction(QStringLiteral("Pause"), "pause", QKeySequence(), [this] { mDocument.Player().Pause(); });
        mStopAction = MakeAction(QStringLiteral("Stop"), "stop", QKeySequence(), [this] {
            // Back to the start, still previewing: the editor's Stop rewinds, and closing
            // the Sequencer is what hands the level back.
            mDocument.Player().Pause();
            mDocument.SetTime(mDocument.Sequence().PlaybackStart);
        });
        mNextFrameAction = MakeAction(QStringLiteral("Next Frame"), "frame-next", QKeySequence(Qt::Key_Right),
                                      [this] { StepFrames(1); });
        mJumpEndAction = MakeAction(QStringLiteral("Jump to End"), "jump-end", QKeySequence(Qt::Key_End),
                                    [this] { Seek(mDocument.Sequence().PlaybackEnd); });
        mLoopAction = MakeAction(QStringLiteral("Loop"), "loop", QKeySequence(), [this] {
            mDocument.Player().SetLooping(mLoopAction->isChecked());
        }, true);

        // Space toggles, the way every timeline does; the Play and Pause buttons stay
        // explicit.
        MakeAction(QStringLiteral("Play / Pause"), nullptr, QKeySequence(Qt::Key_Space), [this] {
            SequencePlayer& player = mDocument.Player();
            if (player.IsPlaying())
                player.Pause();
            else
                player.Play();
        });

        mAutoKeyAction = MakeAction(QStringLiteral("Auto Key"), "auto-key", QKeySequence(), [this] {
            mDocument.AutoKey = mAutoKeyAction->isChecked();
            ShowStatus(mDocument.AutoKey ? QStringLiteral("Auto Key on: edits to animated objects are keyed at the playhead.")
                                         : QStringLiteral("Auto Key off."));
        }, true);
        mAddKeyAction = MakeAction(QStringLiteral("Add Key"), "add-key", QKeySequence(Qt::Key_S), [this] { AddKeysToSelectedRows(); });
        mSnapAction = MakeAction(QStringLiteral("Snap to Frames"), "snap", QKeySequence(), [this] {
            mDocument.SnapEnabled = mSnapAction->isChecked();
        }, true);
        mSnapAction->setChecked(mDocument.SnapEnabled);
        mSettingsAction = MakeAction(QStringLiteral("Timeline Settings..."), "settings", QKeySequence(), [this] { ShowSettings(); });
        mLockCameraAction = MakeAction(QStringLiteral("Camera View"), "camera-lock", QKeySequence(Qt::Key_C), [this] {
            gCameraLock = mLockCameraAction->isChecked();
            ShowStatus(gCameraLock ? QStringLiteral("Camera View: the viewport looks through the camera cut at the playhead, "
                                                    "framed to the render's aspect ratio.")
                                   : QStringLiteral("Camera View off: the viewport is free again."));
        }, true);
        mLockCameraAction->setToolTip(QStringLiteral("Camera View (C): look through the sequence's cut camera, framed exactly "
                                                     "as Render Sequence will frame it"));
        mRenderAction = MakeAction(QStringLiteral("Render"), "render", QKeySequence(), [this] { ShowRenderDialog(); });
        mRenderAction->setToolTip(QStringLiteral("Render Sequence: write the sequence to images or an MP4"));

        MakeAction(QStringLiteral("Delete"), nullptr, QKeySequence::Delete, [this] { mDocument.DeleteSelectedKeys(); });
        MakeAction(QStringLiteral("Copy"), nullptr, QKeySequence::Copy, [this] { mDocument.CopySelectedKeys(); });
        MakeAction(QStringLiteral("Paste"), nullptr, QKeySequence::Paste, [this] { mDocument.Paste(mDocument.Time()); });
        MakeAction(QStringLiteral("Duplicate"), nullptr, QKeySequence(Qt::CTRL | Qt::Key_D), [this] { mDocument.DuplicateSelectedKeys(); });
        MakeAction(QStringLiteral("Frame All"), nullptr, QKeySequence(Qt::Key_A), [this] { mTimeline->FrameAll(); });
        MakeAction(QStringLiteral("Frame Selection"), nullptr, QKeySequence(Qt::Key_F), [this] { mTimeline->FrameSelection(); });
    }

    void BuildToolbar()
    {
        auto* toolbar = addToolBar(QStringLiteral("Sequencer"));
        toolbar->setObjectName(QStringLiteral("sequencerToolbar"));
        toolbar->setMovable(false);
        toolbar->setIconSize(QSize(16, 16));

        toolbar->addAction(mNewAction);
        toolbar->addAction(mOpenAction);
        toolbar->addAction(mSaveAction);
        toolbar->addAction(mSaveAsAction);
        toolbar->addSeparator();
        toolbar->addAction(mUndoAction);
        toolbar->addAction(mRedoAction);
        toolbar->addSeparator();
        toolbar->addAction(mJumpStartAction);
        toolbar->addAction(mPreviousFrameAction);
        toolbar->addAction(mPlayFromStartAction);
        toolbar->addAction(mPlayAction);
        toolbar->addAction(mPauseAction);
        toolbar->addAction(mStopAction);
        toolbar->addAction(mNextFrameAction);
        toolbar->addAction(mJumpEndAction);
        toolbar->addAction(mLoopAction);
        toolbar->addSeparator();

        mTimeField = new QLineEdit(toolbar);
        mTimeField->setFixedWidth(104);
        mTimeField->setAlignment(Qt::AlignCenter);
        mTimeField->setToolTip(QStringLiteral("Current time. Type a time and press Enter to jump there."));
        QObject::connect(mTimeField, &QLineEdit::editingFinished, this, [this] {
            double seconds = 0.0;
            if (ParseSequencerTime(mTimeField->text(), mDocument.Sequence().DisplayRate, mModel.TimeDisplay, seconds))
                Seek(seconds);
            Refresh();
        });
        toolbar->addWidget(mTimeField);

        mDisplayCombo = new QComboBox(toolbar);
        mDisplayCombo->addItems({ QStringLiteral("Timecode"), QStringLiteral("Seconds"), QStringLiteral("Frames") });
        mDisplayCombo->setToolTip(QStringLiteral("How times are shown"));
        QObject::connect(mDisplayCombo, &QComboBox::currentIndexChanged, this, [this](int index) {
            mModel.TimeDisplay = static_cast<SequencerTimeDisplay>(index);
            Refresh();
        });
        toolbar->addWidget(mDisplayCombo);

        mRateCombo = new QComboBox(toolbar);
        mRateCombo->setToolTip(QStringLiteral("Frame rate: what a frame step and snapping mean"));
        QObject::connect(mRateCombo, &QComboBox::activated, this, [this](int index) {
            const QVariant data = mRateCombo->itemData(index);
            const QPoint ratio = data.toPoint();
            if (ratio.x() > 0 && ratio.y() > 0)
                mDocument.SetFrameRate(FrameRate{ ratio.x(), ratio.y() });
            Refresh();
        });
        toolbar->addWidget(mRateCombo);
        toolbar->addSeparator();

        toolbar->addAction(mAutoKeyAction);
        toolbar->addAction(mAddKeyAction);
        toolbar->addAction(mSnapAction);
        toolbar->addSeparator();

        // Adding things to animate.
        auto* addButton = new QToolButton(toolbar);
        addButton->setIcon(ThemeIcon("plus"));
        addButton->setText(QStringLiteral("Add"));
        addButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        addButton->setPopupMode(QToolButton::InstantPopup);
        addButton->setToolTip(QStringLiteral("Add an entity, a camera or a master track"));
        auto* addMenu = new QMenu(addButton);
        QObject::connect(addMenu, &QMenu::aboutToShow, addMenu, [this, addMenu] { FillAddMenu(*addMenu); });
        addButton->setMenu(addMenu);
        toolbar->addWidget(addButton);
        toolbar->addSeparator();

        // The two things a user looks for by name get their names on the button.
        toolbar->addAction(mLockCameraAction);
        toolbar->addAction(mRenderAction);
        for (QAction* labelled : { mLockCameraAction, mRenderAction })
        {
            if (auto* button = qobject_cast<QToolButton*>(toolbar->widgetForAction(labelled)))
                button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        }
        toolbar->addAction(mSettingsAction);
        // The curve editor arrives in the next phase; until it works it is not offered.
        mToolbar = toolbar;
    }

    void BuildCentralWidget()
    {
        auto* central = new QWidget(this);
        auto* layout = new QHBoxLayout(central);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(0);

        auto* splitter = new QSplitter(Qt::Horizontal, central);
        splitter->setChildrenCollapsible(false);
        mTree = new SequencerTrackTree(mModel, splitter);
        mTimeline = new SequencerTimeline(mModel, splitter);
        splitter->addWidget(mTree);
        splitter->addWidget(mTimeline);
        splitter->setStretchFactor(0, 0);
        splitter->setStretchFactor(1, 1);
        splitter->setSizes({ 280, 900 });

        mScrollBar = new QScrollBar(Qt::Vertical, central);
        QObject::connect(mScrollBar, &QScrollBar::valueChanged, this, [this](int value) {
            if (mModel.ScrollY != value)
            {
                mModel.ScrollY = value;
                mTree->update();
                mTimeline->update();
            }
        });

        layout->addWidget(splitter, 1);
        layout->addWidget(mScrollBar);
        setCentralWidget(central);
    }

    void BindModelActions()
    {
        SequencerUiActions& actions = mModel.Actions;
        actions.Changed = [this] { Refresh(); };
        actions.Status = [this](const QString& message) { ShowStatus(message); };
        actions.EditKey = [this](const SequencerKeyHandle& key) { EditKey(key); };
        actions.AddEventAt = [this](double time) { AddEventAt(time); };
        actions.ShowAddTrackMenu = [this](const Guid& binding, const QPoint& position) { ShowAddTrackMenu(binding, position); };
        actions.FillAddCutMenu = [this](QMenu& menu, double time) { FillAddCutMenu(menu, time); };
        actions.SelectBindingEntity = [this](const Guid& binding) {
            if (const Entity* entity = BindingEntity(binding); entity != nullptr && gHost != nullptr)
                gHost->SelectEntity(entity->Id);
        };
        actions.RebindToSelection = [this](const Guid& binding) {
            const std::vector<std::uint64_t> selected = gHost != nullptr ? gHost->SelectedEntityIds() : std::vector<std::uint64_t>{};
            const Entity* entity = selected.empty() ? nullptr : FindEntityById(selected.front());
            if (entity == nullptr)
            {
                ShowStatus(QStringLiteral("Select an entity in the level first."));
                return;
            }
            mDocument.RebindEntity(binding, entity->Id, entity->Name);
        };
        actions.PilotBinding = [this](const Guid& binding) {
            if (const Entity* entity = BindingEntity(binding); entity != nullptr && gHost != nullptr)
            {
                gHost->PilotCamera(entity->Id);
                ShowStatus(QStringLiteral("Piloting %1: fly the viewport to move it. Viewport > Stop Piloting ends it.")
                               .arg(QString::fromStdString(entity->Name)));
            }
        };
        actions.IsCameraBinding = [this](const Guid& binding) {
            const Entity* entity = BindingEntity(binding);
            return entity != nullptr && entity->Camera.has_value();
        };
        actions.IsBindingResolved = [this](const SequenceBinding& binding) { return mWorld.ResolveBinding(binding) != nullptr; };
        actions.ScaleSelection = [this] { ScaleSelection(); };
    }

    void AttachPlayer(SequencePlayer& player)
    {
        player.SetLooping(mLoopAction != nullptr && mLoopAction->isChecked());
        player.OnSequenceEvent = [this](SequencePlayer& p, const SequenceEventKey& key) {
            QString text = QStringLiteral("Event '%1' at %2").arg(QString::fromStdString(key.Name), TimeText(p.GetTime()));
            if (!key.Payload.empty())
                text += QStringLiteral(" (%1)").arg(QString::fromStdString(key.Payload));
            ShowStatus(text);
        };
        player.OnSequenceFinished = [this](SequencePlayer&) { Refresh(); };
    }

    // ---- State -----------------------------------------------------------------------

    const Entity* BindingEntity(const Guid& bindingId)
    {
        const SequenceBinding* binding = mDocument.Sequence().FindBinding(bindingId);
        return binding != nullptr && binding->Domain == "Entity" ? static_cast<const Entity*>(mWorld.ResolveBinding(*binding))
                                                                 : nullptr;
    }

    void SyncWorld()
    {
        std::vector<Entity>* entities = gHost != nullptr ? gHost->Entities() : nullptr;
        if (entities != mWorldEntities)
        {
            mWorldEntities = entities;
            mWorld.SetEntities(entities);
        }
        if (gHost != nullptr)
            mWorld.SetEnvironment(gHost->Environment());
    }

    void SyncViewFromDocument()
    {
        const CinematicSequence& sequence = mDocument.Sequence();
        mModel.ViewStart = sequence.WorkStart;
        mModel.ViewEnd = sequence.WorkEnd > sequence.WorkStart ? sequence.WorkEnd : sequence.WorkStart + 1.0;
        mModel.ScrollY = 0;
        mModel.SelectionStart = mModel.SelectionEnd = 0.0;
    }

    QString TimeText(double seconds) const
    {
        return FormatSequencerTime(seconds, mDocument.Sequence().DisplayRate, mModel.TimeDisplay);
    }

    void ShowStatus(const QString& message)
    {
        statusBar()->showMessage(message, 6000);
    }

    void Refresh()
    {
        mLastRevision = mDocument.Revision();
        const CinematicSequence& sequence = mDocument.Sequence();
        SequencePlayer& player = mDocument.Player();
        const bool suspended = mDocument.IsPreviewSuspended() || (gHost != nullptr && gHost->IsPlaySessionActive());

        if (!mTimeField->hasFocus())
            mTimeField->setText(TimeText(mDocument.Time()));

        // The rate list: the standard ones, plus the sequence's own if it is unusual.
        const FrameRate rate = sequence.DisplayRate;
        if (mRateComboRate != rate || mRateCombo->count() == 0)
        {
            const QSignalBlocker blocker(mRateCombo);
            mRateCombo->clear();
            int count = 0;
            const FrameRate* rates = StandardFrameRates(count);
            int current = -1;
            for (int i = 0; i < count; ++i)
            {
                mRateCombo->addItem(QString::fromStdString(FrameRateLabel(rates[i])),
                                    QPoint(rates[i].Numerator, rates[i].Denominator));
                if (rates[i] == rate)
                    current = i;
            }
            if (current < 0)
            {
                mRateCombo->addItem(QString::fromStdString(FrameRateLabel(rate)), QPoint(rate.Numerator, rate.Denominator));
                current = mRateCombo->count() - 1;
            }
            mRateCombo->setCurrentIndex(current);
            mRateComboRate = rate;
        }

        mUndoAction->setEnabled(mDocument.CanUndo() && !suspended);
        mRedoAction->setEnabled(mDocument.CanRedo() && !suspended);
        mUndoAction->setToolTip(mDocument.CanUndo() ? QStringLiteral("Undo %1").arg(QString::fromStdString(mDocument.UndoLabel()))
                                                    : QStringLiteral("Undo"));
        mRedoAction->setToolTip(mDocument.CanRedo() ? QStringLiteral("Redo %1").arg(QString::fromStdString(mDocument.RedoLabel()))
                                                    : QStringLiteral("Redo"));
        mPlayAction->setEnabled(!player.IsPlaying() && !suspended);
        mPauseAction->setEnabled(player.IsPlaying());
        mPlayFromStartAction->setEnabled(!suspended);
        {
            const QSignalBlocker blocker(mLockCameraAction);
            mLockCameraAction->setChecked(gCameraLock);
        }
        // Nothing may change the sequence under a render; the progress window cancels it.
        centralWidget()->setEnabled(!suspended && !mRendering);
        if (mToolbar != nullptr)
            mToolbar->setEnabled(!mRendering);

        const QString name = mDocument.Path().empty() ? QStringLiteral("Untitled")
                                                      : QFileInfo(QString::fromStdString(mDocument.Path())).fileName();
        mFileLabel->setText(name + (mDocument.IsDirty() ? QStringLiteral(" *") : QString()) +
                            QStringLiteral("   %1 - %2").arg(TimeText(sequence.PlaybackStart), TimeText(sequence.PlaybackEnd)));
        if (QDockWidget* dock = qobject_cast<QDockWidget*>(parentWidget()))
            dock->setWindowTitle(QStringLiteral("Sequencer - ") + name + (mDocument.IsDirty() ? QStringLiteral("*") : QString()));

        // Scroll range follows the content.
        const int visible = (std::max)(0, mTimeline->height() - SequencerTimeline::kRulerHeight);
        const int maximum = (std::max)(0, mModel.ContentHeight() - visible);
        mModel.ScrollY = std::clamp(mModel.ScrollY, 0, maximum);
        {
            const QSignalBlocker blocker(mScrollBar);
            mScrollBar->setRange(0, maximum);
            mScrollBar->setPageStep((std::max)(1, visible));
            mScrollBar->setValue(mModel.ScrollY);
        }

        mTree->update();
        mTimeline->update();
    }

    // ---- Commands ----------------------------------------------------------------------

    void Seek(double seconds)
    {
        mDocument.Player().Pause();
        mDocument.SetTime(seconds);
    }

    void StepFrames(int frames)
    {
        const FrameRate& rate = mDocument.Sequence().DisplayRate;
        const double frame = double(rate.SecondsToNearestFrame(mDocument.Time()) + frames);
        Seek(rate.FrameToSeconds(frame));
    }

    std::string SequencesDirectory() const
    {
        if (gHost == nullptr)
            return {};
        std::filesystem::path directory = std::filesystem::path(gHost->DataDirectory()) / "Cinematics";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        return directory.string();
    }

    void NewSequence()
    {
        if (!ConfirmDiscard())
            return;
        mDocument.New();
        // A sequence starts with somewhere to put cuts and events: the two master tracks
        // nearly every cinematic uses. Part of the fresh state, not an undo step.
        mDocument.Sequence().GetOrAddCameraCutTrack();
        mDocument.Sequence().GetOrAddEventTrack();
        mModel.SelectedRows.clear();
        mModel.Invalidate();
        SyncViewFromDocument();
        ShowStatus(QStringLiteral("New sequence. Add entities with Add > Add Selected Entities."));
    }

    void OpenSequence()
    {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Open Sequence"),
                                                          QString::fromStdString(SequencesDirectory()),
                                                          QStringLiteral("Sequence (*.sequence);;All Files (*.*)"));
        if (!path.isEmpty())
            OpenFile(path);
    }

    bool Save(bool askForPath)
    {
        std::string path = mDocument.Path();
        if (askForPath || path.empty())
        {
            const QString suggested = QString::fromStdString(
                (std::filesystem::path(SequencesDirectory()) / (mDocument.Sequence().Name + ".sequence")).string());
            const QString chosen = QFileDialog::getSaveFileName(this, QStringLiteral("Save Sequence"), suggested,
                                                                QStringLiteral("Sequence (*.sequence)"));
            if (chosen.isEmpty())
                return false;
            path = chosen.toStdString();
            if (std::filesystem::path(path).extension() != ".sequence")
                path += ".sequence";
            // A sequence still carrying the default name takes the file's.
            if (mDocument.Sequence().Name == "Sequence")
                mDocument.Sequence().Name = std::filesystem::path(path).stem().string();
        }

        std::string error;
        if (!mDocument.Save(path, error))
        {
            QMessageBox::warning(this, QStringLiteral("Save Sequence"), QString::fromStdString(error));
            return false;
        }
        ShowStatus(QStringLiteral("Saved ") + QString::fromStdString(path));
        PTERO_LOG_INFO(kLogCategory, "Saved sequence '%s'.", path.c_str());
        return true;
    }

    void FillAddMenu(QMenu& menu)
    {
        menu.clear();
        menu.addAction(QStringLiteral("Add Selected Entities"), [this] { AddSelectedEntities(); });
        menu.addAction(QStringLiteral("Add Entity..."), [this] {
            if (const Entity* entity = FindEntityById(PickEntity(this, QStringLiteral("Add Entity"))))
                ExpandBinding(mDocument.AddEntityBinding(entity->Id, entity->Name));
            Refresh();
        });
        menu.addAction(ThemeIcon("camera"), QStringLiteral("Create Camera from View"), [this] { CreateCameraFromView(-1.0); });
        menu.addAction(ThemeIcon("time-of-day"), QStringLiteral("Add Environment"), [this] {
            ExpandBinding(mDocument.AddEnvironmentBinding());
            Refresh();
        });
        menu.addSeparator();
        QAction* cuts = menu.addAction(QStringLiteral("Add Camera Cut Track"), [this] {
            mDocument.Begin("Add Camera Cut Track", SequencerDocument::Scope_MasterTracks);
            mDocument.Sequence().GetOrAddCameraCutTrack();
            mDocument.MarkChanged(true);
            mDocument.Commit();
            Refresh();
        });
        cuts->setEnabled(mDocument.Sequence().FindCameraCutTrack() == nullptr);
        QAction* events = menu.addAction(QStringLiteral("Add Event Track"), [this] {
            mDocument.Begin("Add Event Track", SequencerDocument::Scope_MasterTracks);
            mDocument.Sequence().GetOrAddEventTrack();
            mDocument.MarkChanged(true);
            mDocument.Commit();
            Refresh();
        });
        events->setEnabled(mDocument.Sequence().FindEventTrack() == nullptr);
    }

    void ExpandBinding(const Guid& binding)
    {
        mModel.SetExpanded(QString::fromStdString(binding.ToString()), true);
    }

    void AddSelectedEntities()
    {
        if (gHost == nullptr)
            return;
        gHost->EnsureEntityIds();
        const std::vector<std::uint64_t> selected = gHost->SelectedEntityIds();
        if (selected.empty())
        {
            ShowStatus(QStringLiteral("Select entities in the level or the Level Explorer first."));
            return;
        }
        // One undo step for the lot.
        mDocument.Begin("Add Selected Entities", SequencerDocument::Scope_None);
        int added = 0;
        for (std::uint64_t id : selected)
        {
            if (const Entity* entity = FindEntityById(id))
            {
                if (mDocument.Sequence().FindBindingForEntity(id) == nullptr)
                    ++added;
                ExpandBinding(mDocument.AddEntityBinding(entity->Id, entity->Name));
            }
        }
        mDocument.Commit();
        ShowStatus(QStringLiteral("Added %1 entit%2.").arg(added).arg(added == 1 ? "y" : "ies"));
    }

    // Creates a camera where the viewport looks from and binds it; with a time, also cuts
    // to it there.
    void CreateCameraFromView(double cutTime)
    {
        if (gHost == nullptr)
            return;
        int cameraCount = 0;
        if (gHost->Entities() != nullptr)
        {
            for (const Entity& entity : *gHost->Entities())
                cameraCount += entity.Camera ? 1 : 0;
        }
        const std::string name = "Camera_" + std::to_string(cameraCount + 1);
        const std::uint64_t id = gHost->CreateCameraAtView(name);
        const Entity* entity = FindEntityById(id);
        if (entity == nullptr)
        {
            ShowStatus(QStringLiteral("Could not create a camera."));
            return;
        }

        mDocument.Begin("Create Camera from View", SequencerDocument::Scope_MasterTracks);
        const Guid binding = mDocument.AddEntityBinding(entity->Id, entity->Name);
        ExpandBinding(binding);
        if (cutTime >= 0.0)
            mDocument.AddCameraCut(cutTime, binding);
        mDocument.Commit();
        mModel.Invalidate();
        ShowStatus(QStringLiteral("Created %1 at the viewport's view.").arg(QString::fromStdString(name)));
    }

    void FillAddCutMenu(QMenu& menu, double time)
    {
        bool any = false;
        for (const SequenceBinding& binding : mDocument.Sequence().Bindings)
        {
            const Entity* entity = binding.Domain == "Entity" ? static_cast<const Entity*>(mWorld.ResolveBinding(binding)) : nullptr;
            if (entity == nullptr || !entity->Camera)
                continue;
            const Guid id = binding.Id;
            menu.addAction(ThemeIcon("camera"), QString::fromStdString(binding.Name), [this, time, id] {
                mDocument.AddCameraCut(time, id);
                Refresh();
            });
            any = true;
        }
        if (!any)
        {
            QAction* none = menu.addAction(QStringLiteral("No camera bound yet"));
            none->setEnabled(false);
        }
        menu.addSeparator();
        menu.addAction(ThemeIcon("camera"), QStringLiteral("New Camera from View"), [this, time] {
            CreateCameraFromView(time);
            Refresh();
        });
    }

    void ShowAddTrackMenu(const Guid& bindingId, const QPoint& position)
    {
        const SequenceBinding* binding = mDocument.Sequence().FindBinding(bindingId);
        if (binding == nullptr)
            return;
        RegisterEngineProperties();
        void* owner = mWorld.ResolveBinding(*binding);

        QMenu menu(this);
        std::map<QString, QMenu*> groups;
        QMenu* components = nullptr;
        for (const PropertyDescriptor* descriptor : PropertyRegistry::List(binding->Domain))
        {
            if ((descriptor->Flags & PropertyFlag_Animatable) == 0)
                continue;
            if (owner != nullptr && !descriptor->IsAvailableOn(owner))
                continue;

            const QString group = QString::fromStdString(descriptor->Group);
            QMenu* target = &menu;
            // Transform and the camera's lens sit at the top; everything else is grouped
            // by the component that owns it.
            if (group != QStringLiteral("Transform"))
            {
                QMenu*& submenu = groups[group];
                if (submenu == nullptr)
                {
                    QMenu* parentMenu = &menu;
                    if (group != QStringLiteral("Camera") && binding->Domain == "Entity")
                    {
                        if (components == nullptr)
                            components = menu.addMenu(QStringLiteral("Components"));
                        parentMenu = components;
                    }
                    submenu = parentMenu->addMenu(group);
                }
                target = submenu;
            }

            const PropertyDescriptor* chosen = descriptor;
            QAction* action = target->addAction(QString::fromStdString(descriptor->DisplayName), [this, bindingId, chosen] {
                const PropertyTrack* track = mDocument.AddPropertyTrack(bindingId, *chosen, true, mDocument.Time());
                ExpandBinding(bindingId);
                if (track != nullptr)
                    ShowStatus(QStringLiteral("Added %1, keyed at the playhead.").arg(QString::fromStdString(chosen->DisplayName)));
                Refresh();
            });
            action->setToolTip(QString::fromStdString(descriptor->Tooltip));
            if (binding->FindPropertyTrack(descriptor->Path) != nullptr)
            {
                action->setEnabled(false);
                action->setText(action->text() + QStringLiteral("  (animated)"));
            }
        }
        if (menu.isEmpty())
            menu.addAction(owner == nullptr ? QStringLiteral("The bound entity is not in this level")
                                            : QStringLiteral("Nothing on this entity can be animated"))
                ->setEnabled(false);
        menu.exec(position);
    }

    void AddKeysToSelectedRows()
    {
        // Keys every selected row at the playhead: a binding row keys all of its tracks.
        mDocument.Begin("Add Key", SequencerDocument::Scope_AllBindings);
        int keyed = 0;
        for (const QString& key : mModel.SelectedRows)
        {
            const SequencerRow* row = mModel.FindRow(key);
            if (row == nullptr)
                continue;
            for (const auto& [track, mask] : mModel.TracksOfRow(*row))
            {
                mDocument.AddKey(track->Id, mDocument.Time(), mask);
                ++keyed;
            }
        }
        mDocument.Commit();
        ShowStatus(keyed > 0 ? QStringLiteral("Keyed %1 track%2 at %3.").arg(keyed).arg(keyed == 1 ? "" : "s").arg(TimeText(mDocument.Time()))
                             : QStringLiteral("Select the tracks (or bindings) to key in the track list."));
    }

    void AddEventAt(double time)
    {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Add Event"),
                                                   QStringLiteral("Event name (what graphs listen for):"),
                                                   QLineEdit::Normal, QStringLiteral("Event"), &ok);
        if (!ok || name.trimmed().isEmpty())
            return;
        mDocument.AddEvent(time, name.trimmed().toStdString());
        mModel.Invalidate();
        Refresh();
    }

    void ScaleSelection()
    {
        double first = 1e30;
        for (const SequencerKeyHandle& handle : mDocument.SelectedKeys())
        {
            double time = 0.0;
            if (mDocument.KeyTime(handle, time))
                first = (std::min)(first, time);
        }
        if (first > 1e29)
            return;
        bool ok = false;
        const double factor = QInputDialog::getDouble(this, QStringLiteral("Scale Selection"),
                                                      QStringLiteral("Scale the selected keys' timing by\n(about the first selected key):"),
                                                      1.0, 0.01, 100.0, 3, &ok);
        if (ok)
            mDocument.ScaleSelectedKeys(factor, first);
        Refresh();
    }

    void ShowSettings()
    {
        CinematicSequence& sequence = mDocument.Sequence();
        const FrameRate rate = sequence.DisplayRate;
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("Timeline Settings"));
        auto* form = new QFormLayout(&dialog);

        auto timeField = [&](double seconds) {
            auto* field = new QLineEdit(TimeText(seconds), &dialog);
            field->setToolTip(QStringLiteral("Timecode (HH:MM:SS:FF), seconds (\"2.5 s\") or frames"));
            return field;
        };
        auto* name = new QLineEdit(QString::fromStdString(sequence.Name), &dialog);
        auto* playStart = timeField(sequence.PlaybackStart);
        auto* playEnd = timeField(sequence.PlaybackEnd);
        auto* viewStart = timeField(mModel.ViewStart);
        auto* viewEnd = timeField(mModel.ViewEnd);
        auto* interpolation = new QComboBox(&dialog);
        const KeyInterpolation modes[] = { KeyInterpolation::Auto, KeyInterpolation::Cubic, KeyInterpolation::Linear,
                                           KeyInterpolation::Constant, KeyInterpolation::EaseInOut };
        for (KeyInterpolation mode : modes)
        {
            interpolation->addItem(QString::fromUtf8(KeyInterpolationName(mode)), static_cast<int>(mode));
            if (mode == mDocument.DefaultInterpolation)
                interpolation->setCurrentIndex(interpolation->count() - 1);
        }

        form->addRow(QStringLiteral("Name"), name);
        form->addRow(QStringLiteral("Playback start"), playStart);
        form->addRow(QStringLiteral("Playback end"), playEnd);
        form->addRow(QStringLiteral("View start"), viewStart);
        form->addRow(QStringLiteral("View end"), viewEnd);
        form->addRow(QStringLiteral("New keys"), interpolation);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        form->addRow(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted)
            return;

        double a = 0.0, b = 0.0, c = 0.0, d = 0.0;
        const bool rangeOk = ParseSequencerTime(playStart->text(), rate, mModel.TimeDisplay, a) &&
                             ParseSequencerTime(playEnd->text(), rate, mModel.TimeDisplay, b) && b > a;
        const bool viewOk = ParseSequencerTime(viewStart->text(), rate, mModel.TimeDisplay, c) &&
                            ParseSequencerTime(viewEnd->text(), rate, mModel.TimeDisplay, d) && d > c;

        mDocument.Begin("Timeline Settings", SequencerDocument::Scope_Header);
        if (!name->text().trimmed().isEmpty())
            sequence.Name = name->text().trimmed().toStdString();
        if (rangeOk)
        {
            sequence.PlaybackStart = a;
            sequence.PlaybackEnd = b;
        }
        mDocument.MarkChanged(false);
        mDocument.Commit();
        if (viewOk)
        {
            mModel.ViewStart = c;
            mModel.ViewEnd = d;
            mDocument.SetWorkRange(c, d);
        }
        mDocument.DefaultInterpolation = static_cast<KeyInterpolation>(interpolation->currentData().toInt());
        mDocument.SetTime(mDocument.Time());
        if (!rangeOk || !viewOk)
            ShowStatus(QStringLiteral("A range was not understood (or its end was not after its start) and was left as it was."));
    }

    // ---- Render Sequence ------------------------------------------------------------

    void ShowRenderDialog()
    {
        if (gHost == nullptr)
            return;
        CinematicSequence& sequence = mDocument.Sequence();
        auto setting = [&](const char* key, const std::string& fallback) {
            const auto it = sequence.Metadata.find(key);
            return QString::fromStdString(it != sequence.Metadata.end() ? it->second : fallback);
        };

        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("Render Sequence"));
        auto* form = new QFormLayout(&dialog);

        struct Preset
        {
            const char* Name;
            int Width;
            int Height;
        };
        static const Preset kPresets[] = {
            { "1280 x 720 (HD)", 1280, 720 },          { "1920 x 1080 (Full HD)", 1920, 1080 },
            { "2560 x 1440 (QHD)", 2560, 1440 },       { "3840 x 2160 (4K UHD)", 3840, 2160 },
            { "2048 x 858 (2K Scope 2.39:1)", 2048, 858 }, { "1080 x 1920 (Vertical)", 1080, 1920 },
        };
        auto* preset = new QComboBox(&dialog);
        for (const Preset& entry : kPresets)
            preset->addItem(QString::fromUtf8(entry.Name), QPoint(entry.Width, entry.Height));
        preset->addItem(QStringLiteral("Custom"), QPoint(0, 0));
        auto* width = new QSpinBox(&dialog);
        auto* height = new QSpinBox(&dialog);
        width->setRange(16, 8192);
        height->setRange(16, 8192);
        width->setValue(setting("Render.Width", "1920").toInt());
        height->setValue(setting("Render.Height", "1080").toInt());
        auto matchPreset = [&] {
            const QSignalBlocker blocker(preset);
            int match = preset->count() - 1;
            for (int i = 0; i < preset->count() - 1; ++i)
            {
                if (preset->itemData(i).toPoint() == QPoint(width->value(), height->value()))
                    match = i;
            }
            preset->setCurrentIndex(match);
        };
        matchPreset();
        QObject::connect(preset, &QComboBox::currentIndexChanged, &dialog, [&](int index) {
            const QPoint size = preset->itemData(index).toPoint();
            if (size.x() > 0)
            {
                const QSignalBlocker a(width);
                const QSignalBlocker b(height);
                width->setValue(size.x());
                height->setValue(size.y());
            }
        });
        QObject::connect(width, &QSpinBox::valueChanged, &dialog, [&](int) { matchPreset(); });
        QObject::connect(height, &QSpinBox::valueChanged, &dialog, [&](int) { matchPreset(); });

        // Frame rate, and the range in frames at that rate (the playback range by default).
        auto* rate = new QComboBox(&dialog);
        int rateCount = 0;
        const FrameRate* rates = StandardFrameRates(rateCount);
        for (int i = 0; i < rateCount; ++i)
        {
            rate->addItem(QString::fromStdString(FrameRateLabel(rates[i])), QPoint(rates[i].Numerator, rates[i].Denominator));
            if (rates[i] == sequence.DisplayRate)
                rate->setCurrentIndex(i);
        }
        auto rateOf = [&] {
            const QPoint ratio = rate->currentData().toPoint();
            return FrameRate{ ratio.x(), ratio.y() };
        };
        auto* startFrame = new QSpinBox(&dialog);
        auto* endFrame = new QSpinBox(&dialog);
        auto setRangeFor = [&](const FrameRate& r) {
            const int first = static_cast<int>(r.SecondsToNearestFrame(sequence.PlaybackStart));
            const int last = static_cast<int>(r.SecondsToNearestFrame(sequence.PlaybackEnd));
            for (QSpinBox* box : { startFrame, endFrame })
                box->setRange(first, last);
            startFrame->setValue(first);
            endFrame->setValue(last);
        };
        setRangeFor(rateOf());
        QObject::connect(rate, &QComboBox::currentIndexChanged, &dialog, [&](int) { setRangeFor(rateOf()); });

        auto* output = new QLineEdit(&dialog);
        const std::filesystem::path defaultOutput =
            std::filesystem::path(gHost->DataDirectory()).parent_path() / "Renders" / sequence.Name;
        output->setText(setting("Render.Output", defaultOutput.string()));
        auto* browse = new QPushButton(QStringLiteral("Browse..."), &dialog);
        QObject::connect(browse, &QPushButton::clicked, &dialog, [&] {
            const QString chosen = QFileDialog::getExistingDirectory(&dialog, QStringLiteral("Render Output Folder"), output->text());
            if (!chosen.isEmpty())
                output->setText(QDir::toNativeSeparators(chosen));
        });
        auto* outputRow = new QHBoxLayout;
        outputRow->addWidget(output, 1);
        outputRow->addWidget(browse);

        auto* format = new QComboBox(&dialog);
        format->addItem(QStringLiteral("PNG image sequence"), static_cast<int>(CinematicRenderSettings::Format::Png));
        format->addItem(QStringLiteral("JPEG image sequence"), static_cast<int>(CinematicRenderSettings::Format::Jpeg));
        format->addItem(QStringLiteral("MP4 video (H.264)"), static_cast<int>(CinematicRenderSettings::Format::Mp4));
        format->setCurrentIndex(std::clamp(setting("Render.Format", "0").toInt(), 0, 2));
        auto* quality = new QSpinBox(&dialog);
        quality->setRange(1, 100);
        quality->setValue(setting("Render.Quality", "90").toInt());
        quality->setToolTip(QStringLiteral("JPEG quality, or MP4 visual quality. PNG is lossless and ignores it."));
        auto* warmup = new QSpinBox(&dialog);
        warmup->setRange(0, 1000);
        warmup->setValue(setting("Render.Warmup", "24").toInt());
        warmup->setToolTip(QStringLiteral("Frames rendered and discarded before the first one is kept, so TAA, exposure "
                                          "and GI have settled."));
        QObject::connect(format, &QComboBox::currentIndexChanged, &dialog,
                         [&](int index) { quality->setEnabled(index != 0); });
        quality->setEnabled(format->currentIndex() != 0);

        form->addRow(QStringLiteral("Resolution"), preset);
        auto* sizeRow = new QHBoxLayout;
        sizeRow->addWidget(width);
        sizeRow->addWidget(new QLabel(QStringLiteral("x"), &dialog));
        sizeRow->addWidget(height);
        form->addRow(QString(), sizeRow);
        form->addRow(QStringLiteral("Frame rate"), rate);
        form->addRow(QStringLiteral("Start frame"), startFrame);
        form->addRow(QStringLiteral("End frame"), endFrame);
        form->addRow(QStringLiteral("Output folder"), outputRow);
        form->addRow(QStringLiteral("Format"), format);
        form->addRow(QStringLiteral("Quality"), quality);
        form->addRow(QStringLiteral("Warm-up frames"), warmup);

        // What the render looks through. The cuts when there are any; otherwise the
        // sequence's camera - rendering the free viewport is a choice, never a default.
        auto* camera = new QComboBox(&dialog);
        if (HasCameraCuts())
            camera->addItem(QStringLiteral("Camera Cuts"), QStringLiteral("cuts"));
        for (const SequenceBinding& binding : sequence.Bindings)
        {
            if (CameraOfBinding(binding.Id) != 0)
                camera->addItem(QString::fromStdString(binding.Name), QString::fromStdString(binding.Id.ToString()));
        }
        camera->addItem(QStringLiteral("Viewport (free camera)"), QStringLiteral("viewport"));
        camera->setToolTip(QStringLiteral("The camera the render looks through. Turn on Camera View to see the framing first."));
        form->insertRow(0, QStringLiteral("Camera"), camera);

        QString note = QStringLiteral("Turn on Camera View before rendering to see exactly what will be rendered.");
        if (!HasCameraCuts())
            note += QStringLiteral(" This sequence has no camera cuts - add a Camera Cuts track to switch cameras.");
        note += QStringLiteral("\nEXR is not offered: the engine's final image is display-referred, so EXR would "
                               "carry no extra range. MP4 has no audio yet - sequences have no audio tracks.");
        auto* noteLabel = new QLabel(note, &dialog);
        noteLabel->setWordWrap(true);
        QPalette dim = noteLabel->palette();
        dim.setColor(QPalette::WindowText, SequencerColors::TextDim());
        noteLabel->setPalette(dim);
        form->addRow(noteLabel);

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, &dialog);
        buttons->addButton(QStringLiteral("Render"), QDialogButtonBox::AcceptRole);
        form->addRow(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted)
            return;

        CinematicRenderSettings settings;
        settings.OutputDirectory = output->text().trimmed().toStdString();
        settings.BaseName = sequence.Name.empty() ? std::string("Sequence") : sequence.Name;
        settings.Width = width->value();
        settings.Height = height->value();
        settings.RateNumerator = rateOf().Numerator;
        settings.RateDenominator = rateOf().Denominator;
        settings.StartFrame = startFrame->value();
        settings.EndFrame = (std::max)(startFrame->value(), endFrame->value());
        settings.OutputFormat = static_cast<CinematicRenderSettings::Format>(format->currentData().toInt());
        settings.Quality = quality->value();
        settings.WarmupFrames = warmup->value();

        const QString cameraChoice = camera->currentData().toString();
        if (cameraChoice == QStringLiteral("cuts"))
            mRenderCamera = RenderCamera::Cuts;
        else if (cameraChoice == QStringLiteral("viewport"))
            mRenderCamera = RenderCamera::Viewport;
        else
        {
            mRenderCamera = RenderCamera::Binding;
            mRenderCameraBinding = Guid::Parse(cameraChoice.toStdString());
        }

        // Remembered with the sequence; the resolution also sets the Camera View framing.
        mDocument.Begin("Render Settings", SequencerDocument::Scope_Header);
        sequence.Metadata["Render.Width"] = std::to_string(settings.Width);
        sequence.Metadata["Render.Height"] = std::to_string(settings.Height);
        sequence.Metadata["Render.Format"] = std::to_string(format->currentIndex());
        sequence.Metadata["Render.Quality"] = std::to_string(settings.Quality);
        sequence.Metadata["Render.Warmup"] = std::to_string(settings.WarmupFrames);
        sequence.Metadata["Render.Output"] = settings.OutputDirectory;
        mDocument.MarkChanged(false);
        mDocument.Commit();

        std::string error;
        if (!gHost->StartCinematicRender(settings, error))
        {
            QMessageBox::warning(this, QStringLiteral("Render Sequence"), QString::fromStdString(error));
            return;
        }
        ShowStatus(QStringLiteral("Rendering..."));
    }

    void UpdateRenderProgress()
    {
        const CinematicRenderStatus status = gHost->GetCinematicRenderStatus();
        if (status.Active)
        {
            if (mRenderProgress == nullptr)
            {
                // Not modal: the editor's frame loop has to keep running to render.
                mRenderProgress = new QProgressDialog(this);
                mRenderProgress->setWindowTitle(QStringLiteral("Render Sequence"));
                mRenderProgress->setCancelButtonText(QStringLiteral("Cancel"));
                mRenderProgress->setAutoClose(false);
                mRenderProgress->setAutoReset(false);
                mRenderProgress->setMinimumDuration(0);
                mRenderProgress->setWindowModality(Qt::NonModal);
                QObject::connect(mRenderProgress, &QProgressDialog::canceled, this, [] {
                    if (gHost != nullptr)
                        gHost->CancelCinematicRender();
                });
                mRenderProgress->show();
            }
            mRenderProgress->setMaximum(status.Encoding ? 0 : (std::max)(1, status.FramesTotal));
            if (!status.Encoding)
                mRenderProgress->setValue(status.FramesDone);
            mRenderProgress->setLabelText(QString::fromStdString(status.Message));
            mRenderWasActive = true;
            return;
        }

        if (!mRenderWasActive)
            return;
        mRenderWasActive = false;
        if (mRenderProgress != nullptr)
        {
            mRenderProgress->hide();
            mRenderProgress->deleteLater();
            mRenderProgress = nullptr;
        }

        ShowStatus(QString::fromStdString(status.Message));
        auto* box = new QMessageBox(status.Failed ? QMessageBox::Warning : QMessageBox::Information,
                                    QStringLiteral("Render Sequence"), QString::fromStdString(status.Message),
                                    QMessageBox::Close, this);
        box->setAttribute(Qt::WA_DeleteOnClose);
        box->setWindowModality(Qt::NonModal);
        if (!status.Failed && !status.OutputPath.empty())
        {
            const QString path = QString::fromStdString(status.OutputPath);
            QPushButton* open = box->addButton(QStringLiteral("Show in Explorer"), QMessageBox::ActionRole);
            QObject::connect(open, &QPushButton::clicked, box, [path] {
                const QFileInfo info(path);
                QProcess::startDetached(QStringLiteral("explorer.exe"),
                                        info.isDir() ? QStringList{ QDir::toNativeSeparators(path) }
                                                     : QStringList{ QStringLiteral("/select,") + QDir::toNativeSeparators(path) });
            });
        }
        box->show();
    }

    void EditKey(const SequencerKeyHandle& handle)
    {
        SequenceBinding* binding = nullptr;
        SequenceTrack* track = mDocument.FindTrack(handle.Track, &binding);
        if (track == nullptr)
            return;
        const FrameRate rate = mDocument.Sequence().DisplayRate;

        if (handle.Channel == SequencerKeyHandle::kEventChannel)
        {
            auto* events = dynamic_cast<EventTrack*>(track);
            const SequenceEventKey* key = nullptr;
            for (const SequenceEventKey& candidate : events->Keys())
            {
                if (candidate.Id == handle.Key)
                    key = &candidate;
            }
            if (key == nullptr)
                return;

            QDialog dialog(this);
            dialog.setWindowTitle(QStringLiteral("Edit Event"));
            auto* form = new QFormLayout(&dialog);
            auto* name = new QLineEdit(QString::fromStdString(key->Name), &dialog);
            auto* payload = new QLineEdit(QString::fromStdString(key->Payload), &dialog);
            payload->setPlaceholderText(QStringLiteral("Optional text handed to listeners"));
            auto* time = new QLineEdit(TimeText(key->Time), &dialog);
            form->addRow(QStringLiteral("Name"), name);
            form->addRow(QStringLiteral("Payload"), payload);
            form->addRow(QStringLiteral("Time"), time);
            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
            form->addRow(buttons);
            QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
            if (dialog.exec() != QDialog::Accepted)
                return;
            double seconds = key->Time;
            ParseSequencerTime(time->text(), rate, mModel.TimeDisplay, seconds);
            mDocument.EditEvent(handle.Key, seconds, name->text().trimmed().toStdString(), payload->text().toStdString());
            Refresh();
            return;
        }

        auto* property = dynamic_cast<PropertyTrack*>(track);
        if (property == nullptr || binding == nullptr || handle.Channel >= property->ChannelCount())
            return;
        FloatChannel& channel = property->Channel(handle.Channel);
        const Keyframe* key = channel.FindKeyById(handle.Key);
        if (key == nullptr)
            return;
        const Keyframe original = *key;
        const PropertyType type = property->ValueType();
        const bool isColor = type == PropertyType::Color || type == PropertyType::ColorRGB;

        QDialog dialog(this);
        const QString channelName = QString::fromUtf8(PropertyChannelName(type, handle.Channel));
        dialog.setWindowTitle(QStringLiteral("Edit Key - %1%2").arg(QString::fromStdString(track->Name),
                                                                    channelName.isEmpty() ? QString() : QStringLiteral(" ") + channelName));
        auto* form = new QFormLayout(&dialog);
        auto* time = new QLineEdit(TimeText(original.Time), &dialog);
        auto* value = new QDoubleSpinBox(&dialog);
        value->setRange(-1.0e9, 1.0e9);
        value->setDecimals(channel.ValueKind() == ChannelValueKind::Float ? 4 : 0);
        value->setValue(original.Value);
        auto* interpolation = new QComboBox(&dialog);
        const KeyInterpolation modes[] = { KeyInterpolation::Auto,     KeyInterpolation::Cubic,  KeyInterpolation::Bezier,
                                           KeyInterpolation::Linear,   KeyInterpolation::Constant, KeyInterpolation::EaseIn,
                                           KeyInterpolation::EaseOut,  KeyInterpolation::EaseInOut };
        for (KeyInterpolation mode : modes)
        {
            interpolation->addItem(QString::fromUtf8(KeyInterpolationName(mode)), static_cast<int>(mode));
            if (mode == original.Interpolation)
                interpolation->setCurrentIndex(interpolation->count() - 1);
        }
        interpolation->setEnabled(channel.ValueKind() != ChannelValueKind::Bool);
        auto* tangents = new QComboBox(&dialog);
        tangents->addItem(QStringLiteral("Auto"), static_cast<int>(TangentMode::Auto));
        tangents->addItem(QStringLiteral("Unified"), static_cast<int>(TangentMode::Unified));
        tangents->addItem(QStringLiteral("Broken"), static_cast<int>(TangentMode::Broken));
        tangents->setCurrentIndex(static_cast<int>(original.Tangents));
        auto spin = [&](double initial) {
            auto* box = new QDoubleSpinBox(&dialog);
            box->setRange(-1.0e7, 1.0e7);
            box->setDecimals(4);
            box->setValue(initial);
            return box;
        };
        auto* arrive = spin(original.ArriveTangent);
        auto* leave = spin(original.LeaveTangent);
        auto* arriveWeight = spin(original.ArriveWeight);
        auto* leaveWeight = spin(original.LeaveWeight);
        arriveWeight->setMinimum(0.0);
        leaveWeight->setMinimum(0.0);
        arriveWeight->setSpecialValueText(QStringLiteral("Default"));
        leaveWeight->setSpecialValueText(QStringLiteral("Default"));

        auto updateEnabled = [&] {
            const auto mode = static_cast<KeyInterpolation>(interpolation->currentData().toInt());
            const auto tangentMode = static_cast<TangentMode>(tangents->currentData().toInt());
            const bool curve = mode == KeyInterpolation::Cubic || mode == KeyInterpolation::Bezier;
            tangents->setEnabled(curve);
            arrive->setEnabled(curve && tangentMode != TangentMode::Auto);
            leave->setEnabled(curve && tangentMode == TangentMode::Broken);
            arriveWeight->setEnabled(mode == KeyInterpolation::Bezier);
            leaveWeight->setEnabled(mode == KeyInterpolation::Bezier);
        };
        QObject::connect(interpolation, &QComboBox::currentIndexChanged, &dialog, [&](int) { updateEnabled(); });
        QObject::connect(tangents, &QComboBox::currentIndexChanged, &dialog, [&](int) { updateEnabled(); });
        updateEnabled();

        form->addRow(QStringLiteral("Time"), time);
        form->addRow(QStringLiteral("Value"), value);
        form->addRow(QStringLiteral("Interpolation"), interpolation);
        form->addRow(QStringLiteral("Tangents"), tangents);
        form->addRow(QStringLiteral("Tangent (in / unified)"), arrive);
        form->addRow(QStringLiteral("Tangent (out)"), leave);
        form->addRow(QStringLiteral("Weight in (s)"), arriveWeight);
        form->addRow(QStringLiteral("Weight out (s)"), leaveWeight);

        // A colour key is better picked than typed: this keys every colour channel at the
        // key's time from one picker.
        QColor pickedColor;
        if (isColor)
        {
            PropertyValue current;
            current.Components[3] = 1.0;
            property->EvaluateInto(original.Time, current);
            auto* pick = new QPushButton(QStringLiteral("Pick Colour..."), &dialog);
            QObject::connect(pick, &QPushButton::clicked, &dialog, [&, current] {
                const QColor initial = QColor::fromRgbF(float(std::clamp(current.Components[0], 0.0, 1.0)),
                                                        float(std::clamp(current.Components[1], 0.0, 1.0)),
                                                        float(std::clamp(current.Components[2], 0.0, 1.0)),
                                                        float(std::clamp(current.Components[3], 0.0, 1.0)));
                const QColor chosen = QColorDialog::getColor(
                    initial, &dialog, QStringLiteral("Key Colour"),
                    type == PropertyType::Color ? QColorDialog::ShowAlphaChannel : QColorDialog::ColorDialogOptions());
                if (chosen.isValid())
                {
                    pickedColor = chosen;
                    dialog.accept();
                }
            });
            form->addRow(QStringLiteral("Colour"), pick);
        }

        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
        form->addRow(buttons);
        QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        if (dialog.exec() != QDialog::Accepted)
            return;

        double seconds = original.Time;
        ParseSequencerTime(time->text(), rate, mModel.TimeDisplay, seconds);

        mDocument.Begin("Edit Key", SequencerDocument::Scope_None, { binding->Id });
        if (pickedColor.isValid())
        {
            PropertyValue color;
            color.Type = type;
            color.Components[0] = pickedColor.redF();
            color.Components[1] = pickedColor.greenF();
            color.Components[2] = pickedColor.blueF();
            color.Components[3] = pickedColor.alphaF();
            property->SetKey(original.Time, color, original.Interpolation);
        }
        else if (Keyframe* edited = channel.MutableKeyById(handle.Key))
        {
            edited->Time = seconds;
            edited->Value = value->value();
            edited->Interpolation = static_cast<KeyInterpolation>(interpolation->currentData().toInt());
            edited->Tangents = static_cast<TangentMode>(tangents->currentData().toInt());
            edited->ArriveTangent = arrive->value();
            edited->LeaveTangent = edited->Tangents == TangentMode::Broken ? leave->value() : arrive->value();
            edited->ArriveWeight = arriveWeight->value();
            edited->LeaveWeight = leaveWeight->value();
            channel.OnKeysChanged();
        }
        mDocument.MarkChanged(false);
        mDocument.Commit();
        Refresh();
    }

    // ---- Members -----------------------------------------------------------------------

    CinematicWorldHost mWorld;
    std::vector<Entity>* mWorldEntities = nullptr;
    SequencerDocument mDocument;
    SequencerTimelineModel mModel;
    std::uint64_t mLastRevision = 0;
    FrameRate mRateComboRate{ 0, 0 };

    SequencerTrackTree* mTree = nullptr;
    SequencerTimeline* mTimeline = nullptr;
    QScrollBar* mScrollBar = nullptr;
    QLineEdit* mTimeField = nullptr;
    QComboBox* mDisplayCombo = nullptr;
    QComboBox* mRateCombo = nullptr;
    QLabel* mFileLabel = nullptr;

    QAction* mNewAction = nullptr;
    QAction* mOpenAction = nullptr;
    QAction* mSaveAction = nullptr;
    QAction* mSaveAsAction = nullptr;
    QAction* mUndoAction = nullptr;
    QAction* mRedoAction = nullptr;
    QAction* mJumpStartAction = nullptr;
    QAction* mPreviousFrameAction = nullptr;
    QAction* mPlayFromStartAction = nullptr;
    QAction* mPlayAction = nullptr;
    QAction* mPauseAction = nullptr;
    QAction* mStopAction = nullptr;
    QAction* mNextFrameAction = nullptr;
    QAction* mJumpEndAction = nullptr;
    QAction* mLoopAction = nullptr;
    QAction* mAutoKeyAction = nullptr;
    QAction* mAddKeyAction = nullptr;
    QAction* mSnapAction = nullptr;
    QAction* mSettingsAction = nullptr;
    QAction* mLockCameraAction = nullptr;
    QAction* mRenderAction = nullptr;
    QToolBar* mToolbar = nullptr;

    bool mRendering = false;
    bool mLockBeforeRender = false;
    // What a render looks through: the cut track (falling back to the first camera where
    // no cut covers a frame), one chosen camera binding, or the free viewport.
    enum class RenderCamera
    {
        Cuts,
        Binding,
        Viewport
    };
    RenderCamera mRenderCamera = RenderCamera::Cuts;
    Guid mRenderCameraBinding;
    bool mRenderWasActive = false;
    QProgressDialog* mRenderProgress = nullptr;
};

QDockWidget* gDock = nullptr;
SequencerPanel* gPanel = nullptr;
int gSuspendCount = 0;

SequencerPanel* EnsurePanel()
{
    if (gPanel != nullptr)
        return gPanel;
    // QtUi owns the QApplication; before it exists there is nothing to build on.
    if (QApplication::instance() == nullptr || gHost == nullptr)
        return nullptr;

    auto* shell = static_cast<QMainWindow*>(QtUi::ShellWidget());
    gDock = new QDockWidget(QStringLiteral("Sequencer"), shell);
    gDock->setObjectName(QStringLiteral("PteroSequencerDock"));
    gDock->setFont(QApplication::font());
    // The editor polls its shortcuts from the keyboard; this keeps them quiet while the
    // Sequencer has focus (see QtUi's WantTextInput).
    gDock->setProperty("pteroKeyboardOwner", true);
    gPanel = new SequencerPanel(gDock);
    gDock->setWidget(gPanel);
    if (shell != nullptr)
    {
        shell->addDockWidget(Qt::BottomDockWidgetArea, gDock);
        // Puts the dock back where the user left it last session, if the saved layout
        // has it; otherwise it stays along the bottom.
        shell->restoreDockWidget(gDock);
    }
    gDock->resize(gDock->width(), 320);

    // Closing the dock ends the preview: the level goes back to how it was authored.
    // A render in progress is cancelled with it - the render needs the preview it ends.
    QObject::connect(gDock, &QDockWidget::visibilityChanged, gDock, [](bool visible) {
        if (!visible && gPanel != nullptr && gDock != nullptr && gDock->isHidden())
        {
            if (gPanel->IsRendering() && gHost != nullptr)
                gHost->CancelCinematicRender();
            gPanel->Document().EndPreview();
            gCameraLock = false;
        }
    });

    for (int i = 0; i < gSuspendCount; ++i)
        gPanel->Document().SuspendPreview();
    return gPanel;
}
} // namespace

namespace SequencerEditor
{
void SetHost(SequencerEditorHost* host)
{
    gHost = host;
}

void Show()
{
    SequencerPanel* panel = EnsurePanel();
    if (panel == nullptr || gDock == nullptr)
        return;
    gDock->show();
    gDock->raise();
}

bool IsVisible()
{
    return gDock != nullptr && gDock->isVisible();
}

void Shutdown()
{
    if (gPanel != nullptr)
        gPanel->Document().EndPreview();
    delete gDock;
    gDock = nullptr;
    gPanel = nullptr;
}

bool OpenSequence(const std::string& path)
{
    SequencerPanel* panel = EnsurePanel();
    if (panel == nullptr)
        return false;
    Show();
    return panel->OpenFile(QString::fromStdString(path));
}

void Tick(float deltaSeconds)
{
    // A tabbed-away dock still renders and still reports render progress.
    if (gPanel != nullptr && gDock != nullptr && (gDock->isVisible() || gPanel->IsRendering()))
        gPanel->Tick(deltaSeconds);
}

void OnSceneChanged()
{
    if (gPanel != nullptr && gDock != nullptr && gDock->isVisible())
        gPanel->OnSceneChanged();
}

void OnLevelReplaced()
{
    if (gPanel != nullptr)
        gPanel->Document().OnWorldReplaced();
}

void OnLevelRestored()
{
    if (gPanel != nullptr)
        gPanel->Document().MarkEvaluationDirty();
}

void SuspendPreview()
{
    ++gSuspendCount;
    if (gPanel != nullptr)
        gPanel->Document().SuspendPreview();
}

void ResumePreview()
{
    if (gSuspendCount == 0)
        return;
    --gSuspendCount;
    if (gPanel != nullptr)
        gPanel->Document().ResumePreview();
}

void SetCameraLock(bool locked)
{
    gCameraLock = locked;
    if (gPanel != nullptr)
        gPanel->SetCameraLockChecked(locked);
}

bool IsCameraLocked()
{
    return gCameraLock;
}

std::uint64_t ActiveCutCamera(double& shakeTime)
{
    if (gPanel == nullptr || gDock == nullptr || (!gDock->isVisible() && !gPanel->IsRendering()))
        return 0;
    return gPanel->ActiveCutCamera(shakeTime);
}

bool ConfirmClose()
{
    return gPanel == nullptr || gPanel->ConfirmDiscard();
}

double PreviewAspect()
{
    return gPanel != nullptr ? gPanel->PreviewAspect() : 16.0 / 9.0;
}

bool BeginCinematicRender()
{
    return gPanel != nullptr && gPanel->BeginRender();
}

void SetCinematicRenderTime(double seconds)
{
    if (gPanel != nullptr)
        gPanel->SetRenderTime(seconds);
}

void EndCinematicRender()
{
    if (gPanel != nullptr)
        gPanel->EndRender();
}
} // namespace SequencerEditor
