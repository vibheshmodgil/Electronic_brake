# Calibration and commissioning

Do these in order. Each step assumes the previous one passed.

> **Keep the shaft mechanically disconnected** from anything that matters until
> step 7. Steps 1–6 exist precisely to find the wiring mistakes that would make
> a connected machine dangerous.

Have the serial monitor open at **115200 baud**, or run the dashboard:

```bash
python tools/brake_dashboard.py --port COM3
```

---

## Step 1 — Relay polarity

**Goal:** confirm `ACTIVE_LOW` matches the module. Getting this wrong inverts the
entire brake logic.

Brake coil **not yet connected**. Power the Arduino only.

1. At reset the sketch calls `setRelay(false)`. Listen and look: the relay should
   be **de-energised** — no click into the active position, module LED in its
   idle state.
2. Twist the throttle past the threshold. The relay should now **click and
   energise**, and the log should show `Relay = ON | Brake = OFF`.
3. Release. After the timer expires it should click back off.

| What you observe | Meaning | Action |
|---|---|---|
| Off at reset, on with throttle | Correct | Continue |
| **On at reset**, off with throttle | Inverted | Set `ACTIVE_LOW = false`, re-flash, repeat |
| Never clicks | Wiring, or module needs its own supply | See [WIRING.md](WIRING.md#3-relay-module-to-arduino) |
| Clicks briefly on every reset | `D7` floating during boot | Fit the 10 kΩ pull-up |

Confirm with a meter across COM–NO: open at reset, closed under throttle.

---

## Step 2 — Fail-safe direction

**Goal:** prove that losing power applies the brake. This is the safety argument
of the whole project; do not take it on trust.

Now connect the brake coil, still with the shaft disconnected.

1. Throttle past the threshold. Brake **releases** — shaft turns by hand.
2. **Pull the Arduino's power** while the brake is released.
3. The brake must **apply immediately**. The shaft must lock.

If the shaft stays free with the power off, **stop**. Either the brake is
power-on-engaged (wrong type of brake), or the coil is on the NC contact instead
of NO. Both are correctable; neither is safe to leave.

Repeat with the brake supply pulled instead of the logic supply. Same result.

---

## Step 3 — Throttle range

**Goal:** find the real idle and full-scale voltages, and set the threshold above
idle with margin.

With the throttle connected, read the `Throttle =` column:

| Position | Reading | Typical |
|---|---|---|
| Fully released | ______ V | 0.8–0.9 V |
| Fully twisted | ______ V | 3.6–4.2 V |

Write both into the table in
[HARDWARE.md](HARDWARE.md#record-what-you-actually-fitted).

> **The likely problem.** The committed `THROTTLE_ON_V = 0.80f` sits exactly where
> many hall throttles idle. If your released reading is 0.80 V or above, the
> system arms itself at power-up with nobody touching anything — and the log
> shows `Relay = ON` immediately after `READY`.

**Set the threshold** roughly a quarter of the way up the travel, and at least
0.2 V above measured idle:

```
    THROTTLE_ON_V  ≈  idle + 0.25 × (full − idle)
```

For an idle of 0.85 V and full of 4.10 V, that is about 1.66 V. Round to
something memorable and re-flash.

**Add hysteresis if the relay chatters** with the throttle held near the
threshold:

```cpp
const float THROTTLE_ON_V  = 1.70f;
const float THROTTLE_OFF_V = 1.50f;
```

**Also check the wiring fault case:** unplug the throttle signal wire and watch
the reading. A floating `A4` drifts and can wander above the threshold. If it
does, fit a 10 kΩ pull-down from `A4` to ground so a disconnected throttle reads
a solid zero.

---

## Step 4 — Encoder signal integrity

**Goal:** confirm all four encoder lines stay inside the ADC's window.

With a meter or scope on each of PCOS, PSIN, NCOS, NSIN in turn, turn the shaft
slowly through a full revolution by hand.

| Line | Minimum | Maximum |
|---|---|---|
| PCOS | ______ V | ______ V |
| PSIN | ______ V | ______ V |
| NCOS | ______ V | ______ V |
| NSIN | ______ V | ______ V |

**Every one must stay within 0 V and 5 V.** Any excursion below 0 V is clipped by
the ADC and the computed angle is wrong over part of every revolution — while
still looking like a smooth plausible signal. Fix the biasing before continuing;
see [HARDWARE.md](HARDWARE.md#critical-requirement-the-signals-must-be-unipolar).

Also check the two amplitudes are **similar**. If the sine pair swings noticeably
more than the cosine pair, the angle speeds and slows within each cycle and the
rpm shows periodic ripple. Trim the larger channel:

```cpp
const float COS_SCALE = 1.00f;
const float SIN_SCALE = 0.92f;    // if SIN is ~8% larger
```

---

## Step 5 — `PERIODS_PER_REV`

**Goal:** get the mechanical conversion right. This is the constant most likely
to be silently wrong, and it scales every rpm reading.

**From the datasheet:** find the sin/cos cycles per revolution — it may be called
periods, lines, or signal periods per revolution. Put that number in
`PERIODS_PER_REV`.

**By measurement**, if you have no datasheet:

1. Mark the shaft.
2. Watch the raw `SIN` channel on a scope, or add a temporary line to the sketch
   that prints `angle`.
3. Turn the shaft exactly one revolution, slowly.
4. Count the complete sine periods — equivalently, the number of times the angle
   wraps from 2π back to 0. That count is `PERIODS_PER_REV`.

**Cross-check against reality.** Spin the shaft at a speed you can verify another
way — a tachometer, a strobe, or a known drive setting — and compare with the
`RPM` column:

| Symptom | Cause |
|---|---|
| Reads N times too high | `PERIODS_PER_REV` left at 1.0 for an N-cycle encoder |
| Reads N times too low | `PERIODS_PER_REV` set too high |
| Correct at low speed, wrong at high | Aliasing — past the ~2600 rpm ceiling |
| Sign inverted | `A1`/`A2` swapped, or encoder mounted the other way. Harmless to the logic; the comparison is on absolute value. |

---

## Step 6 — RPM noise floor, and `RPM_THRESHOLD`

**Goal:** set `RPM_THRESHOLD` above the standstill noise. Set it too low and the
brake never engages; too high and the brake can apply to a slowly creeping shaft.

1. Bring the shaft to a genuine standstill.
2. Watch `RPM Raw` and `RPM` for **at least 30 seconds**. The dashboard makes the
   excursions easy to see; the CSV log gives you the numbers.
3. Record the largest excursion of the **filtered** `RPM` from zero — that is your
   noise floor.

```
    RPM_THRESHOLD  ≈  2 × measured filtered noise floor
```

For a floor of ±8 rpm, 20 is a sensible threshold — which is where the committed
value comes from.

**Do this with the motor powered but stationary**, not merely switched off. Motor
drives inject noise even at zero command, and that is the condition the interlock
actually has to work in.

| Symptom | Cause | Fix |
|---|---|---|
| Brake never engages; timer keeps cancelling | Threshold below the noise floor | Raise it, or attack the noise |
| Noise grows when the motor is live | Ground loop or unshielded cable | See [WIRING.md](WIRING.md#5-grounding) |
| Noise floor above ~30 rpm | Encoder amplitude too small, or bad grounding | Fix the cause; do not just raise the threshold |

Raising `RPM_THRESHOLD` to paper over noise makes the brake willing to engage on
a genuinely moving shaft. Fix noise at its source.

---

## Step 7 — `BRAKE_DELAY_MS`

Now the mechanical settle time, which is about the machine rather than the
electronics.

Too short and the brake grabs during a momentary throttle dip mid-manoeuvre. Too
long and the machine sits unsecured after stopping. Run the intended duty cycle
and watch for `Brake timer CANCELLED after ___ ms` in the log: if legitimate
manoeuvres routinely reach 700–900 ms of a 1000 ms timer, lengthen it.

---

## Step 8 — Loaded commissioning

Only now reconnect the shaft to the machine.

1. Confirm the brake is applied before anything can move.
2. Release under throttle, at the lowest useful speed. Confirm free rotation.
3. Come to a stop. Confirm the brake applies after the timer, once.
4. Repeat with a throttle blip during the timer. Confirm the timer cancels and
   the brake does **not** apply.
5. Repeat the fail-safe test from step 2, loaded, at standstill.
6. Log a full duty cycle to CSV and review it:

   ```bash
   python tools/brake_dashboard.py --port COM3 --csv commissioning.csv
   ```

Keep that CSV with the project. It is the record of how the machine behaved when
it was known good, and it is what you will diff against when something changes.

---

## Final record

Copy the values you settled on into
[HARDWARE.md](HARDWARE.md#record-what-you-actually-fitted) **and** commit the
edited sketch. The constants in the repository should be the constants on the
bench.

| Constant | Committed default | Yours |
|---|---|---|
| `ACTIVE_LOW` | `true` | |
| `THROTTLE_ON_V` | `0.80f` | |
| `THROTTLE_OFF_V` | `0.80f` | |
| `PERIODS_PER_REV` | `1.0f` | |
| `COS_SCALE` / `SIN_SCALE` | `1.0f` / `1.0f` | |
| `RPM_THRESHOLD` | `20.0f` | |
| `BRAKE_DELAY_MS` | `1000` | |
| `ADC_VREF` | `5.00f` | |
