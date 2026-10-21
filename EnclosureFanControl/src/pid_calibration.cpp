#include "pid_calibration.h"

#include <math.h>

// Pi as a float literal rather than M_PI. M_PI is a POSIX extension that some
// toolchains only expose with _USE_MATH_DEFINES, and this file is also built by
// the host-side simulation where that is easy to get wrong.
static const float kPi = 3.14159265358979323846f;

namespace PidCalibration
{
    void Autotuner::begin(float outputLow, float outputHigh, float noisebandC)
    {
        relay = Relay();
        freq = Frequency();
        amp = Amplitude();

        relay.outputNegative = outputLow;
        relay.outputPositive = outputHigh;
        relay.noiseband = noisebandC;
        // Same ratio as ESPHome: the zero-crossing detector needs a tighter band
        // than the relay, or sensor noise produces spurious crossings and a
        // period that is too short.
        freq.noiseband = noisebandC / 4.0f;

        enoughDataPhase = 0;
    }

    float Autotuner::update(float errorC, uint32_t nowMs, bool &finished, Result &result)
    {
        finished = false;

        const float output = relay.update(errorC);
        freq.update(nowMs, errorC);
        amp.update(errorC, relay.state);

        if (!freq.hasEnough() || !amp.hasEnough())
            return output;

        const bool symmetrical = freq.symmetrical();
        const bool convergent = amp.convergent();

        // ESPHome's quality gate. A failed check does not abort or retry: the
        // result is computed anyway and flagged, so the dialog can warn about
        // it. ESPHome's six-phase grace is effectively unreachable in its
        // source too - the first failure sets enough_data_phase_ and falls
        // through to the calculation, which this port reproduces.
        if (!symmetrical || !convergent)
        {
            if (enoughDataPhase == 0)
                enoughDataPhase = relay.phaseCount;
            else if ((uint16_t)(relay.phaseCount - enoughDataPhase) <= 6)
                return output;
        }

        const float amplitude = amp.meanAmplitude();
        const float period = freq.meanPeriod();
        const float d = (relay.outputPositive - relay.outputNegative) / 2.0f;

        // A degenerate measurement (zero amplitude, zero period) cannot produce
        // usable gains. Leave the experiment running: the regulator's 20-minute
        // ceiling will stop it and report why, which is more honest than
        // inventing a result from nothing.
        if (!(amplitude > 0.0f) || !(period > 0.0f) || !(d > 0.0f) ||
            !isfinite(amplitude) || !isfinite(period) || !isfinite(d))
            return output;

        Result r;
        r.valid = true;
        r.amplitude = amplitude;
        r.relayDelta = d;
        // Ku = 4d / (pi * a), the describing-function result for a relay.
        r.ku = 4.0f * d / (kPi * amplitude);
        r.pu = period;
        // Ziegler-Nichols PID rule, as used by ESPHome.
        r.gains.kp = 0.6f * r.ku;
        r.gains.ki = 1.2f * r.ku / r.pu;
        r.gains.kd = 0.075f * r.ku * r.pu;

        const PidGains raw = r.gains;
        pidClampGains(r.gains);
        r.clamped = (raw.kp != r.gains.kp) || (raw.ki != r.gains.ki) || (raw.kd != r.gains.kd);
        r.zcSymmetrical = symmetrical;
        r.amplitudeConvergent = convergent;
        r.proceededWithWarning = (!symmetrical || !convergent);
        r.phases = relay.phaseCount;

        result = r;
        finished = true;
        return output;
    }

    // ===================== RelayFunction =====================

    float Autotuner::Relay::update(float error)
    {
        if (state == State::Init)
            state = (error > noiseband) ? State::Positive : State::Negative;

        bool changed = false;
        if (state == State::Positive && error < -noiseband)
        {
            state = State::Negative;
            changed = true;
        }
        else if (state == State::Negative && error > noiseband)
        {
            state = State::Positive;
            changed = true;
        }

        if (changed)
            phaseCount++;

        return (state == State::Positive) ? outputPositive : outputNegative;
    }

    // ================= OscillationFrequencyDetector =================

