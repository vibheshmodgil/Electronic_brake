# Troubleshooting

Symptom, cause, fix. Ordered roughly by how often each turns up.

---

## Brake behaviour

### The brake never engages — the timer keeps cancelling

The log shows `Brake timer CANCELLED after 200 ms` over and over.

One of the two stop conditions is flickering. The log tells you which: watch
`RPM` and `Throttle` against their thresholds.

| Cause | Check | Fix |
|---|---|---|
| `RPM_THRESHOLD` sits inside the encoder noise floor | `RPM` at standstill swings past 20 | Measure the floor, [CALIBRATION.md step 6](CALIBRATION.md#step-6--rpm-noise-floor-and-rpm_threshold) |
| Throttle idles right at the threshold | `Throttle` hovers at 0.80 V released | Raise `THROTTLE_ON_V`, [step 3](CALIBRATION.md#step-3--throttle-range) |
| Motor drive injecting noise | Noise grows when the motor is live | Grounding, [WIRING.md](WIRING.md#5-grounding) |
| Shaft genuinely creeping | Feel it, or watch the sign of `RPM Raw` | Mechanical, not electrical |

### The brake engages while the machine is still moving

Serious. Work through it in this order:

1. **Aliasing.** Above roughly 2600 rpm the angle wraps more than half a cycle
   per sample and the reported speed becomes wrong — often low. Check the true
   speed independently. See [CONTROL_LOGIC.md](CONTROL_LOGIC.md#speed-ceiling).
2. **`PERIODS_PER_REV` too high.** Every rpm reads proportionally low, so a
   moving shaft looks stopped. [Step 5](CALIBRATION.md#step-5--periods_per_rev).
3. **Encoder disconnected or dead.** A constant angle differentiates to exactly
   zero, which reads as *stopped*. This is the design's known fail-unsafe
   direction — check the connector first, every time.
4. **`RPM_THRESHOLD` raised to paper over noise.** If someone pushed it to 100 to
   stop the cancelling, it now accepts 90 rpm as stopped.

### The brake is released at rest, and grabs under throttle — everything inverted

`ACTIVE_LOW` does not match the relay module. Set it to `false`, re-flash, and
redo [step 1](CALIBRATION.md#step-1--relay-polarity).

If `ACTIVE_LOW` is right, the coil is on the **NC** contact instead of **NO**.
See [WIRING.md](WIRING.md#4-relay-contacts-to-the-brake).

### The brake releases for a moment on every reset or upload

`D7` floats as an input between reset and `setup()`. Fit a 10 kΩ pull-up from
`D7` to `5V` for an active-low module — [WIRING.md](WIRING.md#3-relay-module-to-arduino).

### The system arms itself at power-up, untouched

The throttle's idle voltage is at or above `THROTTLE_ON_V`. Extremely common,
because 0.80 V is a very typical hall-throttle idle. Measure yours and raise the
threshold — [step 3](CALIBRATION.md#step-3--throttle-range).

### The relay chatters near the throttle threshold

No hysteresis: `THROTTLE_ON_V` equals `THROTTLE_OFF_V`. Split them, e.g. 0.85 and
0.75. [CONTROL_LOGIC.md](CONTROL_LOGIC.md#no-throttle-hysteresis).

### The relay contacts have welded shut

The brake now never applies. Almost always a missing or failed **flyback diode**
across the coil — every de-energisation arcs across the contacts and erodes them.
Replace the relay, fit the diode, and check the contact's **DC** rating (not its
AC rating) against the coil current. [WIRING.md](WIRING.md#4-relay-contacts-to-the-brake).

---

## Speed readings

### RPM is a constant multiple of the truth

`PERIODS_PER_REV`. N times too high means it is still 1.0 for an N-cycle encoder.
[Step 5](CALIBRATION.md#step-5--periods_per_rev).

### RPM is always zero, even when spinning

| Cause | Check |
|---|---|
| Encoder unpowered | 5 V and ground at the encoder connector |
| All four lines on the wrong pins | Continuity to `A0`–`A3` |
| Encoder output is digital, not analogue sin/cos | Scope one line: a sine, not a square wave |
| Both differential pairs shorted together | `PCOS − NCOS` sits at zero regardless of angle |

### RPM is noisy at standstill

Expect roughly ±8 rpm on the filtered value for a 1 Vpp encoder. More than that:

| Cause | Fix |
|---|---|
| Ground loop | One star point only, [WIRING.md](WIRING.md#5-grounding) |
| Unshielded or untwisted encoder cable | Twisted pairs, shield grounded at the Arduino end only |
| Encoder amplitude too small | Fewer ADC counts per radian, so more noise per count. Check the signal swing. |
| 5 V rail sagging when the relay switches | Separate supply for the relay module |

Raise `ADC_SAMPLES` or `RPM_FILTER_SIZE` only after the cause is addressed — both
cost sample rate or lag, and neither fixes a ground loop.

### RPM ripples periodically as the shaft turns

Unequal sin and cos amplitude. `atan2` then advances unevenly within each cycle.
Trim `COS_SCALE` / `SIN_SCALE` — [step 4](CALIBRATION.md#step-4--encoder-signal-integrity).

A smaller version of the same ripple comes from ADC channel crosstalk: the Mega
has one sample-and-hold multiplexed across all analogue inputs, so a channel
carries a little charge from the one read before it. The 4-sample average mostly
absorbs it. To reduce it further, read each channel twice and keep the second
reading, or lower the source impedance driving the ADC pins.

### RPM sign is inverted

`A1` and `A2` swapped, or the encoder is mounted the other way round. Harmless to
the brake logic — the comparison uses the absolute value — but every log and plot
is mirrored. Swap the two wires, or negate `SIN_SCALE`.

### RPM is right at low speed and nonsense at high speed

Aliasing past the ~2600 rpm ceiling. It does not saturate; it wraps.
[CONTROL_LOGIC.md](CONTROL_LOGIC.md#speed-ceiling).

---

## Throttle readings

### Throttle voltage is consistently off by a few percent

`ADC_VREF` is `5.00f`, but a USB-powered Mega's rail is commonly 4.7–5.1 V.
Measure the `5V` pin and put the measured value in. Note the `AREF` warning in
[HARDWARE.md](HARDWARE.md#2-arduino-mega-2560) before going further.

### Throttle drifts when the signal wire is unplugged

A floating ADC input picks up whatever is nearby and can wander above the
threshold — a disconnected throttle then reads as *pressed*. Fit a 10 kΩ
pull-down from `A4` to ground.

---

## Serial and dashboard

### Nothing arrives on the serial monitor

Baud must be **115200**. Check the port, and remember the Mega resets when the
port opens — the banner prints about two seconds later.

### The dashboard says "No serial ports found" or the port is busy

On Windows only one process may hold a COM port. Close the Arduino Serial
Monitor, and any other copy of the dashboard, first.

### The dashboard reports "Mostly unprintable bytes -> BAUD MISMATCH"

Exactly what it says. `--baud 115200`, matching `Serial.begin(115200)`.

### Lines arrive but nothing is plotted

The dashboard prints the first few unparsed lines when it exits. Its regexes
expect `Throttle = <number>` and `| RPM = <number>` on the same line. If you
changed `PRINT_INTERVAL_MS` output format, update the regexes at the top of
`tools/brake_dashboard.py`.

### The dashboard window opens blank and never updates

A matplotlib backend problem, not a serial one. The script forces `TkAgg` when it
finds a headless backend; if that fails, `pip install --upgrade matplotlib` and
ensure tkinter is installed. Confirm with `python brake_dashboard.py --demo`,
which needs no hardware — if the demo also stays blank, the problem is
definitely matplotlib.

### The plotted thresholds do not match the firmware

The dashboard has its own defaults (`THROTTLE_TH`, `RPM_TH`, `BRAKE_MS` at the
top of the file) and does **not** learn them from the Arduino. Either pass
`--throttle-threshold` / `--rpm-threshold` / `--brake-ms`, or edit those
constants to match the sketch.

---

## Compiling and uploading

### `avrdude: stk500v2_ReceiveMessage(): timeout`

The port is held by something else — the serial monitor or the dashboard. Close
it and retry.

### The sketch will not compile

No external libraries are required. If the Arduino IDE cannot find `Arduino.h`,
the board is wrong: **Tools → Board → Arduino Mega or Mega 2560**, processor
**ATmega2560**.

### It compiles but behaves like an older version

Check that you flashed `firmware/ElectronicBrake/ElectronicBrake.ino` and not an
old copy elsewhere. The banner printed at boot lists the active thresholds — read
them and compare against the constants you expect. That banner is generated from
the constants, so it cannot lie about what is running.
