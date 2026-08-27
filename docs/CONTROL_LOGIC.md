# Control logic

What the firmware does, why it does it that way, and the maths behind the speed
measurement. This is the reference for anyone changing the behaviour.

---

## 1. The state machine

Two states. That is the whole controller.

```
                         power-up
                            |
                            v
              +-----------------------------+
              |        BRAKE APPLIED        |
              |                             |
              |   relay  OFF                |
              |   brake  ON  (spring)       |   <-- fail-safe resting state
              |   shaft  held               |
              +-----------------------------+
                   |                    ^
                   |                    |
   throttle >= THROTTLE_ON_V            |  both stop conditions held
                   |                    |  continuously for BRAKE_DELAY_MS
                   v                    |
              +-----------------------------+
              |       BRAKE RELEASED        |
              |                             |
              |   relay  ON                 |
              |   brake  OFF                |
              |   shaft  free               |
              +-----------------------------+
                            |  ^
                            |  |  any breach cancels and
              stop conditions  |  restarts the wait from zero
              first satisfied  |
                            v  |
                   [ timer running, still released ]
```

### Release: one condition

```
    throttle >= THROTTLE_ON_V   ->   release
```

Immediate. No delay, no debounce beyond the four averaged ADC samples. The
operator asked for torque; making them wait would be worse than useless.

The speed is deliberately **not** consulted here. Any state in which the machine
is already moving is a state in which releasing the brake is the safe answer.

### Apply: two conditions, held

```
    throttle < THROTTLE_OFF_V
      AND
    |rpm|    < RPM_THRESHOLD
      held continuously for BRAKE_DELAY_MS
                               ->   apply
```

**Both** must be true. **Continuously.** The instant either fails, the timer is
cancelled and the next attempt starts a fresh full `BRAKE_DELAY_MS`, not a
resumption of the old one:

```cpp
if (lowCondition) {
    if (!brakeTimerRunning) {
        brakeTimerRunning = true;
        brakeTimerStart   = millis();     // fresh start, every time
    }
    if (millis() - brakeTimerStart >= BRAKE_DELAY_MS) {
        setRelay(false);                  // brake applies
    }
} else {
    brakeTimerRunning = false;            // any breach cancels
}
```

### Why each piece is there

| Condition | What it prevents |
|---|---|
| Throttle low | Braking against an operator who is still asking for torque |
| Speed low | Slamming a **holding** brake onto a spinning shaft, glazing the friction face |
| Held for a full second | A brake grab on a momentary throttle dip mid-manoeuvre |
| Timer restarts from zero | A slow creep from accumulating enough scattered quiet moments to trigger a brake |

### `\|rpm\|`, not `rpm`

The comparison uses `fabsf(rpm)`, so direction of rotation is irrelevant. A shaft
rolling backwards down a slope at 50 rpm is just as much "moving" as one going
forwards, and the brake correctly stays off. Sign is preserved in the serial log
for diagnosis, but never used for a decision.

---

## 2. Timing

| Quantity | Value | Where it comes from |
|---|---|---|
| Loop period | ~2.24 ms | 20 `analogRead()` calls at ~112 µs |
| Loop rate | ~440 Hz | 1 / loop period |
| Filter window | 8 samples ≈ 18 ms | `RPM_FILTER_SIZE` |
| Filter lag | ~8 ms | (8−1)/2 samples of group delay |
| Brake wait | 1000 ms | `BRAKE_DELAY_MS` |
| Status print | every 200 ms | `PRINT_INTERVAL_MS` |

The control decision runs at the full ~440 Hz. Only the printing is throttled to
200 ms, so the serial output is a sampled *view* of the state, not its rate — a
transition can be decided and executed between two printed lines.

**`millis()` rollover** occurs after about 49.7 days of continuous running. The
timer uses subtraction (`millis() - brakeTimerStart`) on unsigned arithmetic,
which is correct across the wrap. The same is true of `micros()` in the RPM
calculation, which wraps every ~71 minutes. Neither needs special handling.

