# Wiring

Every connection in the system. Component selection and ratings are in
[HARDWARE.md](HARDWARE.md); this file is about what joins to what.

---

## Block diagram

```
                    +--------------------------------------------------+
                    |            ARDUINO MEGA 2560                     |
                    |                                                  |
   ENCODER          |                                                  |
   +---------+      |                                                  |
   |  PCOS   |------|--> A0  \                                         |
   |  PSIN   |------|--> A1   |  COS = PCOS - NCOS                     |
   |  NCOS   |------|--> A2   |  SIN = PSIN - NSIN                     |
   |  NSIN   |------|--> A3  /   angle = atan2(SIN, COS)               |
   +---------+      |            rpm   = d(angle)/dt                   |
                    |                                                  |
   THROTTLE         |                                                  |
   +---------+      |                                                  |
   |  OUT1   |------|--> A4      throttle voltage                      |
   +---------+      |                                                  |
                    |                                                  |
                    |    D7 --+                                        |
                    +---------|----------------------------------------+
                              |
                              v
                     +-----------------+
                     |  RELAY MODULE   |      active-low: IN low = coil on
                     |   IN  VCC  GND  |
                     |  COM   NO   NC  |
                     +--|-----|-----|--+
                        |     |
          BRAKE SUPPLY -+     +--> ELECTROMAGNETIC BRAKE COIL --> BRAKE SUPPLY -
             (fused)                    (flyback diode across it)

                     relay ON  -> coil energised -> BRAKE RELEASED
                     relay OFF -> coil dead      -> BRAKE APPLIED
```

---

## 1. Encoder to Arduino

| Encoder signal | Mega pin | Firmware constant |
|---|---|---|
| PCOS (cosine, positive) | `A0` | `PCOS_PIN` |
| PSIN (sine, positive) | `A1` | `PSIN_PIN` |
| NCOS (cosine, inverted) | `A2` | `NCOS_PIN` |
| NSIN (sine, inverted) | `A3` | `NSIN_PIN` |
| Encoder 5 V | `5V` | — |
| Encoder GND | `GND` | — |

**Note the ordering.** The pins are *not* in P, N, P, N order — both positive
lines come first (`A0`, `A1`), then both negative (`A2`, `A3`). Miswiring `A1`
and `A2` swaps sine and cosine, which produces a perfectly plausible angle that
runs **backwards**. The reported rpm will then have the wrong sign; since the
brake logic compares the absolute value, the interlock still works, but every log
and plot is mirrored.

**Cable.** Use twisted pairs — PCOS with NCOS, PSIN with NSIN — and a shield
grounded at the Arduino end only. Grounding a shield at both ends invites motor
current to flow along it.