    void Autotuner::Frequency::update(uint32_t nowMs, float error)
    {
        if (state == State::Init)
            state = (error > noiseband) ? State::Positive : State::Negative;

        bool crossed = false;
        if (state == State::Positive && error < -noiseband)
        {
            state = State::Negative;
            crossed = true;
        }
        else if (state == State::Negative && error > noiseband)
        {
            state = State::Positive;
            crossed = true;
        }

        if (!crossed)
            return;

        // A crossing is half a period, so consecutive crossing timestamps are
        // recorded and doubled in meanPeriod().
        if (hasLastCross)
        {
            const uint32_t dt = nowMs - lastCrossMs;
            if (count < kMaxIntervals)
            {
                intervals[count++] = dt;
            }
            else
            {
                for (uint8_t i = 1; i < kMaxIntervals; ++i)
                    intervals[i - 1] = intervals[i];
                intervals[kMaxIntervals - 1] = dt;
            }
        }
        hasLastCross = true;
        lastCrossMs = nowMs;
    }

    float Autotuner::Frequency::meanPeriod() const
    {
        if (count == 0)
            return 0.0f;

        uint64_t sum = 0;
        for (uint8_t i = 0; i < count; ++i)
            sum += intervals[i];

        // Mean half-period in seconds, doubled to a full period.
        return (float)((double)sum / (double)count / 1000.0 * 2.0);
    }

    bool Autotuner::Frequency::symmetrical() const
    {
        if (count == 0)
            return false;

        uint32_t minimum = intervals[0];
        uint32_t maximum = intervals[0];
        for (uint8_t i = 0; i < count; ++i)
        {
            if (intervals[i] < minimum)
                minimum = intervals[i];
            if (intervals[i] > maximum)
                maximum = intervals[i];
        }
        if (maximum == 0)
            return false;

        // ESPHome's threshold: the rising and falling halves must take within
        // roughly a factor of 1.5 of each other. A much faster cool than heat (or
        // the reverse) means the system is not the symmetric process ZN assumes.
        return ((float)minimum / (float)maximum) >= 0.66f;
    }

    // ================= OscillationAmplitudeDetector =================

    void Autotuner::Amplitude::update(float error, Relay::State relayState)
    {
        if (relayState != lastState)
        {
            // The extreme in the phase that just ended is one half-cycle behind
            // the relay transition, because the process lags the actuator. That
            // is why the previous phase's max/min is recorded here rather than
            // the current one's.
            if (lastState == Relay::State::Positive)
                push(maxs, maxCount, phaseMax);
            else if (lastState == Relay::State::Negative)
                push(mins, minCount, phaseMin);

            phaseMin = error;
            phaseMax = error;
        }
        lastState = relayState;

        if (error < phaseMin)
            phaseMin = error;
        if (error > phaseMax)
            phaseMax = error;
    }

    void Autotuner::Amplitude::push(float *values, uint8_t &count, float value)
    {
        if (count < kMaxPhases)
        {
            values[count++] = value;
            return;
        }
        // Keep the newest 7. The first phases are the start-up transient, so
        // discarding the oldest is what makes the result describe the settled
        // oscillation rather than the approach to it.
        for (uint8_t i = 1; i < kMaxPhases; ++i)
            values[i - 1] = values[i];
        values[kMaxPhases - 1] = value;
    }

    float Autotuner::Amplitude::meanAmplitude() const
    {
        // At least 3 phases, so the loop below has at least one pair after
        // discarding the first phase (ESPHome's rule: the first phase depends on
        // where the process started).
        const uint8_t n = (minCount < maxCount) ? minCount : maxCount;
        if (n < 3)
            return 0.0f;

        float total = 0.0f;
        uint8_t samples = 0;
        for (uint8_t i = 1; i + 1 < n; ++i)
        {
            total += fabsf(maxs[i] - mins[i + 1]);
            samples++;
        }
        if (samples == 0)
            return 0.0f;

        // Peak-to-peak to amplitude from the centre.
        return total / (float)samples / 2.0f;
    }

    bool Autotuner::Amplitude::convergent() const
    {
        if (minCount == 0 || maxCount == 0)
            return false;

        float globalMin = mins[0];
        float globalMax = maxs[0];
        for (uint8_t i = 0; i < minCount; ++i)
        {
            if (mins[i] < globalMin)
                globalMin = mins[i];
        }
        for (uint8_t i = 0; i < maxCount; ++i)
        {
            if (maxs[i] > globalMax)
                globalMax = maxs[i];
        }

        const float globalAmplitude = (globalMax - globalMin) / 2.0f;
        if (!(globalAmplitude > 0.0f))
            return false;

        const float mean = meanAmplitude();
        return (mean - globalAmplitude) / globalAmplitude < 0.05f;
    }
} // namespace PidCalibration
