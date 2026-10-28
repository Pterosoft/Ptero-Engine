#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Keyframes and the curves they make.
//
// Every animated value in a sequence - a float, one component of a vector or colour, a
// boolean, an integer - is stored as a FloatChannel. Booleans and integers are channels
// whose reads are rounded (and whose keys are forced to Constant for booleans), so the
// key editing, selection, undo and curve drawing are written once for all of them.

// How the curve travels from a key to the next one. Owned by the key the segment leaves.
enum class KeyInterpolation : std::uint8_t
{
    // Holds this key's value until the next key.
    Constant,
    Linear,
    // Hermite cubic through this key's leave tangent and the next key's arrive tangent.
    Cubic,
    // Cubic with weighted tangents: each handle also has a length in time, so the curve
    // can be pulled further along the handle than a plain cubic allows.
    Bezier,
    // Cubic whose tangents the channel computes itself: smooth through the keys, and flat
    // at a local minimum or maximum so the curve never overshoots the keyed values.
    Auto,
    EaseIn,
    EaseOut,
    EaseInOut
};

// How a key's two tangents relate. Only read by Cubic and Bezier keys - Auto keys always
// have computed tangents, and the other modes do not use tangents at all.
enum class TangentMode : std::uint8_t
{
    // Computed like an Auto key's, but the segment still uses the key's interpolation.
    Auto,
    // One tangent: setting either side sets both.
    Unified,
    // Arrive and leave are independent, which allows a sharp corner at the key.
    Broken
};

const char* KeyInterpolationId(KeyInterpolation interpolation);
const char* KeyInterpolationName(KeyInterpolation interpolation);
KeyInterpolation KeyInterpolationFromId(const std::string& id);
const char* TangentModeId(TangentMode mode);
TangentMode TangentModeFromId(const std::string& id);

struct Keyframe
{
    double Time = 0.0;
    double Value = 0.0;
    KeyInterpolation Interpolation = KeyInterpolation::Auto;
    TangentMode Tangents = TangentMode::Auto;
    // Slopes in value units per second.
    double ArriveTangent = 0.0;
    double LeaveTangent = 0.0;
    // Handle lengths in seconds, used by Bezier segments. Zero or less means a third of
    // the segment, which makes a Bezier segment identical to a Cubic one.
    double ArriveWeight = 0.0;
    double LeaveWeight = 0.0;
    // Identifies the key to editors while it moves and the list re-sorts around it.
    // Assigned by the channel, never saved; a reloaded channel numbers its keys afresh.
    std::uint32_t Id = 0;
};

// What reads of a channel are rounded to.
enum class ChannelValueKind : std::uint8_t
{
    Float,
    Int,
    Bool
};

class FloatChannel
{
public:
    // Used when the channel has no keys.
    double DefaultValue = 0.0;

    ChannelValueKind ValueKind() const { return mValueKind; }
    // A Bool channel forces every key to Constant; changing the kind re-applies that.
    void SetValueKind(ChannelValueKind kind);

    bool HasKeys() const { return !mKeys.empty(); }
    int KeyCount() const { return static_cast<int>(mKeys.size()); }
    const std::vector<Keyframe>& Keys() const { return mKeys; }

    // Adds a key, or replaces the value of one already within `timeTolerance` of `time`
    // (keeping that key's interpolation and id). Returns the key's id.
    std::uint32_t SetKey(double time, double value, KeyInterpolation interpolation = KeyInterpolation::Auto,
                         double timeTolerance = 1e-6);
    // Inserts a fully specified key as-is, assigning it a fresh id. For paste and loading.
    std::uint32_t InsertKey(Keyframe key);

    int FindKeyIndexById(std::uint32_t id) const;
    // Index of the key within `tolerance` of `time`, or -1.
    int FindKeyIndexAt(double time, double tolerance) const;
    const Keyframe* FindKeyById(std::uint32_t id) const;

    bool RemoveKeyById(std::uint32_t id);
    void ClearKeys();

    // Gives full access to a key. The caller must call OnKeysChanged() once it has
    // finished editing, which re-sorts the keys and recomputes automatic tangents.
    Keyframe* MutableKeyById(std::uint32_t id);
    void OnKeysChanged();

    // The value at `time`, rounded for Int and Bool channels. Before the first key the
    // curve holds the first key's value, after the last the last's.
    //
    // Cost is O(1) for playback - the segment found last time, or the one after it, is
    // checked first - and O(log n) for a jump. The remembered segment makes Evaluate
    // unsafe to call on one channel from two threads at once.
    double Evaluate(double time) const;

    // The unrounded curve, which is what a curve editor draws.
    double EvaluateRaw(double time) const;

    // Earliest and latest key times; false when there are no keys.
    bool GetTimeRange(double& first, double& last) const;

private:
    // Index i such that Keys[i].Time <= time < Keys[i + 1].Time, for 0 <= i < n - 1.
    // Only called with at least two keys and time strictly inside the key range.
    int FindSegment(double time) const;
    void SortKeys();
    void ComputeAutoTangents();
    double EvaluateSegment(int index, double time) const;
    double Round(double value) const;

    std::vector<Keyframe> mKeys;
    ChannelValueKind mValueKind = ChannelValueKind::Float;
    std::uint32_t mNextKeyId = 1;
    mutable int mSegmentHint = 0;
};