**Before trusting anything:** confirm every one of the four lines stays between
0 V and 5 V through a full revolution. Anything that swings negative is clipped
by the ADC and the angle is wrong over part of the turn. See the unipolar
requirement in [HARDWARE.md](HARDWARE.md#critical-requirement-the-signals-must-be-unipolar).

---

## 2. Throttle to Arduino

| Throttle wire | Mega pin | Firmware constant |
|---|---|---|
| OUT1 (analogue out) | `A4` | `THROTTLE_PIN` |
| 5 V (supply) | `5V` | — |
| GND | `GND` | — |

Hall throttles are usually red = 5 V, black = GND, green or white = signal, but
**verify against your unit** — a reversed supply kills a hall sensor instantly.

If the throttle cable runs any distance, a 100 nF capacitor from `A4` to ground
at the Arduino end steadies the reading. The firmware already averages four
samples per loop, so this is a refinement, not a requirement.

---

## 3. Relay module to Arduino

| Relay module pin | Mega pin | Firmware constant |
|---|---|---|
| IN | `D7` | `RELAY_PIN` |
| VCC | `5V` (or its own 5 V — see below) | — |
| GND | `GND` | — |

**Polarity.** `ACTIVE_LOW = true` in the sketch, matching the usual opto-isolated
blue module where `IN` low energises the coil. Verify per
[CALIBRATION.md](CALIBRATION.md) — an inverted relay inverts the entire brake
logic.

**Add a pull-up on `D7`.** A 10 kΩ resistor from `D7` to `5V` holds the input in
its inactive state during the few milliseconds between reset and `setup()`, when
the pin is still a floating input. Without it the brake can release briefly on
every reset and on every upload.

**Powering the module.** If the relay coil draws enough to sag the Arduino's 5 V
rail, feed `VCC` from a separate 5 V supply and remove the board's `VCC–JD-VCC`
jumper if it has one, keeping only the grounds common. A rail that dips when the
relay switches moves `AVCC`, and every ADC reading jumps at exactly the moment
the brake changes state.

---

## 4. Relay contacts to the brake

Wire the coil through the **normally-open** contacts, so that a relay which is
off, unpowered, or removed leaves the brake applied:

| Relay contact | Connects to |
|---|---|
| COM | Brake supply **+**, through the fuse |
| NO | Brake coil **+** |
| NC | *unused — leave open* |

and brake coil **−** returns to brake supply **−**.

```
    BRAKE SUPPLY +  --[ FUSE ]--  COM
                                  NO  ----+---- BRAKE COIL + 
                                          |
                                        [ diode, cathode to + ]
                                          |
    BRAKE SUPPLY -  --------------------- +---- BRAKE COIL -
```

**Flyback diode — mandatory.** A general-purpose rectifier such as a 1N4007 for
a modest coil, reverse-biased across the coil: **cathode (banded end) to the
positive side**. Mount it at the coil, not at the relay, so the spike is
contained where it starts. Without it the contacts arc on every release, erode,
and eventually weld shut — and a welded contact means the brake never applies
again.

**Fuse.** In the supply positive, sized just above the coil's steady current.

**Using NC instead of NO is the classic mistake.** It looks equivalent and it
inverts the fail-safe: a relay that loses power would then *release* the brake.

---

## 5. Grounding

**One star point.** The Arduino ground, encoder ground, throttle ground and relay
logic ground meet at a single node — the Arduino's ground is the natural choice.

**Keep the brake and motor power grounds separate** from that star, joined to it
at one point only if they must be joined at all. Motor and brake currents are
large and switch abruptly; if any of that current can flow through a wire shared
with the encoder, it appears in the ADC readings as noise that looks exactly like
real rotation — which is precisely the signal the brake interlock depends on.

**Ground loop symptom:** rpm noise at standstill that grows when the motor runs,
or that changes when you touch the cable. If you see it, look for a second ground
path before reaching for more filtering.

---

## 6. Power-up order

1. Logic 5 V — Arduino, encoder, throttle, relay module.
2. Confirm the relay is **off** and the brake is **applied**.
3. Brake supply.
4. Motor supply.

Down in reverse. The brake supply going live before the logic is settled can
release the brake for a moment on a machine nobody is watching.

---

## 7. Pre-power checklist

Run this once, in order, before the first power-up. Steps 1–6 are done with
everything off.

- [ ] Continuity: `A0`, `A1`, `A2`, `A3` really go to PCOS, PSIN, NCOS, NSIN — in
      that order, not P/N interleaved.
- [ ] Throttle supply is 5 V and ground, not reversed.
- [ ] Brake coil is wired through **NO**, never **NC**.
- [ ] Flyback diode is fitted across the coil, banded end to positive.
- [ ] Fuse is fitted in the brake supply positive.
- [ ] All signal grounds meet at exactly one point.
- [ ] A 10 kΩ pull-up sits on `D7`.
- [ ] Shaft is mechanically disconnected from anything that matters.
- [ ] Power up logic only. Relay is off, brake is applied, LED state noted.
- [ ] Encoder lines all stay within 0–5 V through a full hand-turned revolution.
- [ ] Throttle idle voltage measured and written into
      [HARDWARE.md](HARDWARE.md#record-what-you-actually-fitted).

Then continue with [CALIBRATION.md](CALIBRATION.md).
