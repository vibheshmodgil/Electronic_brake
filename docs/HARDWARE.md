# Hardware

Every component in the system, what it does, and — importantly — **what the
firmware assumes about it**. If you swap a part, check its row here first: most
of these assumptions are baked into constants in the sketch, and a mismatch
between the part and the constant is silent.

---

## 1. Bill of materials

| # | Component | Qty | Role in this system |
|---|---|---|---|
| 1 | Arduino Mega 2560 R3 (ATmega2560, 16 MHz) | 1 | The controller. Reads all analogue inputs, runs the state machine, drives the relay. |
| 2 | Differential sin/cos rotary encoder, 4 analogue outputs | 1 | Measures shaft angle. Speed is derived from it. |
| 3 | Hall-effect throttle, 0–5 V analogue output | 1 | The operator's torque request. Only its **voltage** is used, as a release request. |
| 4 | Single-channel relay module, opto-isolated, **active-low** | 1 | Switches the brake coil supply. |
| 5 | Electromagnetic brake, **power-off-engaged** (spring-applied) | 1 | The actuator. Energise to release, de-energise to brake. |
| 6 | Flyback / freewheel diode across the brake coil | 1 | Absorbs the coil's collapse spike so it does not weld the relay contacts. **Not optional.** |
| 7 | Fuse in the brake supply line | 1 | Fault current protection on the switched side. |
| 8 | Motor and its own drive / ESC | 1 | The thing being braked. **This firmware does not command the motor.** |
| 9 | DC supply for the brake coil | 1 | Sized to the brake's rated coil voltage and current. |
| 10 | 5 V supply for the Arduino (USB or barrel jack) | 1 | Logic power. |
| 11 | PC with a USB cable | 1 | Optional. Serial monitor, live dashboard, CSV logging. |

### Record what *you* actually fitted

The firmware cannot tell what parts are on the bench. Fill this in once and the
question never has to be asked again.

| # | Component | Manufacturer / part number | Key ratings | Notes |
|---|---|---|---|---|
| 1 | Arduino Mega 2560 | | 5 V logic, 10-bit ADC | |
| 2 | Sin/cos encoder | | Vpp: ____  Cycles/rev: ____ | must match `PERIODS_PER_REV` |
| 3 | Throttle | | Idle: ____ V  Full: ____ V | measured, see CALIBRATION.md |
| 4 | Relay module | | Coil: ____ V  Contact: ____ A | active-low? ____ |
| 5 | Electromagnetic brake | | Coil: ____ V ____ A  Torque: ____ Nm | power-off-engaged? ____ |
| 6 | Flyback diode | | ____ V ____ A | |
| 7 | Fuse | | ____ A | |
| 8 | Motor + drive | | | |
| 9 | Brake supply | | ____ V ____ A | |

---

## 2. Arduino Mega 2560

**Why the Mega.** Five analogue inputs are needed simultaneously (four encoder,
one throttle) and there is headroom for more. A Uno would also fit, but the Mega
gives room to grow and a larger serial buffer for the 115200-baud telemetry.

**What the firmware assumes:**

| Assumption | Where it lives | If it is wrong |
|---|---|---|
| 10-bit ADC, values 0–1023 | division by `1023.0f` in `readThrottleVoltage()` | Throttle voltage reads wrong by a scale factor |
| 5.00 V analogue reference | `ADC_VREF = 5.00f` | Same — see the note below |
| 16 MHz clock | implied by `micros()` / `millis()` timing | RPM scales wrong |
| `analogRead()` takes about 112 µs | the ~440 Hz loop rate calculation | Speed ceiling and noise floor shift |

> **The 5 V reference is not exactly 5 V.** When the Mega runs from USB, `AVCC`
> follows the USB rail, which commonly sits between 4.7 V and 5.1 V. A throttle
> threshold of 0.80 V is therefore accurate to roughly ±4 % out of the box.
>
> If that matters, measure the actual voltage on the `5V` pin with a meter and
> put the measured figure into `ADC_VREF`. For a permanent fix, feed a precision
> reference into `AREF` and add `analogReference(EXTERNAL)` to `setup()` — but
> **read the Arduino docs first: calling `analogRead()` with an external voltage
> on `AREF` before setting `analogReference(EXTERNAL)` can short and destroy the
> internal reference.**

---

## 3. Differential sin/cos encoder

