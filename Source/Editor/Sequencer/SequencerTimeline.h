#pragma once

// The Sequencer's two custom-painted panes - the track tree on the left and the timeline on
// the right - and the row model they share.
//
// Both panes paint straight from SequencerTimelineModel's flattened row list, which is
// rebuilt only when the sequence's structure or an expansion changes. Painting walks only
// the rows on screen and, per row, only the keys inside the visible time span (found by
// binary search, keys being sorted), so hundreds of tracks and thousands of keys cost
// what is visible, not what exists.

#include "SequencerDocument.h"

#pragma warning(push)
#pragma warning(disable : 4996)
#include <QtGui/QColor>
#include <QtWidgets/QWidget>
#pragma warning(pop)

#include <functional>
#include <map>
#include <set>
#include <vector>

class QMenu;
class SequencerEditorHost;

enum class SequencerTimeDisplay
{
    Timecode,
    Seconds,
    Frames
};

QString FormatSequencerTime(double seconds, const FrameRate& rate, SequencerTimeDisplay display);
// Accepts what FormatSequencerTime writes, plus shortened timecode ("4:12" is 4 s 12 f).
bool ParseSequencerTime(const QString& text, const FrameRate& rate, SequencerTimeDisplay display, double& seconds);

struct SequencerRow
{
    enum class Kind
    {
        Folder,     // Cameras / Lights / Actors / Environment
        CameraCuts,
        Events,
        Binding,
        Track,
        Group,      // a transform's Location / Rotation / Scale
        Channel
    };

    Kind RowKind = Kind::Folder;
    int Depth = 0;
    QString Label;
    QString Tooltip;
    // Stable identity for expansion and row selection: binding or track guid, plus the
    // channel range for groups and channels.
    QString Key;
    Guid Binding;
    Guid Track;
    int ChannelFirst = 0;
    int ChannelCount = 0;
    bool Expandable = false;
    bool Expanded = false;
    bool Muted = false;
    QColor Accent;
    // Content coordinates; the panes subtract the scroll offset.
    int Top = 0;
    int Height = 22;
};

// What the panes need from the window that hosts them.
struct SequencerUiActions
{
    std::function<void(const SequencerKeyHandle&)> EditKey;
    std::function<void(double time)> AddEventAt;
    std::function<void(const Guid& binding, const QPoint& globalPosition)> ShowAddTrackMenu;
    std::function<void(QMenu& menu, double time)> FillAddCutMenu;
    std::function<void(const Guid& binding)> SelectBindingEntity;
    std::function<void(const Guid& binding)> RebindToSelection;
    std::function<void(const Guid& binding)> PilotBinding;
    std::function<bool(const Guid& binding)> IsCameraBinding;
    std::function<bool(const SequenceBinding& binding)> IsBindingResolved;
    std::function<void()> ScaleSelection;
    // Something changed that the toolbar or the other pane must reflect.
    std::function<void()> Changed;
    std::function<void(const QString& message)> Status;
};

class SequencerTimelineModel
{
public:
    SequencerDocument* Document = nullptr;
    SequencerEditorHost* Host = nullptr;
    SequencerUiActions Actions;
    SequencerTimeDisplay TimeDisplay = SequencerTimeDisplay::Timecode;

    // Vertical scroll of both panes, in pixels.
    int ScrollY = 0;
    // The visible time span of the timeline.
    double ViewStart = -0.5;
    double ViewEnd = 5.5;
    // A range marked in the ruler with Ctrl-drag: frame-to-selection and key selection
    // work on it. Empty when ViewSelectionEnd <= ViewSelectionStart.
    double SelectionStart = 0.0;
    double SelectionEnd = 0.0;
    bool HasSelectionRange() const { return SelectionEnd > SelectionStart; }

    std::set<QString> SelectedRows;