---

## 3. Measuring speed from sin/cos

### Step 1 — recover the two channels

```
    COS = PCOS - NCOS
    SIN = PSIN - NSIN
```

Each in raw ADC counts. The subtraction cancels the common-mode noise the pair
picked up together, removes each channel's DC offset, and doubles the amplitude.
No offset calibration step is needed because the difference is naturally centred
on zero. Each result is then multiplied by `COS_SCALE` / `SIN_SCALE`, which exist
only to correct unequal channel amplitudes.

### Step 2 — angle

```
    angle = atan2(SIN, COS)          gives -PI .. +PI
    if (angle < 0) angle += 2*PI     shifted to 0 .. 2*PI
```

`atan2` uses the ratio of the two channels, so it is **immune to amplitude
drift**: if both channels weaken together — temperature, supply sag, a dirty
disc — the angle stays correct. That is the central advantage of sin/cos over
threshold-based encoders.

It also means the angle is **absolute within one electrical cycle**, so there is
no count to lose. A missed sample costs one reading, not permanent position
error, which is why an interrupt-free polling loop is adequate here.

### Step 3 — unwrap the discontinuity

The angle jumps from 2π to 0 once per cycle. Left alone, that single sample
reports an enormous negative speed. The fix:

```cpp
if      (deltaAngle >  PI) deltaAngle -= TWO_PI_F;
else if (deltaAngle < -PI) deltaAngle += TWO_PI_F;
```

Read as: *of the two ways the shaft could have got from the previous angle to
this one, assume the shorter.*

**This assumption sets the speed ceiling.** It holds only while the shaft turns
less than half an electrical cycle between samples. Beyond that the shorter path
is the wrong one, and the reported speed is not merely clipped — it is wrong,
often with the wrong sign. See the ceiling in section 5.

### Step 4 — angle to rpm

```
    dt          = (now - previousTime) / 1e6          seconds
    omega_elec  = deltaAngle / dt                     electrical rad/s
    omega_mech  = omega_elec / PERIODS_PER_REV        mechanical rad/s
    rpm         = omega_mech * 60 / (2*PI)
```

`PERIODS_PER_REV` is where an encoder that gives N cycles per revolution is
converted to mechanical speed. **Leaving it at 1.0 for an N-cycle encoder makes
every rpm N times too large**, and the result looks entirely believable. This is
the single most consequential constant in the file.

### Step 5 — filter

An 8-sample moving average:

```
    rpm_filtered = mean of the last 8 raw values
```

Differentiation amplifies noise — each raw rpm is a difference of two noisy
angles divided by a small `dt` — so raw rpm is jumpy. Averaging 8 samples reduces
that noise by roughly √8 ≈ 2.8× at the cost of ~8 ms of lag, which is
insignificant against a 1000 ms brake timer.

**The control logic uses the filtered value; the serial log prints both.** When
diagnosing, `RPM Raw` shows the true noise and `RPM` shows what the decision
actually saw.

The first sample after boot is discarded — with no previous angle there is no
valid difference — and both rpm values start at zero.

---

## 4. Where the numbers came from

### `RPM_THRESHOLD = 20`

Set by the encoder's noise floor at standstill, not by any mechanical
requirement.

For a 1 Vpp encoder centred at 2.5 V, each channel swings about ±102 ADC counts,
so the differential amplitude is about ±205 counts. One count of ADC noise is
then roughly 1/205 rad ≈ 0.28° of angle error. Differentiated over dt ≈ 2.24 ms
that is about 2.2 rad/s ≈ 21 rpm of noise on the raw value, falling to about
7–8 rpm after the 8-sample average.

**A threshold of 10 rpm would sit inside that noise band.** The stop condition
would flicker, the timer would cancel constantly, and the brake would never
engage. 20 rpm clears the floor with margin.

