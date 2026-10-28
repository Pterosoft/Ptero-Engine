#include "SequencerTimeline.h"

#include "SequencerEditor.h"

#include "../../QtUi/QtUiTheme.h"
#include "../../Renderer_DX12/Components.h"

#pragma warning(push)
#pragma warning(disable : 4996)
#include <QtWidgets/QtWidgets>
#pragma warning(pop)

#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------------------
// Time text
// ---------------------------------------------------------------------------------------

QString FormatSequencerTime(double seconds, const FrameRate& rate, SequencerTimeDisplay display)
{
    switch (display)
    {
    case SequencerTimeDisplay::Seconds:
        return QString::number(seconds, 'f', 3) + QStringLiteral(" s");
    case SequencerTimeDisplay::Frames:
        return QString::number(rate.SecondsToNearestFrame(seconds));
    case SequencerTimeDisplay::Timecode:
    default:
    {
        // Frames within the second count at the nominal integer rate, the way an edit
        // suite labels 29.97 material (non-drop).
        const std::int64_t framesPerSecond = std::max<std::int64_t>(1, std::llround(rate.FramesPerSecond()));
        std::int64_t frames = rate.SecondsToNearestFrame(seconds);
        const bool negative = frames < 0;
        frames = std::llabs(frames);
        const std::int64_t frame = frames % framesPerSecond;
        const std::int64_t totalSeconds = frames / framesPerSecond;
        return QStringLiteral("%1%2:%3:%4:%5")
            .arg(negative ? QStringLiteral("-") : QString())
            .arg(totalSeconds / 3600, 2, 10, QLatin1Char('0'))
            .arg((totalSeconds / 60) % 60, 2, 10, QLatin1Char('0'))
            .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'))
            .arg(frame, 2, 10, QLatin1Char('0'));
    }
    }
}

bool ParseSequencerTime(const QString& input, const FrameRate& rate, SequencerTimeDisplay display, double& seconds)
{
    QString text = input.trimmed();
    if (text.endsWith(QLatin1Char('s')))
    {
        text.chop(1);
        bool ok = false;
        const double value = text.trimmed().toDouble(&ok);
        if (ok)
            seconds = value;
        return ok;
    }

    if (text.contains(QLatin1Char(':')))
    {
        const bool negative = text.startsWith(QLatin1Char('-'));
        if (negative)
            text.remove(0, 1);
        const QStringList parts = text.split(QLatin1Char(':'));
        if (parts.size() > 4)
            return false;
        // Read from the right: frames, seconds, minutes, hours.
        const double multipliers[] = { 1.0 / rate.FramesPerSecond(), 1.0, 60.0, 3600.0 };
        double total = 0.0;
        for (int i = 0; i < parts.size(); ++i)
        {
            bool ok = false;
            const int value = parts[parts.size() - 1 - i].toInt(&ok);
            if (!ok)
                return false;
            total += value * multipliers[i];
        }
        seconds = negative ? -total : total;
        return true;
    }

    bool ok = false;
    const double value = text.toDouble(&ok);
    if (!ok)
        return false;
    seconds = display == SequencerTimeDisplay::Seconds ? value : rate.FrameToSeconds(value);
    return true;
}

// ---------------------------------------------------------------------------------------
// Colours
// ---------------------------------------------------------------------------------------

namespace SequencerColors
{
    namespace
    {
        QColor Token(const char* token, const QColor& fallback)
        {
            const QColor color = QtUiTheme::Color(token);
            return color.isValid() ? color : fallback;
        }

        bool IsDark()
        {
            return Token("base", QColor(30, 31, 34)).lightness() < 128;
        }
    }

    QColor Background() { return Token("base", QColor(27, 29, 33)); }
    QColor Panel() { return Token("panel", QColor(36, 38, 42)); }
    QColor Header() { return Token("header", QColor(44, 47, 52)); }
    QColor RowAlternate() { return Token("alternateBase", QColor(33, 35, 39)); }
    QColor Border() { return Token("border", QColor(58, 61, 67)); }
    QColor Text() { return Token("text", QColor(220, 222, 226)); }
    QColor TextDim() { return Token("textDim", QColor(140, 145, 152)); }
    QColor Accent() { return Token("accent", QColor(74, 136, 214)); }
    QColor Playhead() { return IsDark() ? QColor(232, 96, 72) : QColor(200, 60, 40); }

    QColor Channel(PropertyType type, int channel)
    {
        if (type == PropertyType::Color || type == PropertyType::ColorRGB)
        {
            static const QColor kColor[] = { QColor(214, 86, 86), QColor(104, 186, 96), QColor(86, 132, 222),
                                             QColor(170, 170, 170) };
            return kColor[std::clamp(channel, 0, 3)];
        }
        // X, Y, Z in the engine's axis colours; for a transform the pattern repeats per group.
        static const QColor kAxis[] = { QColor(205, 84, 84), QColor(110, 180, 92), QColor(84, 128, 214),
                                        QColor(170, 170, 170) };
        const int axis = type == PropertyType::Transform ? channel % 3 : channel;
        return kAxis[std::clamp(axis, 0, 3)];
    }
}

namespace
{
    // Key marks are coloured by how the curve leaves them; selection overrides.
    QColor InterpolationColor(KeyInterpolation interpolation)
    {
        switch (interpolation)
        {
        case KeyInterpolation::Constant: return QColor(214, 152, 86);
        case KeyInterpolation::Linear: return QColor(128, 188, 120);
        case KeyInterpolation::Bezier:
        case KeyInterpolation::Cubic: return QColor(176, 150, 214);
        case KeyInterpolation::EaseIn:
        case KeyInterpolation::EaseOut:
        case KeyInterpolation::EaseInOut: return QColor(100, 186, 190);
        case KeyInterpolation::Auto:
        default: return QColor(170, 180, 194);
        }
    }

    int RowHeight(SequencerRow::Kind kind)
    {
        switch (kind)
        {
        case SequencerRow::Kind::Folder: return 20;
        case SequencerRow::Kind::CameraCuts: return 28;
        case SequencerRow::Kind::Events: return 24;
        case SequencerRow::Kind::Binding: return 24;
        case SequencerRow::Kind::Track: return 22;
        default: return 20;
        }
    }

    constexpr int kIndent = 14;
    constexpr int kButtonWidth = 18;
    constexpr double kMinimumSpan = 0.05;
    constexpr double kMaximumSpan = 4.0 * 3600.0;

    void DrawDiamond(QPainter& painter, QPointF center, double radius, const QColor& fill, const QColor& outline)
    {
        const QPointF points[] = { { center.x(), center.y() - radius },
                                   { center.x() + radius, center.y() },
                                   { center.x(), center.y() + radius },
                                   { center.x() - radius, center.y() } };
        painter.setPen(QPen(outline, 1.0));
        painter.setBrush(fill);
        painter.drawPolygon(points, 4);
    }