    const std::vector<SequencerRow>& Rows();
    // Forces the row list to be rebuilt on next use - after an expansion, or after the
    // level changed under the bindings (a camera component added, an entity deleted).
    void Invalidate() { mDirty = true; }
    int ContentHeight();
    const SequencerRow* RowAtContentY(int y);
    const SequencerRow* FindRow(const QString& key);

    bool IsExpanded(const QString& key, bool fallback) const;
    void SetExpanded(const QString& key, bool expanded);

    double TimeToX(double time, int width) const;
    double XToTime(double x, int width) const;

    struct RowKey
    {
        double Time = 0.0;
        KeyInterpolation Interpolation = KeyInterpolation::Auto;
        bool Selected = false;
        // Every key this mark stands for: one for a channel row, all the channels' keys at
        // that instant for a track, group or binding row.
        std::vector<SequencerKeyHandle> Handles;
        // For an event mark.
        QString Name;
    };
    // The keys a row shows between t0 and t1, in time order.
    void CollectRowKeys(const SequencerRow& row, double t0, double t1, std::vector<RowKey>& out);

    // The property tracks under a row (all of a binding's, one for a track row).
    std::vector<std::pair<PropertyTrack*, unsigned>> TracksOfRow(const SequencerRow& row);

    // The cut camera's display name, or a placeholder when its binding is gone.
    QString CameraName(const Guid& binding);

private:
    void Rebuild();
    QString BindingFolder(const SequenceBinding& binding) const;

    std::vector<SequencerRow> mRows;
    std::map<QString, bool> mExpanded;
    std::uint64_t mBuiltStructure = 0;
    bool mDirty = true;
};

// Shared colours, taken from the active editor style so the panes follow it.
namespace SequencerColors
{
    QColor Background();
    QColor Panel();
    QColor Header();
    QColor RowAlternate();
    QColor Border();
    QColor Text();
    QColor TextDim();
    QColor Accent();
    QColor Playhead();
    QColor Channel(PropertyType type, int channel);
}

class SequencerTrackTree final : public QWidget
{
public:
    SequencerTrackTree(SequencerTimelineModel& model, QWidget* parent);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    enum class Button
    {
        None,
        Expand,
        PreviousKey,
        AddKey,
        NextKey,
        AddTrack,
        AddCut,
        AddEvent
    };
    Button HitButton(const SequencerRow& row, const QPoint& position) const;
    void JumpToKey(const SequencerRow& row, bool forward);
    unsigned ChannelMask(const SequencerRow& row) const;

    SequencerTimelineModel& mModel;
};

class SequencerTimeline final : public QWidget
{
public:
    static constexpr int kRulerHeight = 28;

    SequencerTimeline(SequencerTimelineModel& model, QWidget* parent);

    // Fits the view to every key (or to the playback range when there are none).
    void FrameAll();
    void FrameSelection();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    enum class Drag
    {
        None,
        Scrub,
        PlaybackStart,
        PlaybackEnd,
        SelectionRange,
        Keys,
        Box,
        CutStart,
        CutEnd,
        CutMove,
        Pan
    };

    void PaintRuler(class QPainter& painter);
    void PaintRows(class QPainter& painter);
    void PaintCuts(class QPainter& painter, const SequencerRow& row, int y);

    // The key mark under `position` in `row`, if any.
    bool HitKey(const SequencerRow& row, const QPoint& position, SequencerTimelineModel::RowKey& key);
    // A camera cut under `position`: which one, and which part (0 start edge, 1 end edge,
    // 2 body). False when there is none.
    bool HitCut(const QPoint& position, Guid& cut, int& part);
    const SequencerRow* RowAt(int y);
    void SelectInBox();
    void Scrub(int x);

    SequencerTimelineModel& mModel;
    Drag mDrag = Drag::None;
    QPoint mPressPosition;
    double mPressTime = 0.0;
    double mPressViewStart = 0.0;
    double mPressViewEnd = 0.0;
    bool mDragStarted = false;
    SequencerKeyHandle mDragAnchor;
    QRect mBox;
    std::set<SequencerKeyHandle> mBoxBase;
    Guid mDragCut;
};
