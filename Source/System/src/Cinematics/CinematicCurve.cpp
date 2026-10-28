#include "System/Cinematics/CinematicCurve.h"

#include <algorithm>
#include <cmath>

namespace
{
    struct InterpolationInfo
    {
        KeyInterpolation Value;
        const char* Id;
        const char* Name;
    };

    constexpr InterpolationInfo kInterpolations[] = {
        { KeyInterpolation::Constant, "constant", "Constant" },
        { KeyInterpolation::Linear, "linear", "Linear" },
        { KeyInterpolation::Cubic, "cubic", "Cubic" },
        { KeyInterpolation::Bezier, "bezier", "Bezier" },
        { KeyInterpolation::Auto, "auto", "Auto" },
        { KeyInterpolation::EaseIn, "easein", "Ease In" },
        { KeyInterpolation::EaseOut, "easeout", "Ease Out" },
        { KeyInterpolation::EaseInOut, "easeinout", "Ease In Out" },
    };

    bool UsesAutoTangents(const Keyframe& key)
    {
        return key.Interpolation == KeyInterpolation::Auto || key.Tangents == TangentMode::Auto;
    }

    double CubicBezier(double p0, double p1, double p2, double p3, double s)
    {
        const double inv = 1.0 - s;
        return inv * inv * inv * p0 + 3.0 * inv * inv * s * p1 + 3.0 * inv * s * s * p2 + s * s * s * p3;
    }

    double CubicBezierDerivative(double p0, double p1, double p2, double p3, double s)
    {
        const double inv = 1.0 - s;
        return 3.0 * inv * inv * (p1 - p0) + 6.0 * inv * s * (p2 - p1) + 3.0 * s * s * (p3 - p2);
    }

    // The easing shapes are cubic, matching the slope at the eased end of a Cubic segment
    // with a flat tangent, so switching a key between Ease Out and Auto with flat tangents
    // moves the curve only slightly.
    double Ease(KeyInterpolation interpolation, double u)
    {
        switch (interpolation)
        {
        case KeyInterpolation::EaseIn:
            return u * u * u;
        case KeyInterpolation::EaseOut:
        {
            const double inv = 1.0 - u;
            return 1.0 - inv * inv * inv;
        }
        case KeyInterpolation::EaseInOut:
        {
            if (u < 0.5)
                return 4.0 * u * u * u;
            const double inv = -2.0 * u + 2.0;
            return 1.0 - inv * inv * inv * 0.5;
        }
        default:
            return u;
        }
    }
}

const char* KeyInterpolationId(KeyInterpolation interpolation)
{
    for (const InterpolationInfo& info : kInterpolations)
    {
        if (info.Value == interpolation)
            return info.Id;
    }
    return "auto";
}

const char* KeyInterpolationName(KeyInterpolation interpolation)
{
    for (const InterpolationInfo& info : kInterpolations)
    {
        if (info.Value == interpolation)
            return info.Name;
    }
    return "Auto";
}

KeyInterpolation KeyInterpolationFromId(const std::string& id)
{
    for (const InterpolationInfo& info : kInterpolations)
    {
        if (id == info.Id)
            return info.Value;
    }
    return KeyInterpolation::Auto;
}

const char* TangentModeId(TangentMode mode)
{
    switch (mode)
    {
    case TangentMode::Auto: return "auto";
    case TangentMode::Unified: return "unified";
    case TangentMode::Broken: return "broken";
    }
    return "auto";
}

TangentMode TangentModeFromId(const std::string& id)
{
    if (id == "unified")
        return TangentMode::Unified;
    if (id == "broken")
        return TangentMode::Broken;
    return TangentMode::Auto;
}

void FloatChannel::SetValueKind(ChannelValueKind kind)
{
    mValueKind = kind;
    if (kind == ChannelValueKind::Bool)
    {
        for (Keyframe& key : mKeys)
            key.Interpolation = KeyInterpolation::Constant;
    }
}

std::uint32_t FloatChannel::SetKey(double time, double value, KeyInterpolation interpolation, double timeTolerance)
{
    const int existing = FindKeyIndexAt(time, timeTolerance);
    if (existing >= 0)
    {
        mKeys[existing].Value = value;
        const std::uint32_t id = mKeys[existing].Id;
        OnKeysChanged();
        return id;
    }

    Keyframe key;
    key.Time = time;
    key.Value = value;
    key.Interpolation = interpolation;
    return InsertKey(key);
}

