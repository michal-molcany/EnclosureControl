#ifndef PID_CALIBRATION_H
#define PID_CALIBRATION_H

// Relay-feedback PID autotuner for the chamber cooling loop.
//
// PORTED FROM ESPHOME
// -------------------
// This is a line-by-line port of ESPHome's PIDAutotuner
// (esphome/components/pid/pid_autotuner.cpp), which is the implementation the
// project asked for. The structure - hysteretic relay, zero-crossing frequency
// detector, phase amplitude detector, then the Ziegler-Nichols gains - is kept
// intact so the behaviour is the familiar one, not a lookalike. The only
// deliberate difference is the control direction:
//
//   ESPHome is written for a HEATER: error = setpoint - process, and a positive
//   error selects output_positive (heat).
//
//   The chamber loop is COOLING ONLY: error = chamber - setpoint, and a
//   positive error selects output_positive, which this port maps to FULL
//   COOLING. output_negative is then NO COOLING.
//
// Both detectors and the ZN formulas are sign-symmetric, so flipping the error
// and the two relay levels changes nothing about the maths - it only names the
// relay levels the right way round for this hardware.
//
// WHY THIS LIVES OUTSIDE THE REGULATOR
// ------------------------------------
// The class has no Arduino or hardware dependency beyond the shared PidGains
// definition, and takes its timestamp as an argument. That keeps the algorithm
// testable against a simulated process on a workstation, which is the only way
// to check the maths without running a real 20-minute thermal experiment.
//
// UNITS
// -----
// errorC:     degrees C, chamber minus setpoint.
// output:     the regulator's 0-200 span (0-100 = louvre, 100-200 = fan).
// amplitude:  degrees C, half the peak-to-peak oscillation.
// ku:         output units per degree C - the same units as kp.
// pu:         seconds.
// gains:      directly usable as PidGains, already clamped to the ranges in
//             enclosure_pid.h (result.clamped says whether clamping bit).

#include <stdint.h>
#include <enclosure_pid.h>

namespace PidCalibration
{
    // Lifecycle, shared with the web UI as a numeric code. Append only: the
    // page maps these to labels, so renumbering would silently relabel a run.
    enum class Phase : uint8_t
    {
        None = 0,
        Preheating = 1,
        Settling = 2,
        Relay = 3,
        Done = 4,
        Aborted = 5
    };

    // What the experiment measured and what it computed. `valid` is the only
    // field a caller needs to check before using the rest.
    struct Result
    {
        bool valid = false;
        float ku = 0.0f;             // ultimate gain, output/C
        float pu = 0.0f;             // ultimate period, seconds
        float amplitude = 0.0f;      // measured oscillation amplitude a, C
        float relayDelta = 0.0f;     // relay magnitude d, output units
        PidGains gains = {0.0f, 0.0f, 0.0f};
        bool clamped = false;        // one or more gains hit a PID_* limit
        bool zcSymmetrical = false;  // ESPHome's symmetry quality gate
        bool amplitudeConvergent = false;
        // True when the symmetry/convergence check failed and the result was
        // computed anyway. ESPHome's first failure falls through to the
        // calculation (its six-phase grace is unreachable), so this is a
        // warning, not a retry. The result is still usable, but the page should
        // say so.
        bool proceededWithWarning = false;
        uint16_t phases = 0;
    };

    // What the regulator reports to the web UI. Plain data so the programme can
    // copy it into EnclosureUi::Status without this module knowing that library
    // exists.
    struct Info
    {
        Phase phase = Phase::None;
        bool running = false;
        bool hasResult = false;
        bool aborted = false;
        uint32_t elapsedMs = 0;
        uint16_t cycles = 0;
        float ku = 0.0f;
        float pu = 0.0f;
        float amplitude = 0.0f;
        float relayDelta = 0.0f;
        PidGains oldGains = {0.0f, 0.0f, 0.0f};
        PidGains newGains = {0.0f, 0.0f, 0.0f};
        bool clamped = false;
        bool applied = false;
        bool zcSymmetrical = false;
        bool amplitudeConvergent = false;
        bool proceededWithWarning = false;
        float bedTargetC = 0.0f;
        float setpointC = 0.0f;
        // Fixed buffer, not a String: this struct crosses the module boundary
        // and is copied by value, and a String would make that allocation and
        // lifetime story worse for no benefit. Always NUL-terminated.
        char error[96] = {0};
    };

    class Autotuner
    {
    public:
        // Clears all state and configures the relay. `outputLow`/`outputHigh`
        // are in the regulator's 0-200 units; `noisebandC` is the relay
        // hysteresis in degrees C. The zero-crossing detector uses a quarter of
        // that, as ESPHome does, to suppress sensor noise.
        void begin(float outputLow, float outputHigh, float noisebandC);

        // Feeds one sample. `errorC` is chamber minus setpoint; `nowMs` is a
        // monotonic millisecond clock (millis() on the board). Returns the relay
        // output to apply. When the experiment has produced a result,
        // `finished` is set and `result` is filled; the caller should stop
        // feeding samples and leave the relay output in place.
        //
        // On the finish iteration the returned output is still the relay level
        // for that sample, so the caller never applies an unintended zero.
        float update(float errorC, uint32_t nowMs, bool &finished, Result &result);

        uint16_t phaseCount() const { return relay.phaseCount; }
        uint16_t zeroCrossings() const { return freq.count; }
        bool hasEnoughData() const { return freq.hasEnough() && amp.hasEnough(); }

        // Live diagnostics for the progress dialog. Safe to call at any time.
        float currentAmplitude() const { return amp.meanAmplitude(); }
        float currentPeriod() const { return freq.meanPeriod(); }

    private:
        // Fixed capacities instead of std::vector: this runs on a board whose
        // heap is already tight, and the experiment is bounded in time. ESPHome
        // keeps at most 7 phase extremes; 8 half-period intervals is more than
        // enough to average over the steady-state part of a 20-minute run.
        enum : uint8_t
        {
            kMaxIntervals = 8,
            kMaxPhases = 7
        };

        struct Relay
        {
            enum class State : uint8_t
            {
                Init,
                Positive,
                Negative
            };

            float update(float error);

            State state = State::Init;
            float noiseband = 0.5f;
            float outputPositive = 200.0f;
            float outputNegative = 0.0f;
            uint16_t phaseCount = 0;
        };

        struct Frequency
        {
            void update(uint32_t nowMs, float error);
            bool hasEnough() const { return count >= 2; }
            float meanPeriod() const;
            bool symmetrical() const;

            enum class State : uint8_t
            {
                Init,
                Positive,
                Negative
            };

            State state = State::Init;
            float noiseband = 0.125f;
            bool hasLastCross = false;
            uint32_t lastCrossMs = 0;
            uint32_t intervals[kMaxIntervals] = {0};
            uint8_t count = 0;
        };

        struct Amplitude
        {
            void update(float error, Relay::State relayState);
            bool hasEnough() const { return (minCount < maxCount ? minCount : maxCount) >= 3; }
            float meanAmplitude() const;
            bool convergent() const;

            float phaseMin = 0.0f;
            float phaseMax = 0.0f;
            float mins[kMaxPhases] = {0.0f};
            float maxs[kMaxPhases] = {0.0f};
            uint8_t minCount = 0;
            uint8_t maxCount = 0;
            Relay::State lastState = Relay::State::Init;

            static void push(float *values, uint8_t &count, float value);
        };

        Relay relay;
        Frequency freq;
        Amplitude amp;
        uint16_t enoughDataPhase = 0;
    };
} // namespace PidCalibration

#endif // PID_CALIBRATION_H