Measure your own floor — [CALIBRATION.md](CALIBRATION.md) — rather than inheriting
this number. It scales with encoder amplitude, cable quality and grounding.

### `BRAKE_DELAY_MS = 1000`

A settle time. Long enough that a brief throttle dip during a manoeuvre does not
trigger a grab; short enough that the machine is secured promptly once genuinely
stopped. Adjust to the machine, not to the encoder.

### `THROTTLE_ON_V = THROTTLE_OFF_V = 0.80`

Just above the idle output of a typical hall throttle. **Verify it against yours**
— many idle at almost exactly 0.80 V, which would arm the system at power-up with
nobody touching the throttle.

---

## 5. Limits of the design

### Speed ceiling

The unwrap in step 3 needs less than half an electrical cycle per sample:

```
    hard limit  = 1 / (2 * 0.00224 s)  ~ 223 cycles/s  ~ 13,400 rpm   (aliases)
    practical   = 1 / (10 * 0.00224 s) ~  44 cycles/s  ~  2,600 rpm   (usable)
```

at `PERIODS_PER_REV = 1.0`; divide by N for an N-cycle encoder. The practical
figure allows about ten samples per cycle, which is what a clean `atan2`
trajectory needs.

**Past the hard limit the reading does not saturate — it aliases**, and reports a
plausible low speed for a fast shaft. That is dangerous in exactly the wrong
direction: the interlock could see "stopped" while the machine spins. If your
machine can exceed the ceiling, reduce `ADC_SAMPLES`, drop the throttle read to
every Nth loop, or use the ADC in free-running mode — do not simply hope.

### No throttle hysteresis

`THROTTLE_ON_V` and `THROTTLE_OFF_V` are equal, so there is no dead band. A
throttle parked exactly on the threshold, with ADC noise pushing it either side,
can chatter the relay. If you see it, split them:

```cpp
const float THROTTLE_ON_V  = 0.85f;   // arm here
const float THROTTLE_OFF_V = 0.75f;   // count as released here
```

The 0.10 V gap must exceed the ADC noise on that channel, which is a count or two
— roughly 5–10 mV — so 0.10 V is generous.

### Not a functional-safety system

No redundant channel, no encoder plausibility check, no watchdog, no feedback
that the relay contacts actually moved. A disconnected encoder reads a constant
angle, which the firmware interprets as **stopped** — a fail-*unsafe* direction
for that one fault. Mitigations, none of them implemented here, would include a
watchdog timer, a signal-amplitude check (`SIN² + COS²` should be roughly
constant, and collapses if the encoder is unplugged), and a relay contact
feedback input.

Worth stating plainly: **the amplitude check is the cheapest real improvement
available.** `SIN² + COS²` is nearly constant for a healthy encoder at any angle,
and drops to near zero if the cable is pulled — one comparison would turn the
most likely sensor failure from fail-unsafe into a detected fault.

---

## 6. Serial output

A status line every 200 ms:

```
Throttle = 0.812 V | RPM Raw = 143.20 | RPM = 138.44 | Relay = ON | Brake = OFF
```

with the timer appended while it counts:

```
... | Relay = ON | Brake = OFF | TIMER = 350/1000 ms
```

| Field | Meaning |
|---|---|
| `Throttle` | Volts on `A4`, average of 4 ADC samples |
| `RPM Raw` | Unfiltered speed. Signed. Shows the true noise. |
| `RPM` | 8-sample moving average. **This is what the logic uses.** |
| `Relay` | Coil state |
| `Brake` | Always the inverse of `Relay` |
| `TIMER` | Elapsed / required, present only while counting |

State changes print their own banner.

**Every threshold in every message is printed from its constant.** Change
`RPM_THRESHOLD` and the log follows. This is deliberate: an earlier version of
this sketch had messages hard-coded to say "RPM < 10" and "2 second timer" while
the constants read `20.0f` and `1000`, and the text was believed over the code.