std::uint32_t FloatChannel::InsertKey(Keyframe key)
{
    key.Id = mNextKeyId++;
    if (mValueKind == ChannelValueKind::Bool)
        key.Interpolation = KeyInterpolation::Constant;

    const std::uint32_t id = key.Id;
    mKeys.push_back(key);
    OnKeysChanged();
    return id;
}

int FloatChannel::FindKeyIndexById(std::uint32_t id) const
{
    for (int i = 0; i < static_cast<int>(mKeys.size()); ++i)
    {
        if (mKeys[i].Id == id)
            return i;
    }
    return -1;
}

int FloatChannel::FindKeyIndexAt(double time, double tolerance) const
{
    // Keys are sorted, so the candidates are the neighbours of the insertion point.
    const auto it = std::lower_bound(mKeys.begin(), mKeys.end(), time - tolerance,
                                     [](const Keyframe& key, double t) { return key.Time < t; });
    if (it != mKeys.end() && std::fabs(it->Time - time) <= tolerance)
        return static_cast<int>(it - mKeys.begin());
    return -1;
}

const Keyframe* FloatChannel::FindKeyById(std::uint32_t id) const
{
    const int index = FindKeyIndexById(id);
    return index >= 0 ? &mKeys[index] : nullptr;
}

bool FloatChannel::RemoveKeyById(std::uint32_t id)
{
    const int index = FindKeyIndexById(id);
    if (index < 0)
        return false;

    mKeys.erase(mKeys.begin() + index);
    OnKeysChanged();
    return true;
}

void FloatChannel::ClearKeys()
{
    mKeys.clear();
    mSegmentHint = 0;
}

Keyframe* FloatChannel::MutableKeyById(std::uint32_t id)
{
    const int index = FindKeyIndexById(id);
    return index >= 0 ? &mKeys[index] : nullptr;
}

void FloatChannel::OnKeysChanged()
{
    if (mValueKind == ChannelValueKind::Bool)
    {
        for (Keyframe& key : mKeys)
            key.Interpolation = KeyInterpolation::Constant;
    }

    SortKeys();
    ComputeAutoTangents();
    mSegmentHint = 0;
}

void FloatChannel::SortKeys()
{
    // Stable, so two keys dragged onto the same time keep the order they had - otherwise
    // the step at that instant could flip between evaluations.
    std::stable_sort(mKeys.begin(), mKeys.end(),
                     [](const Keyframe& a, const Keyframe& b) { return a.Time < b.Time; });
}

void FloatChannel::ComputeAutoTangents()
{
    const int count = static_cast<int>(mKeys.size());
    for (int i = 0; i < count; ++i)
    {
        Keyframe& key = mKeys[i];
        if (!UsesAutoTangents(key))
            continue;

        // Flat at the ends: an animation settles into its first and last pose rather
        // than arriving at speed.
        if (i == 0 || i == count - 1)
        {
            key.ArriveTangent = 0.0;
            key.LeaveTangent = 0.0;
            continue;
        }

        const Keyframe& previous = mKeys[i - 1];
        const Keyframe& next = mKeys[i + 1];
        const double spanBefore = key.Time - previous.Time;
        const double spanAfter = next.Time - key.Time;
        double slope = 0.0;

        if (spanBefore > 1e-9 && spanAfter > 1e-9)
        {
            const double secantBefore = (key.Value - previous.Value) / spanBefore;
            const double secantAfter = (next.Value - key.Value) / spanAfter;

            // A local extremum stays flat, so the curve never overshoots the value the
            // animator keyed - a light keyed to zero must not dip below it.
            if (secantBefore * secantAfter > 0.0)
            {
                slope = (next.Value - previous.Value) / (next.Time - previous.Time);

                // Fritsch-Carlson limit: beyond three times the smaller secant a Hermite
                // segment stops being monotonic and bulges past its end values.
                const double limit = 3.0 * (std::min)(std::fabs(secantBefore), std::fabs(secantAfter));
                if (std::fabs(slope) > limit)
                    slope = slope > 0.0 ? limit : -limit;
            }
        }

        key.ArriveTangent = slope;
        key.LeaveTangent = slope;
    }
}