    void DrawTriangle(QPainter& painter, QPointF center, double size, bool pointRight, bool pointDown,
                      const QColor& color)
    {
        QPointF points[3];
        if (pointDown)
        {
            points[0] = { center.x() - size, center.y() - size * 0.5 };
            points[1] = { center.x() + size, center.y() - size * 0.5 };
            points[2] = { center.x(), center.y() + size * 0.6 };
        }
        else if (pointRight)
        {
            points[0] = { center.x() - size * 0.5, center.y() - size };
            points[1] = { center.x() + size * 0.6, center.y() };
            points[2] = { center.x() - size * 0.5, center.y() + size };
        }
        else
        {
            points[0] = { center.x() + size * 0.5, center.y() - size };
            points[1] = { center.x() - size * 0.6, center.y() };
            points[2] = { center.x() + size * 0.5, center.y() + size };
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawPolygon(points, 3);
    }

    void DrawPlus(QPainter& painter, QPointF center, double size, const QColor& color)
    {
        painter.setPen(QPen(color, 1.5));
        painter.drawLine(QPointF(center.x() - size, center.y()), QPointF(center.x() + size, center.y()));
        painter.drawLine(QPointF(center.x(), center.y() - size), QPointF(center.x(), center.y() + size));
    }

    // Keys of one channel between t0 and t1, found by search: keys are kept sorted.
    template <class Visit>
    void ForKeysInRange(const FloatChannel& channel, double t0, double t1, Visit visit)
    {
        const std::vector<Keyframe>& keys = channel.Keys();
        auto it = std::lower_bound(keys.begin(), keys.end(), t0,
                                   [](const Keyframe& key, double t) { return key.Time < t; });
        for (; it != keys.end() && it->Time <= t1; ++it)
            visit(*it);
    }
}

// ---------------------------------------------------------------------------------------
// SequencerTimelineModel
// ---------------------------------------------------------------------------------------

QString SequencerTimelineModel::BindingFolder(const SequenceBinding& binding) const
{
    if (binding.Domain == "Environment")
        return QStringLiteral("Environment");

    if (Host != nullptr && Host->Entities() != nullptr)
    {
        const Entity* found = nullptr;
        for (const Entity& entity : *Host->Entities())
        {
            if ((binding.EntityId != 0 && entity.Id == binding.EntityId) ||
                (found == nullptr && !binding.EntityName.empty() && entity.Name == binding.EntityName))
            {
                found = &entity;
                if (entity.Id == binding.EntityId)
                    break;
            }
        }
        if (found != nullptr)
        {
            if (found->Camera)
                return QStringLiteral("Cameras");
            if (found->PointLight)
                return QStringLiteral("Lights");
        }
    }
    return QStringLiteral("Actors");
}

bool SequencerTimelineModel::IsExpanded(const QString& key, bool fallback) const
{
    const auto it = mExpanded.find(key);
    return it != mExpanded.end() ? it->second : fallback;
}

void SequencerTimelineModel::SetExpanded(const QString& key, bool expanded)
{
    mExpanded[key] = expanded;
    mDirty = true;
}

void SequencerTimelineModel::Rebuild()
{
    mRows.clear();
    if (Document == nullptr)
        return;

    int y = 0;
    auto add = [&](SequencerRow row) {
        row.Height = RowHeight(row.RowKind);
        row.Top = y;
        y += row.Height;
        mRows.push_back(std::move(row));
    };

    const CinematicSequence& sequence = Document->Sequence();

    if (const CameraCutTrack* cuts = sequence.FindCameraCutTrack())
    {
        SequencerRow row;
        row.RowKind = SequencerRow::Kind::CameraCuts;
        row.Label = QStringLiteral("Camera Cuts");
        row.Tooltip = QStringLiteral("Which camera the sequence looks through. Drag a cut's edges to change "
                                     "how long it lasts.");
        row.Key = QStringLiteral("cuts:") + QString::fromStdString(cuts->Id.ToString());
        row.Track = cuts->Id;
        row.Muted = cuts->Muted;
        add(row);
    }

    static const char* kFolders[] = { "Cameras", "Actors", "Lights", "Environment" };
    for (const char* folderName : kFolders)
    {
        const QString folder = QString::fromUtf8(folderName);
        std::vector<const SequenceBinding*> bindings;
        for (const SequenceBinding& binding : sequence.Bindings)
        {
            if (BindingFolder(binding) == folder)
                bindings.push_back(&binding);
        }
        if (bindings.empty())
            continue;

        SequencerRow folderRow;
        folderRow.RowKind = SequencerRow::Kind::Folder;
        folderRow.Label = folder;
        folderRow.Key = QStringLiteral("folder:") + folder;
        folderRow.Expandable = true;
        folderRow.Expanded = IsExpanded(folderRow.Key, true);
        const bool folderOpen = folderRow.Expanded;
        add(folderRow);
        if (!folderOpen)
            continue;

        for (const SequenceBinding* binding : bindings)
        {
            SequencerRow bindingRow;
            bindingRow.RowKind = SequencerRow::Kind::Binding;
            bindingRow.Depth = 1;
            bindingRow.Label = QString::fromStdString(binding->Name.empty() ? binding->EntityName : binding->Name);
            bindingRow.Key = QString::fromStdString(binding->Id.ToString());
            bindingRow.Binding = binding->Id;
            bindingRow.Expandable = !binding->Tracks.empty();
            bindingRow.Expanded = IsExpanded(bindingRow.Key, true);
            bindingRow.Tooltip = binding->Domain == "Environment"
                ? QStringLiteral("Engine-wide settings")
                : QStringLiteral("Entity '%1' (id %2)")
                      .arg(QString::fromStdString(binding->EntityName))
                      .arg(binding->EntityId);
            const bool bindingOpen = bindingRow.Expandable && bindingRow.Expanded;
            add(bindingRow);
            if (!bindingOpen)
                continue;

            for (const std::unique_ptr<SequenceTrack>& track : binding->Tracks)
            {
                const auto* property = dynamic_cast<const PropertyTrack*>(track.get());
                if (property == nullptr)
                    continue;

                SequencerRow trackRow;
                trackRow.RowKind = SequencerRow::Kind::Track;
                trackRow.Depth = 2;
                trackRow.Label = QString::fromStdString(track->Name.empty() ? property->PropertyPath() : track->Name);
                trackRow.Key = QString::fromStdString(track->Id.ToString());
                trackRow.Binding = binding->Id;
                trackRow.Track = track->Id;
                trackRow.ChannelFirst = 0;
                trackRow.ChannelCount = property->ChannelCount();
                trackRow.Muted = track->Muted;
                trackRow.Expandable = property->ChannelCount() > 1;
                trackRow.Expanded = IsExpanded(trackRow.Key, false);
                trackRow.Tooltip = QString::fromStdString(property->PropertyPath());
                const bool trackOpen = trackRow.Expandable && trackRow.Expanded;
                add(trackRow);
                if (!trackOpen)
                    continue;

                const PropertyType type = property->ValueType();
                auto addChannel = [&](int channel, int depth) {
                    SequencerRow channelRow;
                    channelRow.RowKind = SequencerRow::Kind::Channel;
                    channelRow.Depth = depth;
                    QString name = QString::fromUtf8(PropertyChannelName(type, channel));
                    if (type == PropertyType::Transform)
                        name = name.mid(name.indexOf(QLatin1Char('.')) + 1);
                    channelRow.Label = name;
                    channelRow.Key = trackRow.Key + QStringLiteral(":%1").arg(channel);
                    channelRow.Binding = binding->Id;
                    channelRow.Track = track->Id;
                    channelRow.ChannelFirst = channel;
                    channelRow.ChannelCount = 1;
                    channelRow.Muted = track->Muted;
                    channelRow.Accent = SequencerColors::Channel(type, channel);
                    add(channelRow);
                };

                if (type == PropertyType::Transform)
                {
                    static const char* kGroups[] = { "Location", "Rotation", "Scale" };
                    for (int group = 0; group < 3; ++group)
                    {
                        SequencerRow groupRow;
                        groupRow.RowKind = SequencerRow::Kind::Group;
                        groupRow.Depth = 3;
                        groupRow.Label = QString::fromUtf8(kGroups[group]);
                        groupRow.Key = trackRow.Key + QStringLiteral(":g%1").arg(group);
                        groupRow.Binding = binding->Id;
                        groupRow.Track = track->Id;
                        groupRow.ChannelFirst = group * 3;
                        groupRow.ChannelCount = 3;
                        groupRow.Muted = track->Muted;
                        groupRow.Expandable = true;
                        groupRow.Expanded = IsExpanded(groupRow.Key, false);
                        const bool groupOpen = groupRow.Expanded;
                        add(groupRow);
                        if (groupOpen)
                        {
                            for (int axis = 0; axis < 3; ++axis)
                                addChannel(group * 3 + axis, 4);
                        }
                    }
                }
                else
                {
                    for (int channel = 0; channel < property->ChannelCount(); ++channel)
                        addChannel(channel, 3);
                }
            }
        }
    }

    for (const std::unique_ptr<SequenceTrack>& track : sequence.MasterTracks)
    {
        if (dynamic_cast<const EventTrack*>(track.get()) == nullptr)
            continue;
        SequencerRow row;
        row.RowKind = SequencerRow::Kind::Events;
        row.Label = QString::fromStdString(track->Name.empty() ? std::string("Events") : track->Name);
        row.Key = QStringLiteral("events:") + QString::fromStdString(track->Id.ToString());
        row.Track = track->Id;
        row.Muted = track->Muted;
        row.Tooltip = QStringLiteral("Markers fired as playback crosses them. Double-click to add one.");
        add(row);
    }

    mBuiltStructure = Document->StructureRevision();
    mDirty = false;
}

const std::vector<SequencerRow>& SequencerTimelineModel::Rows()
{
    if (mDirty || Document == nullptr || mBuiltStructure != Document->StructureRevision())
        Rebuild();
    return mRows;
}

int SequencerTimelineModel::ContentHeight()
{
    const std::vector<SequencerRow>& rows = Rows();
    return rows.empty() ? 0 : rows.back().Top + rows.back().Height;
}

const SequencerRow* SequencerTimelineModel::RowAtContentY(int y)
{
    const std::vector<SequencerRow>& rows = Rows();
    // Rows are laid out top to bottom, so the row is found by search.
    auto it = std::upper_bound(rows.begin(), rows.end(), y, [](int value, const SequencerRow& row) { return value < row.Top; });
    if (it == rows.begin())
        return nullptr;
    --it;
    return y < it->Top + it->Height ? &*it : nullptr;
}

const SequencerRow* SequencerTimelineModel::FindRow(const QString& key)
{
    for (const SequencerRow& row : Rows())
    {
        if (row.Key == key)
            return &row;
    }
    return nullptr;
}

double SequencerTimelineModel::TimeToX(double time, int width) const
{
    return (time - ViewStart) / (ViewEnd - ViewStart) * width;
}

double SequencerTimelineModel::XToTime(double x, int width) const
{
    return ViewStart + x / (std::max)(1, width) * (ViewEnd - ViewStart);
}

std::vector<std::pair<PropertyTrack*, unsigned>> SequencerTimelineModel::TracksOfRow(const SequencerRow& row)
{
    std::vector<std::pair<PropertyTrack*, unsigned>> result;
    if (Document == nullptr)
        return result;

    if (row.RowKind == SequencerRow::Kind::Binding)
    {
        if (SequenceBinding* binding = Document->Sequence().FindBinding(row.Binding))
        {
            for (const std::unique_ptr<SequenceTrack>& track : binding->Tracks)
            {
                if (auto* property = dynamic_cast<PropertyTrack*>(track.get()))
                    result.push_back({ property, (1u << property->ChannelCount()) - 1u });
            }
        }
        return result;
    }

    if (row.RowKind == SequencerRow::Kind::Track || row.RowKind == SequencerRow::Kind::Group ||
        row.RowKind == SequencerRow::Kind::Channel)
    {
        if (auto* property = dynamic_cast<PropertyTrack*>(Document->FindTrack(row.Track)))
        {
            unsigned mask = 0;
            for (int i = row.ChannelFirst; i < row.ChannelFirst + row.ChannelCount; ++i)
                mask |= 1u << i;
            result.push_back({ property, mask });
        }
    }
    return result;
}

void SequencerTimelineModel::CollectRowKeys(const SequencerRow& row, double t0, double t1, std::vector<RowKey>& out)
{
    out.clear();
    if (Document == nullptr)
        return;
    const std::set<SequencerKeyHandle>& selected = Document->SelectedKeys();

    if (row.RowKind == SequencerRow::Kind::Events)
    {
        if (auto* events = dynamic_cast<EventTrack*>(Document->FindTrack(row.Track)))
        {
            for (const SequenceEventKey& key : events->Keys())
            {
                if (key.Time < t0 || key.Time > t1)
                    continue;
                RowKey mark;
                mark.Time = key.Time;
                mark.Name = QString::fromStdString(key.Name);
                mark.Handles.push_back({ row.Track, SequencerKeyHandle::kEventChannel, key.Id });
                mark.Selected = selected.count(mark.Handles.back()) != 0;
                out.push_back(std::move(mark));
            }
        }
        return;
    }

    struct Raw
    {
        double Time;
        KeyInterpolation Interpolation;
        SequencerKeyHandle Handle;
    };
    std::vector<Raw> raw;
    for (const auto& [track, mask] : TracksOfRow(row))
    {
        for (int channel = 0; channel < track->ChannelCount(); ++channel)
        {
            if ((mask & (1u << channel)) == 0)
                continue;
            ForKeysInRange(track->Channel(channel), t0, t1, [&](const Keyframe& key) {
                raw.push_back({ key.Time, key.Interpolation, { track->Id, channel, key.Id } });
            });
        }
    }

    std::sort(raw.begin(), raw.end(), [](const Raw& a, const Raw& b) { return a.Time < b.Time; });
    for (const Raw& entry : raw)
    {
        // Keys at one instant on several channels make one mark on a summary row.
        if (!out.empty() && std::fabs(out.back().Time - entry.Time) < 1e-6)
        {
            out.back().Handles.push_back(entry.Handle);
            out.back().Selected = out.back().Selected || selected.count(entry.Handle) != 0;
            continue;
        }
        RowKey mark;
        mark.Time = entry.Time;
        mark.Interpolation = entry.Interpolation;
        mark.Handles.push_back(entry.Handle);
        mark.Selected = selected.count(entry.Handle) != 0;
        out.push_back(std::move(mark));
    }
}

QString SequencerTimelineModel::CameraName(const Guid& binding)
{
    if (Document != nullptr)
    {
        if (const SequenceBinding* camera = Document->Sequence().FindBinding(binding))
            return QString::fromStdString(camera->Name.empty() ? camera->EntityName : camera->Name);
    }
    return QStringLiteral("(no camera)");
}

// ---------------------------------------------------------------------------------------
// SequencerTrackTree
// ---------------------------------------------------------------------------------------

SequencerTrackTree::SequencerTrackTree(SequencerTimelineModel& model, QWidget* parent)
    : QWidget(parent), mModel(model)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumWidth(180);
}