The encoder produces two analogue signals in quadrature — one shaped like a sine
of shaft angle, one like a cosine — and each is sent **differentially**, as a
signal and its inverse:

```
    PCOS  and  NCOS      the cosine pair
    PSIN  and  NSIN      the sine pair
```

The firmware recovers each channel by subtraction:

```
    COS = PCOS - NCOS
    SIN = PSIN - NSIN
```

**Why differential wiring is worth four ADC channels.** Electrical noise picked
up along the cable — and this is a system with a motor and a switching relay in
it — lands on both wires of a pair almost equally. Subtraction cancels it. The
same subtraction also removes each channel's DC offset, so the result is centred
on zero without any calibration step, and doubles the usable amplitude.

Shaft angle is then just:

```
    angle = atan2(SIN, COS)
```

which is absolute within one electrical cycle and, unlike counting quadrature
edges, cannot silently lose position.

### Critical requirement: the signals must be unipolar

**The Mega's ADC cannot read below 0 V.** Anything negative reads as 0 and is
lost, and the angle calculation quietly produces garbage over part of a
revolution.

| Encoder output style | Works directly? | What to do |
|---|---|---|
| 1 Vpp centred on ~2.5 V (each line swings 2.0–3.0 V) | **Yes** | Wire straight to A0–A3 |
| 0–5 V single-ended pair | **Yes** | Wire straight to A0–A3 |
| ±0.5 V about 0 V (true bipolar) | **No** | Add a bias network to lift the common-mode to ~2.5 V |
| RS-422 / differential line-driver digital | **No** | Wrong encoder type entirely — this firmware needs analogue sin/cos |

Check this with a meter or scope on each of the four lines before trusting a
single reading. It is the most common way to get plausible-looking but wrong
speed.

### Constants tied to the encoder

| Constant | Committed value | Meaning |
|---|---|---|
| `PERIODS_PER_REV` | `1.0f` | Electrical sin/cos cycles per **mechanical** revolution |
| `COS_SCALE`, `SIN_SCALE` | `1.0f` | Per-channel gain trim, for unequal sin/cos amplitude |
| `ADC_SAMPLES` | `4` | Reads averaged per channel per loop |

**`PERIODS_PER_REV` is the one that bites.** If your encoder produces N cycles
per revolution and this is left at 1.0, every reported rpm is **N times too
high** — and it looks completely believable. Find N in the encoder datasheet, or
measure it: turn the shaft exactly one revolution by hand and count the sine
periods. See [CALIBRATION.md](CALIBRATION.md).

`COS_SCALE` and `SIN_SCALE` exist because a sin/cos pair with unequal amplitudes
makes `atan2` return an angle that speeds up and slows down within each cycle,
which shows up as periodic ripple on the rpm. Trim the larger channel down until
the two peak amplitudes match.

---

## 4. Hall-effect throttle

A contactless twist-grip or pedal throttle with three wires: 5 V, ground, and an
analogue output labelled **OUT1** on this build. Output voltage rises with twist.

**The firmware uses the throttle only as a release request.** It is a threshold
comparison, not a proportional command — this system does not drive the motor,
so throttle *position* beyond "above or below 0.80 V" is not used.

**Typical characteristic** (confirm yours — units vary widely):

| State | Output |
|---|---|
| Released (idle) | roughly 0.8–0.9 V |
| Full twist | roughly 3.6–4.2 V |

> **Note the collision.** A great many hall throttles idle at almost exactly
> 0.80 V, which is where `THROTTLE_ON_V` currently sits. If yours does, the
> system will arm itself the instant it powers up, with nobody touching
> anything. **Measure your throttle before commissioning** and set the threshold
> comfortably above its measured idle — see [CALIBRATION.md](CALIBRATION.md).

A hall throttle is chosen over a potentiometer type deliberately: it has no
wiping contact to wear open, and a worn-open pot reads as an ambiguous floating
voltage rather than a clean zero.

---

## 5. Relay module

A single-channel opto-isolated relay board — the common blue kind — driven from
`D7`.

**Polarity.** Most of these boards are **active-low**: pulling `IN` low energises
the coil. The firmware matches that:

```cpp
const bool ACTIVE_LOW = true;
```

If your module is the opposite, set this to `false`. This is a **safety-relevant
setting** — getting it wrong inverts the entire brake logic, so the machine sits
unbraked at rest and grabs under throttle. Verify it on the bench before
connecting the brake, following [CALIBRATION.md](CALIBRATION.md).

