// Runtime-adjustable PID gains for chamber temperature regulation.
//
// Shared by both boards so the CYD (and any tooling) can name the same values
// the C3 runs on, without a second copy to drift.
//
// Why the gains are tunable at all
// --------------------------------
// The controller drives a louvre (a servo, 0-100%) through the lower half of
// its output span and a fan through the upper half. The useful error range in a
// real enclosure is 0-15 C, so the proportional gain has to be sized against
// THAT band, not against the full output span. Hard-coding the gains meant any
// change needed a recompile and a flash - and on this board a USB flash costs
// ~40 s (no -z compression, see EnclosureFanControl/platformio.ini), so the
// practical effect was that the gains were never tuned at all.
//
// The compiled-in values below are DEFAULTS, not the only source: they seed a
// blank NVS and back the "restore defaults" path. The live values are readable
// and writable at runtime over the enclosure API.
//
// Sizing the defaults
// -------------------
// Output is kp*error + ki*integral + kd*derivative, mapped so 0-100 is the
// louvre and 100-200 is the fan. At kp 6.0 the louvre spans its full travel
// across roughly a 16.7 C error band, which matches a real chamber excursion.
// ki 0.1 gives Ti = kp/ki = 60 s, comfortably above the ~5 s sample period (see
// the loop-rate note in temperature_regulation.cpp). kd defaults to 0 because
// the controller samples on a zero-order hold from a 5 s HTTP poll, where a
// derivative term amplifies sample-to-sample sensor noise rather than adding
// damping - it is left implemented and tunable so it can be experimented with.
//
// These numbers are reasoned from the actuator ranges, NOT measured against the
// real enclosure. Treat them as a starting point; a step test (louvre held at a
// fixed %) is what confirms them.
#ifndef ENCLOSURE_PID_H
#define ENCLOSURE_PID_H

#include <Arduino.h>

// Compiled-in defaults. Seed a blank NVS and back "restore defaults".
#define PID_KP_DEFAULT 6.0f
#define PID_KI_DEFAULT 0.1f
#define PID_KD_DEFAULT 0.0f

// Accepted range for a value arriving at runtime (API, NVS, ESP-NOW). These are
// deliberately wide, because the point of the feature is to let someone explore
// a bad tuning rather than to prevent it - but they are finite, so a typo cannot
// wedge the regulator into a state it never recovers from.
//
// Safety note: these gains drive COOLING only. The worst a large kp can do is
// slam the louvre open; the worst a large ki can do is hold it open once the
// integral winds. Neither can defeat the emergency path, which bypasses the PID
// entirely and forces full louvre + full fan at TEMP_EMERGENCY_TRIGGER.
#define PID_KP_MIN 0.0f
#define PID_KP_MAX 50.0f
#define PID_KI_MIN 0.0f
#define PID_KI_MAX 5.0f
#define PID_KD_MIN 0.0f
#define PID_KD_MAX 50.0f

// Ceiling on the integral term's CONTRIBUTION, in output units (the same 0-200
// span the controller output is mapped onto).
//
// The controller clamps `integral` itself, but that clamp is in integral units
// (error-seconds), so the maximum output contribution would otherwise scale with
// ki - a tunable that would silently move the windup bound. Clamping the
// contribution instead keeps the windup behaviour identical across the whole
// tunable ki range.
#define PID_I_TERM_MAX 40.0f

struct PidGains
{
    float kp;
    float ki;
    float kd;
};

// Defaults as a value, for seeding and for the "restore defaults" path.
inline PidGains pidDefaultGains()
{
    PidGains g = {PID_KP_DEFAULT, PID_KI_DEFAULT, PID_KD_DEFAULT};
    return g;
}

// Range + NaN check. Rejecting non-finite values is load-bearing, not
// defensive decoration: constrain() propagates NaN rather than replacing it, so
// one NaN entering `output` would persist in the PID's state forever and the
// actuator would never see a valid command again.
inline bool pidGainsValid(const PidGains &g)
{
    return isfinite(g.kp) && g.kp >= PID_KP_MIN && g.kp <= PID_KP_MAX &&
           isfinite(g.ki) && g.ki >= PID_KI_MIN && g.ki <= PID_KI_MAX &&
           isfinite(g.kd) && g.kd >= PID_KD_MIN && g.kd <= PID_KD_MAX;
}

// Per-field clamp, for values read back from storage where rejecting the whole
// set would mean discarding an otherwise usable tuning. Not a substitute for
// pidGainsValid() on the write path - that path rejects rather than repairs.
inline float pidClampKp(float v) { return isfinite(v) ? constrain(v, PID_KP_MIN, PID_KP_MAX) : PID_KP_DEFAULT; }
inline float pidClampKi(float v) { return isfinite(v) ? constrain(v, PID_KI_MIN, PID_KI_MAX) : PID_KI_DEFAULT; }
inline float pidClampKd(float v) { return isfinite(v) ? constrain(v, PID_KD_MIN, PID_KD_MAX) : PID_KD_DEFAULT; }

inline void pidClampGains(PidGains &g)
{
    g.kp = pidClampKp(g.kp);
    g.ki = pidClampKi(g.ki);
    g.kd = pidClampKd(g.kd);
}

#endif // ENCLOSURE_PID_H