int FloatChannel::FindSegment(double time) const
{
    const int count = static_cast<int>(mKeys.size());
    auto contains = [&](int index) {
        return index >= 0 && index < count - 1 && mKeys[index].Time <= time && time < mKeys[index + 1].Time;
    };

    if (contains(mSegmentHint))
        return mSegmentHint;
    if (contains(mSegmentHint + 1))
        return ++mSegmentHint;

    // The first key strictly after `time`; the segment starts one before it.
    const auto it = std::upper_bound(mKeys.begin(), mKeys.end(), time,
                                     [](double t, const Keyframe& key) { return t < key.Time; });
    int index = static_cast<int>(it - mKeys.begin()) - 1;
    index = std::clamp(index, 0, count - 2);
    mSegmentHint = index;
    return index;
}

double FloatChannel::EvaluateSegment(int index, double time) const
{
    const Keyframe& from = mKeys[index];
    const Keyframe& to = mKeys[index + 1];
    const double span = to.Time - from.Time;
    if (span <= 1e-12)
        return to.Value;

    const double u = std::clamp((time - from.Time) / span, 0.0, 1.0);

    switch (from.Interpolation)
    {
    case KeyInterpolation::Constant:
        return from.Value;

    case KeyInterpolation::Linear:
        return from.Value + (to.Value - from.Value) * u;

    case KeyInterpolation::EaseIn:
    case KeyInterpolation::EaseOut:
    case KeyInterpolation::EaseInOut:
        return from.Value + (to.Value - from.Value) * Ease(from.Interpolation, u);

    case KeyInterpolation::Bezier:
    {
        // Handles may not reach past the other key, otherwise the time curve folds back
        // on itself and one time would have several values.
        const double leaveWeight = from.LeaveWeight > 0.0 ? (std::min)(from.LeaveWeight, span) : span / 3.0;
        const double arriveWeight = to.ArriveWeight > 0.0 ? (std::min)(to.ArriveWeight, span) : span / 3.0;
        const double x0 = from.Time;
        const double x1 = from.Time + leaveWeight;
        const double x2 = to.Time - arriveWeight;
        const double x3 = to.Time;

        // Solve x(s) = time for the curve parameter. Newton from the linear guess,
        // falling back to bisection whenever a step would leave the bracket.
        double low = 0.0;
        double high = 1.0;
        double s = u;
        for (int iteration = 0; iteration < 24; ++iteration)
        {
            const double x = CubicBezier(x0, x1, x2, x3, s) - time;
            if (std::fabs(x) < 1e-9 * (std::max)(1.0, span))
                break;
            if (x > 0.0)
                high = s;
            else
                low = s;

            const double derivative = CubicBezierDerivative(x0, x1, x2, x3, s);
            double next = derivative > 1e-12 ? s - x / derivative : -1.0;
            if (next <= low || next >= high)
                next = 0.5 * (low + high);
            s = next;
        }

        const double y0 = from.Value;
        const double y1 = from.Value + from.LeaveTangent * leaveWeight;
        const double y2 = to.Value - to.ArriveTangent * arriveWeight;
        const double y3 = to.Value;
        return CubicBezier(y0, y1, y2, y3, s);
    }

    case KeyInterpolation::Cubic:
    case KeyInterpolation::Auto:
    default:
    {
        const double u2 = u * u;
        const double u3 = u2 * u;
        const double h00 = 2.0 * u3 - 3.0 * u2 + 1.0;
        const double h10 = u3 - 2.0 * u2 + u;
        const double h01 = -2.0 * u3 + 3.0 * u2;
        const double h11 = u3 - u2;
        return h00 * from.Value + h10 * span * from.LeaveTangent + h01 * to.Value + h11 * span * to.ArriveTangent;
    }
    }
}

double FloatChannel::Round(double value) const
{
    switch (mValueKind)
    {
    case ChannelValueKind::Int:
        return std::round(value);
    case ChannelValueKind::Bool:
        return value >= 0.5 ? 1.0 : 0.0;
    default:
        return value;
    }
}

double FloatChannel::EvaluateRaw(double time) const
{
    const int count = static_cast<int>(mKeys.size());
    if (count == 0)
        return DefaultValue;
    if (count == 1 || time <= mKeys.front().Time)
        return mKeys.front().Value;
    if (time >= mKeys.back().Time)
        return mKeys.back().Value;

    return EvaluateSegment(FindSegment(time), time);
}

double FloatChannel::Evaluate(double time) const
{
    return Round(EvaluateRaw(time));
}

bool FloatChannel::GetTimeRange(double& first, double& last) const
{
    if (mKeys.empty())
        return false;
    first = mKeys.front().Time;
    last = mKeys.back().Time;
    return true;
}
