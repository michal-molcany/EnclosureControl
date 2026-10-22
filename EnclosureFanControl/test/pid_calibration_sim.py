#!/usr/bin/env python3
"""Host-side check of the PID calibration design.

WHAT THIS IS
------------
A plain-Python model of the relay autotuner in
``EnclosureFanControl/src/pid_calibration.cpp`` (itself a port of ESPHome's
PIDAutotuner) driven by a first-order thermal process with transport delay.

It answers the questions that cannot be answered by reading the code:

  * does the 35 C setpoint with a 0/200 relay produce a stable limit cycle?
  * how long is the period, and therefore can a 20-minute run finish?
  * how big is the oscillation, and does it stay under the 50 C abort limit?
  * are the measured amplitude/period the ones the process actually produced?
  * do the Ziegler-Nichols gains land inside the ranges in enclosure_pid.h?

WHAT THIS IS NOT
----------------
It does not execute the C++ - there is no host C++ toolchain in this
environment. The C++ is covered by the firmware build; this script covers the
control design and the arithmetic. If the two ever disagree, the C++ is the
source of truth and this script needs updating.

Run from anywhere:

    python test/pid_calibration_sim.py
"""

import math

SETPOINT = 35.0
NOISEBAND = 0.5            # CAL_NOISEBAND_C
OUT_LO, OUT_HI = 0.0, 200.0  # CAL_RELAY_LOW / CAL_RELAY_HIGH
SAMPLE_S = 5.0             # OCTO_POLL_INTERVAL_MS
TICK_S = 1.0               # PID_UPDATE_INTERVAL
DURATION_S = 1200.0        # CAL_MAX_DURATION_MS

KP_MAX, KI_MAX, KD_MAX = 50.0, 5.0, 50.0
ABORT_C = 50.0             # CAL_ABORT_CHAMBER_C


class Relay:
    def __init__(self):
        self.state = None
        self.phase_count = 0

    def update(self, error):
        if self.state is None:
            self.state = 1 if error > NOISEBAND else -1
        changed = False
        if self.state == 1 and error < -NOISEBAND:
            self.state = -1
            changed = True
        elif self.state == -1 and error > NOISEBAND:
            self.state = 1
            changed = True
        if changed:
            self.phase_count += 1
        return OUT_HI if self.state == 1 else OUT_LO


class Frequency:
    """Zero-crossing detector; intervals are half-periods, in milliseconds."""

    def __init__(self):
        self.state = None
        self.last = None
        self.intervals = []

    def update(self, now_ms, error):
        if self.state is None:
            self.state = 1 if error > NOISEBAND / 4 else -1
        crossed = False
        if self.state == 1 and error < -NOISEBAND / 4:
            self.state = -1
            crossed = True
        elif self.state == -1 and error > NOISEBAND / 4:
            self.state = 1
            crossed = True
        if not crossed:
            return
        if self.last is not None:
            self.intervals.append(now_ms - self.last)
            if len(self.intervals) > 8:
                self.intervals.pop(0)
        self.last = now_ms

    def has_enough(self):
        return len(self.intervals) >= 2

    def mean_period(self):
        if not self.intervals:
            return 0.0
        return sum(self.intervals) / len(self.intervals) / 1000.0 * 2.0

    def symmetrical(self):
        if not self.intervals:
            return False
        return min(self.intervals) / max(self.intervals) >= 0.66