unsigned SequencerTrackTree::ChannelMask(const SequencerRow& row) const
{
    unsigned mask = 0;
    for (int i = row.ChannelFirst; i < row.ChannelFirst + row.ChannelCount; ++i)
        mask |= 1u << i;
    return mask;
}

SequencerTrackTree::Button SequencerTrackTree::HitButton(const SequencerRow& row, const QPoint& position) const
{
    const int arrowX = 4 + row.Depth * kIndent;
    if (row.Expandable && position.x() >= arrowX && position.x() < arrowX + 14)
        return Button::Expand;

    const int right = width() - 4;
    const bool keyed = row.RowKind == SequencerRow::Kind::Track || row.RowKind == SequencerRow::Kind::Group ||
                       row.RowKind == SequencerRow::Kind::Channel;
    if (keyed)
    {
        if (position.x() >= right - kButtonWidth)
            return Button::NextKey;
        if (position.x() >= right - 2 * kButtonWidth)
            return Button::AddKey;
        if (position.x() >= right - 3 * kButtonWidth)
            return Button::PreviousKey;
        return Button::None;
    }
    if (position.x() >= right - kButtonWidth)
    {
        switch (row.RowKind)
        {
        case SequencerRow::Kind::Binding: return Button::AddTrack;
        case SequencerRow::Kind::CameraCuts: return Button::AddCut;
        case SequencerRow::Kind::Events: return Button::AddEvent;
        default: break;
        }
    }
    return Button::None;
}

void SequencerTrackTree::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const int w = width();
    const int h = height();
    painter.fillRect(rect(), SequencerColors::Panel());

    SequencerDocument* document = mModel.Document;
    const double time = document != nullptr ? document->Time() : 0.0;
    const QFontMetrics metrics = fontMetrics();

    painter.save();
    painter.setClipRect(0, SequencerTimeline::kRulerHeight, w, h - SequencerTimeline::kRulerHeight);
    const int offset = SequencerTimeline::kRulerHeight - mModel.ScrollY;
    int index = 0;
    for (const SequencerRow& row : mModel.Rows())
    {
        ++index;
        const int y = row.Top + offset;
        if (y + row.Height < SequencerTimeline::kRulerHeight || y > h)
            continue;
        const QRect rowRect(0, y, w, row.Height);

        QColor fill = row.RowKind == SequencerRow::Kind::Folder ? SequencerColors::Header()
                    : row.RowKind == SequencerRow::Kind::Binding ? SequencerColors::Panel().lighter(112)
                    : (index % 2 == 0 ? SequencerColors::RowAlternate() : SequencerColors::Panel());
        painter.fillRect(rowRect, fill);
        if (mModel.SelectedRows.count(row.Key) != 0)
        {
            QColor highlight = SequencerColors::Accent();
            highlight.setAlpha(70);
            painter.fillRect(rowRect, highlight);
        }
        painter.setPen(SequencerColors::Border());
        painter.drawLine(0, y + row.Height - 1, w, y + row.Height - 1);

        const int arrowX = 4 + row.Depth * kIndent;
        if (row.Expandable)
            DrawTriangle(painter, QPointF(arrowX + 6, y + row.Height * 0.5), 3.5, true, row.Expanded,
                         SequencerColors::TextDim());

        if (row.Accent.isValid())
        {
            painter.fillRect(QRectF(arrowX + 2, y + 5, 3, row.Height - 10), row.Accent);
        }

        // Label, with room kept for the row's buttons on the right.
        const bool keyed = row.RowKind == SequencerRow::Kind::Track || row.RowKind == SequencerRow::Kind::Group ||
                           row.RowKind == SequencerRow::Kind::Channel;
        const int buttonsWidth = keyed ? 3 * kButtonWidth : (row.RowKind == SequencerRow::Kind::Folder ? 0 : kButtonWidth);
        const int labelX = arrowX + 16;

        bool missing = false;
        if (row.RowKind == SequencerRow::Kind::Binding && document != nullptr && mModel.Actions.IsBindingResolved)
        {
            if (const SequenceBinding* binding = document->Sequence().FindBinding(row.Binding))
                missing = !mModel.Actions.IsBindingResolved(*binding);
        }

        QFont font = painter.font();
        font.setBold(row.RowKind == SequencerRow::Kind::Folder || row.RowKind == SequencerRow::Kind::Binding ||
                     row.RowKind == SequencerRow::Kind::CameraCuts || row.RowKind == SequencerRow::Kind::Events);
        painter.setFont(font);
        QColor textColor = row.Muted ? SequencerColors::TextDim() : SequencerColors::Text();
        if (missing)
            textColor = QColor(214, 110, 96);
        painter.setPen(textColor);

        // Current value on the right of property rows: a swatch for a colour, a number for
        // a scalar or a single channel.
        QString valueText;
        QColor swatch;
        if (keyed && document != nullptr)
        {
            if (auto* property = dynamic_cast<PropertyTrack*>(document->FindTrack(row.Track)))
            {
                const PropertyType type = property->ValueType();
                if (row.RowKind == SequencerRow::Kind::Channel && property->Channel(row.ChannelFirst).HasKeys())
                {
                    valueText = QString::number(property->Channel(row.ChannelFirst).Evaluate(time), 'g', 5);
                }
                else if (row.RowKind == SequencerRow::Kind::Track &&
                         (type == PropertyType::Color || type == PropertyType::ColorRGB))
                {
                    PropertyValue value;
                    value.Components[3] = 1.0;
                    property->EvaluateInto(time, value);
                    swatch = QColor::fromRgbF(float(std::clamp(value.Components[0], 0.0, 1.0)),
                                              float(std::clamp(value.Components[1], 0.0, 1.0)),
                                              float(std::clamp(value.Components[2], 0.0, 1.0)));
                }
                else if (row.RowKind == SequencerRow::Kind::Track && property->ChannelCount() == 1 &&
                         property->Channel(0).HasKeys())
                {
                    const double value = property->Channel(0).Evaluate(time);
                    valueText = type == PropertyType::Bool ? (value >= 0.5 ? QStringLiteral("On") : QStringLiteral("Off"))
                                                           : QString::number(value, 'g', 5);
                }
            }
        }

        const int valueWidth = valueText.isEmpty() ? (swatch.isValid() ? 22 : 0) : metrics.horizontalAdvance(valueText) + 8;
        const int labelRight = w - 4 - buttonsWidth - valueWidth;
        QString label = row.Label;
        if (missing)
            label += QStringLiteral("  (not in level)");
        painter.drawText(QRect(labelX, y, (std::max)(0, labelRight - labelX), row.Height), Qt::AlignVCenter | Qt::AlignLeft,
                         metrics.elidedText(label, Qt::ElideRight, (std::max)(0, labelRight - labelX)));
        font.setBold(false);
        painter.setFont(font);

        if (!valueText.isEmpty())
        {
            painter.setPen(SequencerColors::TextDim());
            painter.drawText(QRect(labelRight, y, valueWidth - 4, row.Height), Qt::AlignVCenter | Qt::AlignRight, valueText);
        }
        else if (swatch.isValid())
        {
            painter.setPen(SequencerColors::Border());
            painter.setBrush(swatch);
            painter.drawRect(QRectF(labelRight + 4, y + 5, 14, row.Height - 10));
        }

        // Buttons.
        const int right = w - 4;
        const double midY = y + row.Height * 0.5;
        const QColor glyph = SequencerColors::TextDim();
        if (keyed)
        {
            DrawTriangle(painter, QPointF(right - 2.5 * kButtonWidth, midY), 3.5, false, false, glyph);
            DrawDiamond(painter, QPointF(right - 1.5 * kButtonWidth, midY), 4.5, QColor(0, 0, 0, 0), glyph);
            DrawTriangle(painter, QPointF(right - 0.5 * kButtonWidth, midY), 3.5, true, false, glyph);
        }
        else if (row.RowKind != SequencerRow::Kind::Folder)
        {
            DrawPlus(painter, QPointF(right - 0.5 * kButtonWidth, midY), 4.0, glyph);
        }
    }
    painter.restore();

    // Header strip, level with the timeline's ruler.
    painter.fillRect(QRect(0, 0, w, SequencerTimeline::kRulerHeight), SequencerColors::Header());
    painter.setPen(SequencerColors::Border());
    painter.drawLine(0, SequencerTimeline::kRulerHeight - 1, w, SequencerTimeline::kRulerHeight - 1);
    painter.drawLine(w - 1, 0, w - 1, h);
    painter.setPen(SequencerColors::TextDim());
    painter.drawText(QRect(8, 0, w - 16, SequencerTimeline::kRulerHeight), Qt::AlignVCenter | Qt::AlignLeft,
                     document != nullptr && document->Sequence().Bindings.empty() && document->Sequence().MasterTracks.empty()
                         ? QStringLiteral("Add an entity to start animating")
                         : QStringLiteral("Tracks"));
}

