#ifndef CAL_TRACE_H
#define CAL_TRACE_H

// Temperature history of one PID calibration run, for the web UI's graph.
//
// WHY THE BOARD KEEPS IT
// ----------------------
// The page could accumulate its own points from the 2 s status poll, and for a
// run watched from start to finish that would be enough. It is not enough for
// the way runs actually happen here:
//
//   - a run is started from the OctoPrint plugin or the CYD as often as from
//     the page, so the page is usually opened minutes after the preheat began;
//   - a reload throws the browser's history away;
//   - a backgrounded tab throttles its timers to roughly one tick a minute,
//     which turns a 5 s series into a dotted line exactly while the user is
//     looking at something else.
//
// The board is the only observer that is present for the whole experiment, so
// it is the one that records it.
//
// WHY 5 s AND A FIXED CAPACITY
// ----------------------------
// The relay oscillation has a period of minutes and the chamber sensor itself
// updates at 1 Hz, so 5 s is already far finer than the thing being measured.
// Storing 1 Hz samples for a 60-minute run would cost 21 KB to draw a curve with
// ten times more points than a 560 px canvas can show. 780 samples at 5 s cover
// 65 minutes - the 60-minute window plus the preheat and an aborted tail - in
// 6.2 KB.
//
// Header-only and free of Arduino types so the ring arithmetic can be checked
// without a board.
//
// RAM ONLY: a reboot loses the trace, but a reboot aborts the run anyway, so
// there is nothing to show afterwards either way.

#include <stdint.h>

// One point. Times are seconds since the run started, temperatures are 0.1 C -
// the same encoding /api/status uses, so the page has one unit to deal with.
// ENC_CHAMBER_INVALID marks a sample where the sensor had no usable reading;
// the page breaks the line there instead of drawing a cliff.
struct CalTraceSample
{
    uint16_t elapsedSec = 0;
    int16_t chamberCx10 = 0;
    int16_t bedCx10 = 0;
    // Cooling demand the run had applied at that moment, 0-200. 0 during the
    // preheat (the relay has not started), then the relay's two levels.
    uint8_t output = 0;
    // PidCalibration::Phase as a number, passed through so this header does not
    // have to include the autotuner.
    uint8_t phase = 0;
};

// Fixed-capacity ring, oldest sample first once it wraps.
//
// Templated on capacity rather than taking it at runtime: the array is a member,
// so the size has to be a compile-time constant anyway, and a runtime capacity
// argument would only add a way to get it wrong.
template <uint16_t Capacity>
class CalTrace
{
public:
    void reset()
    {
        count_ = 0;
        next_ = 0;
    }

    uint16_t count() const { return count_; }
    uint16_t capacity() const { return Capacity; }

    // Drops the oldest point once full. Silently ignores nothing: a trace that
    // stops collecting when full would draw a run that looks like it ended
    // early, which is worse than losing the start of it.
    void add(const CalTraceSample &s)
    {
        samples_[next_] = s;
        next_ = (uint16_t)((next_ + 1) % Capacity);
        if (count_ < Capacity)
            ++count_;
    }

    // Oldest first, 0 <= i < count().
    const CalTraceSample &at(uint16_t i) const
    {
        const uint16_t start = (count_ == Capacity) ? next_ : 0;
        return samples_[(uint16_t)((start + i) % Capacity)];
    }

private:
    CalTraceSample samples_[Capacity] = {};
    uint16_t count_ = 0;
    uint16_t next_ = 0;
};

#endif // CAL_TRACE_H