class Amplitude:
    """Phase peak-to-peak detector; keeps the newest seven phases."""

    def __init__(self):
        self.phase_min = 0.0
        self.phase_max = 0.0
        self.mins = []
        self.maxs = []
        self.last_state = None

    def update(self, error, relay_state):
        if relay_state != self.last_state:
            if self.last_state == 1:
                self.maxs.append(self.phase_max)
                self.maxs = self.maxs[-7:]
            elif self.last_state == -1:
                self.mins.append(self.phase_min)
                self.mins = self.mins[-7:]
            self.phase_min = error
            self.phase_max = error
        self.last_state = relay_state
        self.phase_min = min(self.phase_min, error)
        self.phase_max = max(self.phase_max, error)

    def has_enough(self):
        return min(len(self.mins), len(self.maxs)) >= 3

    def mean_amplitude(self):
        n = min(len(self.mins), len(self.maxs))
        if n < 3:
            return 0.0
        # Discard the first phase: it depends on where the process started.
        pairs = [abs(self.maxs[i] - self.mins[i + 1]) for i in range(1, n - 1)]
        if not pairs:
            return 0.0
        return sum(pairs) / len(pairs) / 2.0

    def convergent(self):
        if not self.mins or not self.maxs:
            return False
        ga = (max(self.maxs) - min(self.mins)) / 2.0
        if ga <= 0:
            return False
        return (self.mean_amplitude() - ga) / ga < 0.05


def simulate(t_hot, t_cool, tau, deadtime):
    """First-order chamber with a transport delay on the cooling command.

    t_hot: equilibrium with no cooling (bed at 90 C, louvre shut)
    t_cool: equilibrium with full cooling
    tau: thermal time constant, seconds
    deadtime: seconds between a relay step and the chamber responding
    """
    relay, freq, amp = Relay(), Frequency(), Amplitude()
    temp = SETPOINT
    delay = [OUT_LO] * max(1, int(deadtime / TICK_S))
    held = temp
    next_sample = 0.0
    result = None
    t = 0.0
    temps = []
    crossings = []

    while t <= DURATION_S:
        if t >= next_sample:
            held = temp
            next_sample += SAMPLE_S
        error = held - SETPOINT

        out = relay.update(error)
        freq.update(t * 1000.0, error)
        amp.update(error, relay.state)

        if freq.has_enough() and amp.has_enough() and result is None:
            symmetrical = freq.symmetrical()
            convergent = amp.convergent()
            # A failed quality gate is a warning, not a retry. ESPHome's source
            # falls through to the calculation on the first failure (its
            # six-phase grace is unreachable), and so does this model. The flags
            # travel with the result.
            amplitude = amp.mean_amplitude()
            period = freq.mean_period()
            if amplitude > 0 and period > 0:
                d = (OUT_HI - OUT_LO) / 2.0
                ku = 4.0 * d / (math.pi * amplitude)
                result = {
                    "t": t,
                    "ku": ku,
                    "pu": period,
                    "amp": amplitude,
                    "d": d,
                    "kp": 0.6 * ku,
                    "ki": 1.2 * ku / period,
                    "kd": 0.075 * ku * period,
                    "sym": symmetrical,
                    "conv": convergent,
                    "phases": relay.phase_count,
                }

        # transport delay, then the thermal response
        delay.append(out)
        u_eff = delay.pop(0)
        target = t_hot + (t_cool - t_hot) * (u_eff / 200.0)
        temp += (target - temp) * TICK_S / tau
        temps.append((t, temp))

        # Independent measurement for the assertions: an upward zero crossing
        # of the ACTUAL temperature, and the peak/trough between them.
        if len(temps) >= 2:
            prev = temps[-2][1] - SETPOINT
            curr = temps[-1][1] - SETPOINT
            if prev < 0.0 <= curr:
                crossings.append(t)
        t += TICK_S

    return {
        "result": result,
        "temps": temps,
        "crossings": crossings,
        "max_c": max(v for _, v in temps),
        "min_c": min(v for _, v in temps),
        "end_c": temp,
    }


def true_period(run):
    """Mean period between the last few upward crossings of the real curve."""
    crossings = run["crossings"]
    if len(crossings) < 2:
        return None
    periods = [b - a for a, b in zip(crossings[-4:-1], crossings[-3:])]
    return sum(periods) / len(periods)


def true_amplitude(run):
    """Half peak-to-peak over the settled part of the run (after 60 s)."""
    late = [v for t, v in run["temps"] if t >= 60.0]
    if not late:
        return None
    return (max(late) - min(late)) / 2.0