void SequencerTrackTree::JumpToKey(const SequencerRow& row, bool forward)
{
    SequencerDocument* document = mModel.Document;
    if (document == nullptr)
        return;
    const double now = document->Time();
    double best = forward ? 1e30 : -1e30;
    bool found = false;
    for (const auto& [track, mask] : mModel.TracksOfRow(row))
    {
        for (int channel = 0; channel < track->ChannelCount(); ++channel)
        {
            if ((mask & (1u << channel)) == 0)
                continue;
            for (const Keyframe& key : track->Channel(channel).Keys())
            {
                if (forward && key.Time > now + 1e-6 && key.Time < best)
                {
                    best = key.Time;
                    found = true;
                }
                if (!forward && key.Time < now - 1e-6 && key.Time > best)
                {
                    best = key.Time;
                    found = true;
                }
            }
        }
    }
    if (found)
    {
        document->Player().Pause();
        document->SetTime(best);
        if (mModel.Actions.Changed)
            mModel.Actions.Changed();
    }
}

void SequencerTrackTree::mousePressEvent(QMouseEvent* event)
{
    const QPoint position = event->position().toPoint();
    if (event->button() != Qt::LeftButton || position.y() < SequencerTimeline::kRulerHeight || mModel.Document == nullptr)
        return;

    const SequencerRow* row = mModel.RowAtContentY(position.y() - SequencerTimeline::kRulerHeight + mModel.ScrollY);
    if (row == nullptr)
    {
        mModel.SelectedRows.clear();
        if (mModel.Actions.Changed)
            mModel.Actions.Changed();
        return;
    }
    const SequencerRow current = *row; // the row list may be rebuilt by what follows
    SequencerDocument& document = *mModel.Document;

    switch (HitButton(current, position))
    {
    case Button::Expand:
        mModel.SetExpanded(current.Key, !current.Expanded);
        break;
    case Button::PreviousKey:
        JumpToKey(current, false);
        return;
    case Button::NextKey:
        JumpToKey(current, true);
        return;
    case Button::AddKey:
        document.AddKey(current.Track, document.Time(), ChannelMask(current));
        break;
    case Button::AddTrack:
        if (mModel.Actions.ShowAddTrackMenu)
            mModel.Actions.ShowAddTrackMenu(current.Binding, mapToGlobal(position));
        return;
    case Button::AddCut:
    {
        QMenu menu(this);
        if (mModel.Actions.FillAddCutMenu)
            mModel.Actions.FillAddCutMenu(menu, document.Time());
        menu.exec(mapToGlobal(position));
        break;
    }
    case Button::AddEvent:
        if (mModel.Actions.AddEventAt)
            mModel.Actions.AddEventAt(document.Time());
        break;
    case Button::None:
        if (current.RowKind != SequencerRow::Kind::Folder)
        {
            const bool additive = (event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) != 0;
            if (!additive)
                mModel.SelectedRows.clear();
            if (additive && mModel.SelectedRows.count(current.Key) != 0 && (event->modifiers() & Qt::ControlModifier))
                mModel.SelectedRows.erase(current.Key);
            else
                mModel.SelectedRows.insert(current.Key);

            if (current.RowKind == SequencerRow::Kind::Binding && mModel.Actions.SelectBindingEntity)
                mModel.Actions.SelectBindingEntity(current.Binding);
        }
        else
        {
            mModel.SetExpanded(current.Key, !current.Expanded);
        }
        break;
    }

    if (mModel.Actions.Changed)
        mModel.Actions.Changed();
}

void SequencerTrackTree::mouseDoubleClickEvent(QMouseEvent* event)
{
    const QPoint position = event->position().toPoint();
    const SequencerRow* row = mModel.RowAtContentY(position.y() - SequencerTimeline::kRulerHeight + mModel.ScrollY);
    if (row == nullptr || mModel.Document == nullptr || HitButton(*row, position) != Button::None)
        return;
    const SequencerRow current = *row;

    if (current.RowKind == SequencerRow::Kind::Binding)
    {
        const SequenceBinding* binding = mModel.Document->Sequence().FindBinding(current.Binding);
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("Rename Binding"), QStringLiteral("Name:"),
                                                   QLineEdit::Normal, current.Label, &ok);
        if (ok && binding != nullptr && !name.trimmed().isEmpty())
            mModel.Document->RenameBinding(current.Binding, name.trimmed().toStdString());
    }
    else if (current.Expandable)
    {
        mModel.SetExpanded(current.Key, !current.Expanded);
    }
    if (mModel.Actions.Changed)
        mModel.Actions.Changed();
}

void SequencerTrackTree::wheelEvent(QWheelEvent* event)
{
    const int visible = height() - SequencerTimeline::kRulerHeight;
    const int maximum = (std::max)(0, mModel.ContentHeight() - visible);
    mModel.ScrollY = std::clamp(mModel.ScrollY - event->angleDelta().y() / 3, 0, maximum);
    if (mModel.Actions.Changed)
        mModel.Actions.Changed();
}