**Boot behaviour.** Between reset and the first line of `setup()`, every Arduino
pin is a floating input. For those few milliseconds `D7` is not driving anything.
Choose the module and, if needed, add a pull resistor so that a floating `IN`
leaves the relay **off** — brake applied. For an active-low module that means a
pull-up to 5 V. Do not skip this: without it the brake can release for a moment
on every reset and every upload.

**Contact rating.** The contacts must carry the brake coil's steady current with
margin, and must be rated for **DC** switching. A relay rated 10 A at 250 VAC is
often rated far lower for DC, because a DC arc has no zero crossing to help it
extinguish. Check the DC column of the datasheet, not the AC one.

---

## 6. Electromagnetic brake

A spring-applied, electrically-released brake. A spring clamps the friction
surface; energising the coil pulls it clear.

```
    coil energised    ->  brake RELEASED  ->  shaft free
    coil de-energised ->  brake APPLIED   ->  shaft held
```

**This orientation is the entire safety argument of the project.** The powered
state is the permissive one, so every loss of power — a fault, a break, a reset,
an unplugged connector — falls back to holding the shaft. Fitting a
power-*on*-engaged brake instead would invert this and leave the machine free to
roll on any fault. Confirm which type is on the bench before wiring it.

**It is a holding brake.** It is sized to hold a stationary shaft against its
rated static torque, not to decelerate a spinning one. Repeatedly slamming it
shut at speed will glaze or burn the friction face. The firmware's
`RPM_THRESHOLD` interlock exists precisely to prevent that.

**The coil is inductive.** When the relay opens, the collapsing field generates a
large reverse voltage across the contacts. Without a **flyback diode** across the
coil this arcs, erodes the contacts and eventually welds them shut — a welded
contact means the brake never applies again. Fit the diode, reverse-biased across
the coil, physically at the coil.

---

## 7. Motor and drive

Present in the system as the source of rotation. **This firmware neither reads
nor commands the motor** — it has no connection to the ESC or motor controller.
It only observes the shaft through the encoder and decides whether to hold it.

That separation is intentional: the brake logic keeps working regardless of what
the drive does, and cannot be confused by a drive fault.

---

## 8. Power and grounding

| Rail | Feeds | Notes |
|---|---|---|
| 5 V logic | Arduino, encoder, throttle, relay module logic side | From USB or the Arduino's regulator |
| Brake supply | Brake coil, through the relay contacts | Its own supply at the coil's rated voltage |
| Motor supply | Motor drive | Separate. Keep it away from everything above. |

**All grounds must meet at exactly one point.** The Arduino, the encoder, the
throttle and the relay logic share a common ground reference; if they meet at
several points, motor current finds a path through your signal ground and the
analogue readings develop noise that looks exactly like real motion.

**Keep the relay coil supply off the Arduino's 5 V rail** if the module draws
appreciable current. The inrush drops the rail, which moves `AVCC`, which moves
every ADC reading at the exact moment the brake switches — the worst possible
time for a measurement to jump.

Detailed wiring tables are in **[WIRING.md](WIRING.md)**.

---

## 9. Summary: firmware constants and the hardware behind them

The one table to check whenever a part changes.

| Constant | Value | Hardware fact it encodes |
|---|---|---|
| `ADC_VREF` | `5.00f` | Arduino analogue reference voltage |
| `PERIODS_PER_REV` | `1.0f` | Encoder sin/cos cycles per mechanical revolution |
| `COS_SCALE` / `SIN_SCALE` | `1.0f` | Encoder channel amplitude balance |
| `ADC_SAMPLES` | `4` | Noise-versus-sample-rate trade for this encoder |
| `RPM_FILTER_SIZE` | `8` | Moving-average length; sets lag and noise floor |
| `THROTTLE_ON_V` / `THROTTLE_OFF_V` | `0.80f` | Throttle idle and release voltages |
| `RPM_THRESHOLD` | `20.0f` | Encoder noise floor at standstill |
| `BRAKE_DELAY_MS` | `1000` | Mechanical settle time of this machine |
| `ACTIVE_LOW` | `true` | Relay module drive polarity |
| `PCOS/PSIN/NCOS/NSIN/THROTTLE/RELAY` pins | `A0–A4`, `D7` | The physical wiring |