def clamp(value, lo, hi):
    return max(lo, min(hi, value))


def check_case(name, params, expect_result=True):
    t_hot, t_cool, tau, deadtime = params
    run = simulate(*params)
    result = run["result"]

    print(
        "%-8s bed-load %.0f/%.0f C tau=%3.0fs delay=%2.0fs  "
        "chamber %.1f..%.1f C"
        % (name, t_hot, t_cool, tau, deadtime, run["min_c"], run["max_c"])
    )

    if not expect_result:
        assert result is None, "%s: expected no result, got %r" % (name, result)
        print("         no result (correct: the process never crosses the setpoint)")
        return

    assert result is not None, "%s: no result within %d s" % (name, DURATION_S)
    assert result["t"] <= DURATION_S
    assert run["max_c"] < ABORT_C, (
        "%s: chamber peaked at %.1f C, at/above the %.0f C abort"
        % (name, run["max_c"], ABORT_C)
    )

    period = true_period(run)
    amplitude = true_amplitude(run)
    assert period is not None and amplitude is not None

    # The detector measures a 1 Hz, 5 s-held signal, so allow a little slack.
    period_error = abs(result["pu"] - period) / period
    amplitude_error = abs(result["amp"] - amplitude) / amplitude
    assert period_error < 0.10, (
        "%s: period %.1fs vs actual %.1fs (%.0f%% off)"
        % (name, result["pu"], period, period_error * 100.0)
    )
    assert amplitude_error < 0.25, (
        "%s: amplitude %.2fC vs actual %.2fC (%.0f%% off)"
        % (name, result["amp"], amplitude, amplitude_error * 100.0)
    )

    # The two formulas, independently:
    assert abs(result["ku"] - 4.0 * result["d"] / (math.pi * result["amp"])) < 1e-6
    assert abs(result["kp"] - 0.6 * result["ku"]) < 1e-6
    assert abs(result["ki"] - 1.2 * result["ku"] / result["pu"]) < 1e-6
    assert abs(result["kd"] - 0.075 * result["ku"] * result["pu"]) < 1e-3

    # What the firmware would actually store after pidClampGains().
    kp = clamp(result["kp"], 0.0, KP_MAX)
    ki = clamp(result["ki"], 0.0, KI_MAX)
    kd = clamp(result["kd"], 0.0, KD_MAX)
    assert 0.0 <= kp <= KP_MAX and 0.0 <= ki <= KI_MAX and 0.0 <= kd <= KD_MAX

    print(
        "         done t=%3.0fs phases=%d  Ku=%5.1f  Pu=%5.1fs  a=%.2fC  "
        "kp=%.1f ki=%.2f kd=%.0f%s"
        % (
            result["t"], result["phases"], result["ku"], result["pu"],
            result["amp"], kp, ki, kd,
            " (clamped)" if (kp, ki, kd) != (result["kp"], result["ki"], result["kd"]) else "",
        )
    )
    print(
        "         actual period %.1fs, actual amplitude %.2fC; "
        "symmetry=%s convergence=%s"
        % (period, amplitude, result["sym"], result["conv"])
    )


def main():
    # A spread of plausible enclosures, from a small fast one to a large slow
    # one. All start at the setpoint, as they do after the preheat/settle phase.
    check_case("fast", (50.0, 28.0, 180.0, 10.0))
    check_case("medium", (60.0, 25.0, 300.0, 20.0))
    check_case("slow", (70.0, 22.0, 420.0, 40.0))

    # A cooling-only loop cannot oscillate around a setpoint the bed can never
    # reach: with no cooling the chamber settles BELOW 35 C, so there is no
    # crossing and the regulator's 20-minute timeout is the correct outcome.
    check_case("no-osc", (32.0, 24.0, 300.0, 10.0), expect_result=False)

    print("\nAll PID calibration design checks passed.")


if __name__ == "__main__":
    main()