void SequencerTrackTree::contextMenuEvent(QContextMenuEvent* event)
{
    SequencerDocument* document = mModel.Document;
    if (document == nullptr)
        return;
    const SequencerRow* row = mModel.RowAtContentY(event->pos().y() - SequencerTimeline::kRulerHeight + mModel.ScrollY);
    if (row == nullptr)
        return;
    const SequencerRow current = *row;
    QMenu menu(this);

    switch (current.RowKind)
    {
    case SequencerRow::Kind::Binding:
    {
        menu.addAction(QStringLiteral("Add Track..."), [this, current, event] {
            if (mModel.Actions.ShowAddTrackMenu)
                mModel.Actions.ShowAddTrackMenu(current.Binding, event->globalPos());
        });
        menu.addAction(QStringLiteral("Rename..."), [this, current] {
            bool ok = false;
            const QString name = QInputDialog::getText(this, QStringLiteral("Rename Binding"), QStringLiteral("Name:"),
                                                       QLineEdit::Normal, current.Label, &ok);
            if (ok && !name.trimmed().isEmpty())
                mModel.Document->RenameBinding(current.Binding, name.trimmed().toStdString());
        });
        const SequenceBinding* binding = document->Sequence().FindBinding(current.Binding);
        if (binding != nullptr && binding->Domain == "Entity")
        {
            menu.addAction(QStringLiteral("Select in Level"), [this, current] {
                if (mModel.Actions.SelectBindingEntity)
                    mModel.Actions.SelectBindingEntity(current.Binding);
            });
            menu.addAction(QStringLiteral("Rebind to Selected Entity"), [this, current] {
                if (mModel.Actions.RebindToSelection)
                    mModel.Actions.RebindToSelection(current.Binding);
            });
            if (mModel.Actions.IsCameraBinding && mModel.Actions.IsCameraBinding(current.Binding))
            {
                menu.addAction(QStringLiteral("Pilot Camera"), [this, current] {
                    if (mModel.Actions.PilotBinding)
                        mModel.Actions.PilotBinding(current.Binding);
                });
            }
        }
        menu.addSeparator();
        menu.addAction(QStringLiteral("Delete Binding"), [this, current] { mModel.Document->RemoveBinding(current.Binding); });
        break;
    }
    case SequencerRow::Kind::Track:
    case SequencerRow::Kind::Group:
    case SequencerRow::Kind::Channel:
    {
        menu.addAction(QStringLiteral("Add Key at Playhead"), [this, current] {
            mModel.Document->AddKey(current.Track, mModel.Document->Time(), ChannelMask(current));
        });
        if (current.RowKind == SequencerRow::Kind::Track)
        {
            menu.addAction(current.Muted ? QStringLiteral("Unmute Track") : QStringLiteral("Mute Track"),
                           [this, current] { mModel.Document->SetTrackMuted(current.Track, !current.Muted); });
            menu.addSeparator();
            menu.addAction(QStringLiteral("Delete Track"), [this, current] { mModel.Document->RemoveTrack(current.Track); });
        }
        break;
    }
    case SequencerRow::Kind::CameraCuts:
    {
        QMenu* add = menu.addMenu(QStringLiteral("Add Camera Cut at Playhead"));
        if (mModel.Actions.FillAddCutMenu)
            mModel.Actions.FillAddCutMenu(*add, document->Time());
        menu.addAction(current.Muted ? QStringLiteral("Unmute Camera Cuts") : QStringLiteral("Mute Camera Cuts"),
                       [this, current] { mModel.Document->SetTrackMuted(current.Track, !current.Muted); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Delete Camera Cut Track"), [this, current] { mModel.Document->RemoveTrack(current.Track); });
        break;
    }
    case SequencerRow::Kind::Events:
    {
        menu.addAction(QStringLiteral("Add Event at Playhead..."), [this] {
            if (mModel.Actions.AddEventAt)
                mModel.Actions.AddEventAt(mModel.Document->Time());
        });
        menu.addAction(current.Muted ? QStringLiteral("Unmute Events") : QStringLiteral("Mute Events"),
                       [this, current] { mModel.Document->SetTrackMuted(current.Track, !current.Muted); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Delete Event Track"), [this, current] { mModel.Document->RemoveTrack(current.Track); });
        break;
    }
    default:
        return;
    }

    menu.exec(event->globalPos());
    if (mModel.Actions.Changed)
        mModel.Actions.Changed();
}

// ---------------------------------------------------------------------------------------
// SequencerTimeline
// ---------------------------------------------------------------------------------------

SequencerTimeline::SequencerTimeline(SequencerTimelineModel& model, QWidget* parent)
    : QWidget(parent), mModel(model)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumWidth(240);
}

const SequencerRow* SequencerTimeline::RowAt(int y)
{
    if (y < kRulerHeight)
        return nullptr;
    return mModel.RowAtContentY(y - kRulerHeight + mModel.ScrollY);
}

void SequencerTimeline::FrameAll()
{
    SequencerDocument* document = mModel.Document;
    if (document == nullptr)
        return;
    const CinematicSequence& sequence = document->Sequence();
    double first = sequence.PlaybackStart;
    double last = sequence.PlaybackEnd;
    double keyFirst = 0.0;
    double keyLast = 0.0;
    if (sequence.GetKeyTimeRange(keyFirst, keyLast))
    {
        first = (std::min)(first, keyFirst);
        last = (std::max)(last, keyLast);
    }
    const double margin = (std::max)(0.1, (last - first) * 0.05);
    mModel.ViewStart = first - margin;
    mModel.ViewEnd = last + margin;
    document->SetWorkRange(mModel.ViewStart, mModel.ViewEnd);
    update();
}

void SequencerTimeline::FrameSelection()
{
    SequencerDocument* document = mModel.Document;
    if (document == nullptr)
        return;
    double first = 1e30;
    double last = -1e30;
    for (const SequencerKeyHandle& handle : document->SelectedKeys())
    {
        double time = 0.0;
        if (document->KeyTime(handle, time))
        {
            first = (std::min)(first, time);
            last = (std::max)(last, time);
        }
    }
    if (first > last && mModel.HasSelectionRange())
    {
        first = mModel.SelectionStart;
        last = mModel.SelectionEnd;
    }
    if (first > last)
    {
        FrameAll();
        return;
    }
    const double margin = (std::max)(0.25, (last - first) * 0.1);
    mModel.ViewStart = first - margin;
    mModel.ViewEnd = last + margin;
    update();
}

void SequencerTimeline::PaintRuler(QPainter& painter)
{
    SequencerDocument* document = mModel.Document;
    const int w = width();
    painter.fillRect(QRect(0, 0, w, kRulerHeight), SequencerColors::Header());

    if (document != nullptr)
    {
        const CinematicSequence& sequence = document->Sequence();
        const FrameRate& rate = sequence.DisplayRate;
        const double pixelsPerSecond = w / (mModel.ViewEnd - mModel.ViewStart);
        const double pixelsPerFrame = pixelsPerSecond / rate.FramesPerSecond();
        const double fps = rate.FramesPerSecond();

        // Labelled ticks every `major` frames, chosen to sit at least ~80 px apart, at
        // steps that read naturally: single frames when zoomed in, then half seconds,
        // seconds, ten seconds, minutes.
        const double candidates[] = { 1, 2, 5, 10, fps / 2, fps, fps * 2, fps * 5, fps * 10, fps * 15, fps * 30,
                                      fps * 60, fps * 120, fps * 300, fps * 600, fps * 1800, fps * 3600 };
        double major = candidates[std::size(candidates) - 1];
        for (double candidate : candidates)
        {
            if (candidate >= 1.0 && candidate * pixelsPerFrame >= 80.0)
            {
                major = std::round(candidate);
                break;
            }
        }
        double minor = major / 5.0 >= 1.0 ? major / 5.0 : 1.0;
        if (minor * pixelsPerFrame < 6.0)
            minor = major;

        // The playback range, tinted on the ruler.
        const double startX = mModel.TimeToX(sequence.PlaybackStart, w);
        const double endX = mModel.TimeToX(sequence.PlaybackEnd, w);
        QColor range = SequencerColors::Accent();
        range.setAlpha(40);
        painter.fillRect(QRectF(startX, 0, endX - startX, kRulerHeight), range);

        if (mModel.HasSelectionRange())
        {
            const double x0 = mModel.TimeToX(mModel.SelectionStart, w);
            const double x1 = mModel.TimeToX(mModel.SelectionEnd, w);
            QColor band(230, 190, 90, 90);
            painter.fillRect(QRectF(x0, kRulerHeight - 7, x1 - x0, 6), band);
        }

        const double firstFrame = std::floor(mModel.ViewStart * fps / minor) * minor;
        const double lastFrame = mModel.ViewEnd * fps;
        painter.setPen(SequencerColors::TextDim());
        for (double frame = firstFrame; frame <= lastFrame; frame += minor)
        {
            const double x = mModel.TimeToX(frame / fps, w);
            const bool isMajor = std::fmod(std::fabs(std::round(frame)), major) < 0.5;
            painter.setPen(isMajor ? SequencerColors::TextDim() : SequencerColors::Border());
            painter.drawLine(QPointF(x, isMajor ? kRulerHeight - 10 : kRulerHeight - 5), QPointF(x, kRulerHeight - 1));
            if (isMajor)
            {
                painter.setPen(SequencerColors::TextDim());
                painter.drawText(QPointF(x + 3, 12),
                                 FormatSequencerTime(frame / fps, rate, mModel.TimeDisplay));
            }
        }

        // Playback range handles: green opens, red closes.
        auto handle = [&](double x, const QColor& color, bool start) {
            painter.setPen(QPen(color, 1.5));
            painter.drawLine(QPointF(x, 0), QPointF(x, kRulerHeight));
            const QPointF points[] = { { x, kRulerHeight - 8.0 }, { x + (start ? 7.0 : -7.0), kRulerHeight - 4.0 },
                                       { x, kRulerHeight - 0.0 } };
            painter.setBrush(color);
            painter.setPen(Qt::NoPen);
            painter.drawPolygon(points, 3);
        };
        handle(startX, QColor(96, 190, 110), true);
        handle(endX, QColor(214, 92, 84), false);
    }

    painter.setPen(SequencerColors::Border());
    painter.drawLine(0, kRulerHeight - 1, w, kRulerHeight - 1);
}

void SequencerTimeline::PaintCuts(QPainter& painter, const SequencerRow& row, int y)
{
    SequencerDocument* document = mModel.Document;
    const auto* cuts = dynamic_cast<const CameraCutTrack*>(document->FindTrack(row.Track));
    if (cuts == nullptr)
        return;
    const int w = width();
    const QFontMetrics metrics = fontMetrics();
    for (const CameraCutSection& section : cuts->Sections)
    {
        const double x0 = mModel.TimeToX(section.Start, w);
        const double x1 = mModel.TimeToX(section.End, w);
        if (x1 < 0 || x0 > w)
            continue;
        const QRectF block(x0 + 0.5, y + 3.5, (std::max)(2.0, x1 - x0 - 1.0), row.Height - 7.0);
        const bool selected = document->SelectedCut == section.Id;
        QColor fill = selected ? SequencerColors::Accent() : QColor(62, 84, 112);
        if (row.Muted)
            fill.setAlpha(110);
        painter.setPen(QPen(selected ? SequencerColors::Text() : QColor(96, 122, 156), 1.0));
        painter.setBrush(fill);
        painter.drawRoundedRect(block, 2.0, 2.0);

        const QString name = mModel.CameraName(section.CameraBinding);
        painter.setPen(QColor(235, 238, 242));
        const QRectF textRect = block.adjusted(6, 0, -6, 0);
        painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft,
                         metrics.elidedText(name, Qt::ElideRight, int(textRect.width())));
    }
}

void SequencerTimeline::PaintRows(QPainter& painter)
{
    SequencerDocument* document = mModel.Document;
    const int w = width();
    const int h = height();
    const double t0 = mModel.XToTime(-8, w);
    const double t1 = mModel.XToTime(w + 8, w);
    const int offset = kRulerHeight - mModel.ScrollY;
    const QFontMetrics metrics = fontMetrics();

    std::vector<SequencerTimelineModel::RowKey> keys;
    int index = 0;
    for (const SequencerRow& row : mModel.Rows())
    {
        ++index;
        const int y = row.Top + offset;
        if (y + row.Height < kRulerHeight || y > h)
            continue;
        const QRect rowRect(0, y, w, row.Height);
        QColor fill = row.RowKind == SequencerRow::Kind::Folder ? SequencerColors::Header()
                    : row.RowKind == SequencerRow::Kind::Binding ? SequencerColors::Background().lighter(115)
                    : (index % 2 == 0 ? SequencerColors::RowAlternate() : SequencerColors::Background());
        painter.fillRect(rowRect, fill);
        if (mModel.SelectedRows.count(row.Key) != 0)
        {
            QColor highlight = SequencerColors::Accent();
            highlight.setAlpha(30);
            painter.fillRect(rowRect, highlight);
        }
        painter.setPen(SequencerColors::Border());
        painter.drawLine(0, y + row.Height - 1, w, y + row.Height - 1);

        if (row.RowKind == SequencerRow::Kind::CameraCuts)
        {
            PaintCuts(painter, row, y);
            continue;
        }

        mModel.CollectRowKeys(row, t0, t1, keys);
        const double midY = y + row.Height * 0.5;
        const bool summary = row.RowKind == SequencerRow::Kind::Binding || row.RowKind == SequencerRow::Kind::Group ||
                             (row.RowKind == SequencerRow::Kind::Track && row.ChannelCount > 1);

        if (row.RowKind == SequencerRow::Kind::Events)
        {
            for (const SequencerTimelineModel::RowKey& key : keys)
            {
                const double x = mModel.TimeToX(key.Time, w);
                const QColor color = key.Selected ? SequencerColors::Accent() : QColor(222, 186, 92);
                painter.setPen(QPen(color, 1.5));
                painter.drawLine(QPointF(x, y + 3), QPointF(x, y + row.Height - 3));
                const QPointF flag[] = { { x, y + 3.0 }, { x + 8.0, y + 6.5 }, { x, y + 10.0 } };
                painter.setPen(Qt::NoPen);
                painter.setBrush(color);
                painter.drawPolygon(flag, 3);
                painter.setPen(row.Muted ? SequencerColors::TextDim() : SequencerColors::Text());
                painter.drawText(QPointF(x + 11, midY + metrics.ascent() * 0.5 - 1), key.Name);
            }
            continue;
        }

        for (const SequencerTimelineModel::RowKey& key : keys)
        {
            const double x = mModel.TimeToX(key.Time, w);
            QColor color = key.Selected ? SequencerColors::Accent().lighter(130) : InterpolationColor(key.Interpolation);
            if (row.Muted)
                color.setAlpha(110);
            const QColor outline = key.Selected ? QColor(255, 255, 255) : QColor(20, 20, 22);
            const double radius = summary ? 4.0 : 5.0;
            if (!summary && key.Interpolation == KeyInterpolation::Constant)
            {
                painter.setPen(QPen(outline, 1.0));
                painter.setBrush(color);
                painter.drawRect(QRectF(x - radius + 1, midY - radius + 1, 2 * radius - 2, 2 * radius - 2));
            }
            else
            {
                DrawDiamond(painter, QPointF(x, midY), radius, color, outline);
            }
        }
    }

    // Outside the playback range is dimmed, so the part that plays reads at a glance.
    const CinematicSequence& sequence = document->Sequence();
    const double startX = mModel.TimeToX(sequence.PlaybackStart, w);
    const double endX = mModel.TimeToX(sequence.PlaybackEnd, w);
    const QColor shade(0, 0, 0, 60);
    if (startX > 0)
        painter.fillRect(QRectF(0, kRulerHeight, startX, h - kRulerHeight), shade);
    if (endX < w)
        painter.fillRect(QRectF(endX, kRulerHeight, w - endX, h - kRulerHeight), shade);
    painter.setPen(QPen(QColor(96, 190, 110, 140), 1.0));
    painter.drawLine(QPointF(startX, kRulerHeight), QPointF(startX, h));
    painter.setPen(QPen(QColor(214, 92, 84, 140), 1.0));
    painter.drawLine(QPointF(endX, kRulerHeight), QPointF(endX, h));

    if (mModel.HasSelectionRange())
    {
        const double x0 = mModel.TimeToX(mModel.SelectionStart, w);
        const double x1 = mModel.TimeToX(mModel.SelectionEnd, w);
        painter.fillRect(QRectF(x0, kRulerHeight, x1 - x0, h - kRulerHeight), QColor(230, 190, 90, 18));
    }
}

void SequencerTimeline::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.fillRect(rect(), SequencerColors::Background());
    SequencerDocument* document = mModel.Document;
    if (document == nullptr)
        return;
    painter.setRenderHint(QPainter::Antialiasing, true);

    painter.save();
    painter.setClipRect(0, kRulerHeight, width(), height() - kRulerHeight);
    PaintRows(painter);
    if (mDrag == Drag::Box && mDragStarted)
    {
        QColor fill = SequencerColors::Accent();
        fill.setAlpha(40);
        painter.setPen(QPen(SequencerColors::Accent(), 1.0));
        painter.setBrush(fill);
        painter.drawRect(mBox.normalized());
    }
    painter.restore();

    PaintRuler(painter);

    // Playhead over everything.
    const double x = mModel.TimeToX(document->Time(), width());
    painter.setPen(QPen(SequencerColors::Playhead(), 1.5));
    painter.drawLine(QPointF(x, 0), QPointF(x, height()));
    const QPointF head[] = { { x - 6.0, 0.0 }, { x + 6.0, 0.0 }, { x + 6.0, 9.0 }, { x, 15.0 }, { x - 6.0, 9.0 } };
    painter.setPen(Qt::NoPen);
    painter.setBrush(SequencerColors::Playhead());
    painter.drawPolygon(head, 5);
}

bool SequencerTimeline::HitKey(const SequencerRow& row, const QPoint& position, SequencerTimelineModel::RowKey& hit)
{
    const int w = width();
    const double t0 = mModel.XToTime(position.x() - 7, w);
    const double t1 = mModel.XToTime(position.x() + 7, w);
    std::vector<SequencerTimelineModel::RowKey> keys;
    mModel.CollectRowKeys(row, t0, t1, keys);
    double best = 1e9;
    for (SequencerTimelineModel::RowKey& key : keys)
    {
        const double distance = std::fabs(mModel.TimeToX(key.Time, w) - position.x());
        if (distance < best)
        {
            best = distance;
            hit = key;
        }
    }
    return best <= 7.0;
}

bool SequencerTimeline::HitCut(const QPoint& position, Guid& cut, int& part)
{
    const SequencerRow* row = RowAt(position.y());
    if (row == nullptr || row->RowKind != SequencerRow::Kind::CameraCuts || mModel.Document == nullptr)
        return false;
    const auto* cuts = dynamic_cast<const CameraCutTrack*>(mModel.Document->FindTrack(row->Track));
    if (cuts == nullptr)
        return false;

    const int w = width();
    // Edges first, over every cut: two cuts share a boundary, and that line is what the
    // user means to grab.
    for (const CameraCutSection& section : cuts->Sections)
    {
        if (std::fabs(mModel.TimeToX(section.End, w) - position.x()) <= 4.0)
        {
            cut = section.Id;
            part = 1;
            return true;
        }
    }
    for (const CameraCutSection& section : cuts->Sections)
    {
        if (std::fabs(mModel.TimeToX(section.Start, w) - position.x()) <= 4.0)
        {
            cut = section.Id;
            part = 0;
            return true;
        }
    }
    const double time = mModel.XToTime(position.x(), w);
    if (const CameraCutSection* section = cuts->FindSectionAt(time))
    {
        cut = section->Id;
        part = 2;
        return true;
    }
    return false;
}

void SequencerTimeline::Scrub(int x)
{
    SequencerDocument* document = mModel.Document;
    document->Player().Pause();
    document->SetTime(document->Snap(mModel.XToTime(x, width())));
    if (mModel.Actions.Changed)
        mModel.Actions.Changed();
}

void SequencerTimeline::SelectInBox()
{
    SequencerDocument* document = mModel.Document;
    std::set<SequencerKeyHandle> selection = mBoxBase;
    const QRect box = mBox.normalized();
    const int w = width();
    const double t0 = mModel.XToTime(box.left(), w);
    const double t1 = mModel.XToTime(box.right(), w);
    std::vector<SequencerTimelineModel::RowKey> keys;
    const int offset = kRulerHeight - mModel.ScrollY;
    for (const SequencerRow& row : mModel.Rows())
    {
        const int y = row.Top + offset;
        const int mid = y + row.Height / 2;
        if (mid < box.top() || mid > box.bottom())
            continue;
        mModel.CollectRowKeys(row, t0, t1, keys);
        for (const SequencerTimelineModel::RowKey& key : keys)
            selection.insert(key.Handles.begin(), key.Handles.end());
    }
    document->SelectedKeys() = std::move(selection);
}

void SequencerTimeline::mousePressEvent(QMouseEvent* event)
{
    SequencerDocument* document = mModel.Document;
    if (document == nullptr || document->IsPreviewSuspended())
        return;
    setFocus();

    const QPoint position = event->position().toPoint();
    mPressPosition = position;
    mPressTime = mModel.XToTime(position.x(), width());
    mDragStarted = false;

    if (event->button() == Qt::MiddleButton)
    {
        mDrag = Drag::Pan;
        mPressViewStart = mModel.ViewStart;
        mPressViewEnd = mModel.ViewEnd;
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;

    const bool control = (event->modifiers() & Qt::ControlModifier) != 0;
    const bool shift = (event->modifiers() & Qt::ShiftModifier) != 0;
    const CinematicSequence& sequence = document->Sequence();

    if (position.y() < kRulerHeight)
    {
        const int w = width();
        if (std::fabs(mModel.TimeToX(sequence.PlaybackStart, w) - position.x()) <= 5.0 && position.y() > kRulerHeight / 2)
        {
            mDrag = Drag::PlaybackStart;
            document->Begin("Set Playback Range", SequencerDocument::Scope_Header);
            return;
        }
        if (std::fabs(mModel.TimeToX(sequence.PlaybackEnd, w) - position.x()) <= 5.0 && position.y() > kRulerHeight / 2)
        {
            mDrag = Drag::PlaybackEnd;
            document->Begin("Set Playback Range", SequencerDocument::Scope_Header);
            return;
        }
        if (control)
        {
            mDrag = Drag::SelectionRange;
            mModel.SelectionStart = mModel.SelectionEnd = document->Snap(mPressTime);
            return;
        }
        mDrag = Drag::Scrub;
        Scrub(position.x());
        return;
    }

    Guid cut;
    int part = 0;
    if (HitCut(position, cut, part))
    {
        document->ClearSelection();
        document->SelectedCut = cut;
        document->BeginEditCut(cut);
        mDrag = part == 0 ? Drag::CutStart : part == 1 ? Drag::CutEnd : Drag::CutMove;
        mDragCut = cut;
        if (mModel.Actions.Changed)
            mModel.Actions.Changed();
        return;
    }

    const SequencerRow* row = RowAt(position.y());
    SequencerTimelineModel::RowKey key;
    if (row != nullptr && HitKey(*row, position, key))
    {
        std::set<SequencerKeyHandle>& selection = document->SelectedKeys();
        const bool allSelected = std::all_of(key.Handles.begin(), key.Handles.end(),
                                             [&](const SequencerKeyHandle& handle) { return selection.count(handle) != 0; });
        document->SelectedCut = Guid{};
        if (control)
        {
            for (const SequencerKeyHandle& handle : key.Handles)
            {
                if (allSelected)
                    selection.erase(handle);
                else
                    selection.insert(handle);
            }
            mDrag = Drag::None;
        }
        else
        {
            if (!allSelected && !shift)
                selection.clear();
            selection.insert(key.Handles.begin(), key.Handles.end());
            mDrag = Drag::Keys;
            mDragAnchor = key.Handles.front();
        }
        if (mModel.Actions.Changed)
            mModel.Actions.Changed();
        return;
    }

    // Empty space: a box selection, added to the current one with Shift or Ctrl.
    mBoxBase = (control || shift) ? document->SelectedKeys() : std::set<SequencerKeyHandle>{};
    if (!control && !shift)
        document->ClearSelection();
    mDrag = Drag::Box;
    mBox = QRect(position, position);
    if (mModel.Actions.Changed)
        mModel.Actions.Changed();
}

void SequencerTimeline::mouseMoveEvent(QMouseEvent* event)
{
    SequencerDocument* document = mModel.Document;
    if (document == nullptr)
        return;
    const QPoint position = event->position().toPoint();
    const int w = width();
    const double time = mModel.XToTime(position.x(), w);

    if (mDrag == Drag::None)
    {
        // Cursor hints for what a press would grab.
        Guid cut;
        int part = 0;
        const CinematicSequence& sequence = document->Sequence();
        const bool overRange = position.y() < kRulerHeight && position.y() > kRulerHeight / 2 &&
            (std::fabs(mModel.TimeToX(sequence.PlaybackStart, w) - position.x()) <= 5.0 ||
             std::fabs(mModel.TimeToX(sequence.PlaybackEnd, w) - position.x()) <= 5.0);
        const bool overEdge = HitCut(position, cut, part) && part != 2;
        setCursor(overRange || overEdge ? Qt::SizeHorCursor : Qt::ArrowCursor);
        return;
    }

    switch (mDrag)
    {
    case Drag::Scrub:
        Scrub(position.x());
        return;
    case Drag::Pan:
    {
        const double span = mPressViewEnd - mPressViewStart;
        const double shiftSeconds = (mPressPosition.x() - position.x()) / double((std::max)(1, w)) * span;
        mModel.ViewStart = mPressViewStart + shiftSeconds;
        mModel.ViewEnd = mPressViewEnd + shiftSeconds;
        const int visible = height() - kRulerHeight;
        const int maximum = (std::max)(0, mModel.ContentHeight() - visible);
        mModel.ScrollY = std::clamp(mModel.ScrollY - (position.y() - mPressPosition.y()), 0, maximum);
        mPressPosition.setY(position.y());
        break;
    }
    case Drag::PlaybackStart:
    {
        CinematicSequence& sequence = document->Sequence();
        sequence.PlaybackStart = (std::min)(document->Snap(time), sequence.PlaybackEnd - sequence.DisplayRate.FrameDuration());
        document->MarkChanged(false);
        break;
    }
    case Drag::PlaybackEnd:
    {
        CinematicSequence& sequence = document->Sequence();
        sequence.PlaybackEnd = (std::max)(document->Snap(time), sequence.PlaybackStart + sequence.DisplayRate.FrameDuration());
        document->MarkChanged(false);
        break;
    }
    case Drag::SelectionRange:
    {
        const double snapped = document->Snap(time);
        const double anchor = document->Snap(mPressTime);
        mModel.SelectionStart = (std::min)(anchor, snapped);
        mModel.SelectionEnd = (std::max)(anchor, snapped);
        break;
    }
    case Drag::Keys:
        if (!mDragStarted && std::abs(position.x() - mPressPosition.x()) >= 3)
        {
            mDragStarted = true;
            document->BeginMoveKeys(mDragAnchor);
        }
        if (mDragStarted)
            document->UpdateMoveKeys(time - mPressTime);
        break;
    case Drag::Box:
        mDragStarted = mDragStarted || (position - mPressPosition).manhattanLength() >= 3;
        mBox.setBottomRight(position);
        if (mDragStarted)
            SelectInBox();
        break;
    case Drag::CutStart:
        document->UpdateCutEdge(0, time);
        break;
    case Drag::CutEnd:
        document->UpdateCutEdge(1, time);
        break;
    case Drag::CutMove:
        document->UpdateCutMove(time - mPressTime);
        break;
    default:
        break;
    }

    if (mModel.Actions.Changed)
        mModel.Actions.Changed();
}

void SequencerTimeline::mouseReleaseEvent(QMouseEvent*)
{
    SequencerDocument* document = mModel.Document;
    if (document == nullptr)
        return;

    switch (mDrag)
    {
    case Drag::Keys:
        if (mDragStarted)
            document->EndMoveKeys();
        break;
    case Drag::CutStart:
    case Drag::CutEnd:
    case Drag::CutMove:
        document->EndEditCut();
        break;
    case Drag::PlaybackStart:
    case Drag::PlaybackEnd:
        document->Commit();
        document->SetTime(document->Time());
        break;
    case Drag::SelectionRange:
        if (mModel.SelectionEnd - mModel.SelectionStart < 1e-6)
            mModel.SelectionStart = mModel.SelectionEnd = 0.0;
        break;
    case Drag::Pan:
        document->SetWorkRange(mModel.ViewStart, mModel.ViewEnd);
        break;
    default:
        break;
    }
    mDrag = Drag::None;
    mDragStarted = false;
    if (mModel.Actions.Changed)
        mModel.Actions.Changed();
}

void SequencerTimeline::keyPressEvent(QKeyEvent* event)
{
    if (event->key() == Qt::Key_Escape && mDrag != Drag::None && mModel.Document != nullptr)
    {
        if (mDrag == Drag::Keys && mDragStarted)
            mModel.Document->CancelMoveKeys();
        else if (mDrag == Drag::CutStart || mDrag == Drag::CutEnd || mDrag == Drag::CutMove ||
                 mDrag == Drag::PlaybackStart || mDrag == Drag::PlaybackEnd)
        {
            mModel.Document->Cancel();
            mModel.Document->EndEditCut();
        }
        mDrag = Drag::None;
        mDragStarted = false;
        if (mModel.Actions.Changed)
            mModel.Actions.Changed();
        return;
    }
    QWidget::keyPressEvent(event);
}

void SequencerTimeline::mouseDoubleClickEvent(QMouseEvent* event)
{
    SequencerDocument* document = mModel.Document;
    if (document == nullptr || event->button() != Qt::LeftButton)
        return;
    const QPoint position = event->position().toPoint();
    const SequencerRow* row = RowAt(position.y());
    if (row == nullptr)
        return;
    const SequencerRow current = *row;
    const double time = mModel.XToTime(position.x(), width());

    SequencerTimelineModel::RowKey key;
    if (HitKey(current, position, key))
    {
        if (key.Handles.size() == 1 && mModel.Actions.EditKey)
            mModel.Actions.EditKey(key.Handles.front());
        else if (current.Expandable && !current.Expanded)
            mModel.SetExpanded(current.Key, true);
    }
    else if (current.RowKind == SequencerRow::Kind::Events && mModel.Actions.AddEventAt)
    {
        mModel.Actions.AddEventAt(time);
    }
    else if (current.RowKind == SequencerRow::Kind::CameraCuts)
    {
        QMenu menu(this);
        if (mModel.Actions.FillAddCutMenu)
            mModel.Actions.FillAddCutMenu(menu, time);
        menu.exec(event->globalPosition().toPoint());
    }
    mDrag = Drag::None;
    if (mModel.Actions.Changed)
        mModel.Actions.Changed();
}

void SequencerTimeline::wheelEvent(QWheelEvent* event)
{
    // The wheel zooms, as in every timeline: the time under the cursor stays put. Shift
    // pans along time; Ctrl scrolls the tracks (as does the wheel over the track list).
    const int w = width();
    // Some mice and drivers report Shift+wheel as horizontal; either axis will do.
    const int delta = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->angleDelta().x();
    const double notches = delta / 120.0;
    if ((event->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier)) == 0)
    {
        const double pivot = mModel.XToTime(event->position().x(), w);
        const double factor = std::pow(0.85, notches);
        const double span = std::clamp((mModel.ViewEnd - mModel.ViewStart) * factor, kMinimumSpan, kMaximumSpan);
        const double ratio = (pivot - mModel.ViewStart) / (mModel.ViewEnd - mModel.ViewStart);
        mModel.ViewStart = pivot - ratio * span;
        mModel.ViewEnd = mModel.ViewStart + span;
        if (mModel.Document != nullptr)
            mModel.Document->SetWorkRange(mModel.ViewStart, mModel.ViewEnd);
    }
    else if (event->modifiers() & Qt::ShiftModifier)
    {
        const double span = mModel.ViewEnd - mModel.ViewStart;
        const double shiftSeconds = -notches * span * 0.1;
        mModel.ViewStart += shiftSeconds;
        mModel.ViewEnd += shiftSeconds;
    }
    else
    {
        const int visible = height() - kRulerHeight;
        const int maximum = (std::max)(0, mModel.ContentHeight() - visible);
        mModel.ScrollY = std::clamp(mModel.ScrollY - delta / 3, 0, maximum);
    }
    if (mModel.Actions.Changed)
        mModel.Actions.Changed();
}

void SequencerTimeline::contextMenuEvent(QContextMenuEvent* event)
{
    SequencerDocument* document = mModel.Document;
    if (document == nullptr || document->IsPreviewSuspended())
        return;
    const QPoint position = event->pos();
    const double time = document->Snap(mModel.XToTime(position.x(), width()));
    QMenu menu(this);

    if (position.y() < kRulerHeight)
    {
        menu.addAction(QStringLiteral("Set Playback Start Here"), [document, time] {
            document->SetPlaybackRange(time, (std::max)(document->Sequence().PlaybackEnd,
                                                        time + document->Sequence().DisplayRate.FrameDuration()));
        });
        menu.addAction(QStringLiteral("Set Playback End Here"), [document, time] {
            document->SetPlaybackRange((std::min)(document->Sequence().PlaybackStart,
                                                  time - document->Sequence().DisplayRate.FrameDuration()),
                                       time);
        });
        if (mModel.HasSelectionRange())
        {
            menu.addAction(QStringLiteral("Set Playback Range to Selection"), [this, document] {
                document->SetPlaybackRange(mModel.SelectionStart, mModel.SelectionEnd);
            });
            menu.addAction(QStringLiteral("Clear Selection Range"), [this] { mModel.SelectionStart = mModel.SelectionEnd = 0.0; });
        }
        menu.addSeparator();
        menu.addAction(QStringLiteral("Frame All"), [this] { FrameAll(); });
        menu.exec(event->globalPos());
        if (mModel.Actions.Changed)
            mModel.Actions.Changed();
        return;
    }

    Guid cut;
    int part = 0;
    if (HitCut(position, cut, part))
    {
        document->ClearSelection();
        document->SelectedCut = cut;
        QMenu* cameras = menu.addMenu(QStringLiteral("Camera"));
        for (const SequenceBinding& binding : document->Sequence().Bindings)
        {
            if (!mModel.Actions.IsCameraBinding || !mModel.Actions.IsCameraBinding(binding.Id))
                continue;
            const Guid id = binding.Id;
            cameras->addAction(QString::fromStdString(binding.Name), [document, cut, id] { document->SetCutCamera(cut, id); });
        }
        if (cameras->isEmpty())
            cameras->setEnabled(false);
        menu.addAction(QStringLiteral("Delete Camera Cut"), [document, cut] { document->RemoveCut(cut); });
        menu.exec(event->globalPos());
        if (mModel.Actions.Changed)
            mModel.Actions.Changed();
        return;
    }

    const SequencerRow* row = RowAt(position.y());
    if (row == nullptr)
        return;
    const SequencerRow current = *row;

    SequencerTimelineModel::RowKey key;
    if (HitKey(current, position, key))
    {
        std::set<SequencerKeyHandle>& selection = document->SelectedKeys();
        const bool selected = std::all_of(key.Handles.begin(), key.Handles.end(),
                                          [&](const SequencerKeyHandle& handle) { return selection.count(handle) != 0; });
        if (!selected)
        {
            selection.clear();
            selection.insert(key.Handles.begin(), key.Handles.end());
        }

        const bool hasPropertyKeys = std::any_of(selection.begin(), selection.end(), [](const SequencerKeyHandle& handle) {
            return handle.Channel != SequencerKeyHandle::kEventChannel;
        });
        if (selection.size() == 1 && mModel.Actions.EditKey)
        {
            const SequencerKeyHandle only = *selection.begin();
            menu.addAction(QStringLiteral("Edit Key..."), [this, only] { mModel.Actions.EditKey(only); });
        }
        if (hasPropertyKeys)
        {
            QMenu* interpolation = menu.addMenu(QStringLiteral("Interpolation"));
            const KeyInterpolation modes[] = { KeyInterpolation::Auto,   KeyInterpolation::Cubic,
                                               KeyInterpolation::Bezier, KeyInterpolation::Linear,
                                               KeyInterpolation::Constant, KeyInterpolation::EaseIn,
                                               KeyInterpolation::EaseOut, KeyInterpolation::EaseInOut };
            for (KeyInterpolation mode : modes)
            {
                interpolation->addAction(QString::fromUtf8(KeyInterpolationName(mode)),
                                         [document, mode] { document->SetSelectedInterpolation(mode); });
            }
            QMenu* tangents = menu.addMenu(QStringLiteral("Tangents"));
            tangents->addAction(QStringLiteral("Auto"), [document] { document->SetSelectedTangentMode(TangentMode::Auto); });
            tangents->addAction(QStringLiteral("Unified"), [document] { document->SetSelectedTangentMode(TangentMode::Unified); });
            tangents->addAction(QStringLiteral("Broken"), [document] { document->SetSelectedTangentMode(TangentMode::Broken); });
        }
        menu.addSeparator();
        menu.addAction(QStringLiteral("Copy"), [document] { document->CopySelectedKeys(); });
        menu.addAction(QStringLiteral("Duplicate"), [document] { document->DuplicateSelectedKeys(); });
        if (selection.size() > 1 && mModel.Actions.ScaleSelection)
            menu.addAction(QStringLiteral("Scale Selection..."), [this] { mModel.Actions.ScaleSelection(); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Delete"), [document] { document->DeleteSelectedKeys(); });
    }
    else
    {
        switch (current.RowKind)
        {
        case SequencerRow::Kind::Track:
        case SequencerRow::Kind::Group:
        case SequencerRow::Kind::Channel:
        {
            unsigned mask = 0;
            for (int i = current.ChannelFirst; i < current.ChannelFirst + current.ChannelCount; ++i)
                mask |= 1u << i;
            menu.addAction(QStringLiteral("Add Key Here"), [document, current, time, mask] {
                document->AddKey(current.Track, time, mask);
            });
            break;
        }
        case SequencerRow::Kind::CameraCuts:
        {
            QMenu* add = menu.addMenu(QStringLiteral("Add Camera Cut Here"));
            if (mModel.Actions.FillAddCutMenu)
                mModel.Actions.FillAddCutMenu(*add, time);
            break;
        }
        case SequencerRow::Kind::Events:
            menu.addAction(QStringLiteral("Add Event Here..."), [this, time] {
                if (mModel.Actions.AddEventAt)
                    mModel.Actions.AddEventAt(time);
            });
            break;
        default:
            break;
        }
        if (document->CanPaste())
            menu.addAction(QStringLiteral("Paste Here"), [document, time] { document->Paste(time); });
    }

    if (menu.isEmpty())
        return;
    menu.exec(event->globalPos());
    if (mModel.Actions.Changed)
        mModel.Actions.Changed();
}
