# Electronic Brake

Fail-safe electronic parking brake for a motor-driven shaft, built on an
**Arduino Mega 2560**.

The controller watches a **hall-effect throttle** and the **speed of the shaft**,
measured from a **4-channel differential sin/cos encoder**. It releases an
**electromagnetic brake** through a relay when the operator asks for torque, and
re-applies it only once the machine has genuinely come to rest and stayed at
rest.

The brake is wired **power-off-engaged**. Every failure the system can suffer — a
dead Arduino, a snapped wire, a lost supply, a blown fuse — ends in the same
state: **the brake grabs**. There is no failure mode in which the machine is left
free to roll.

---

## Contents

| Path | What it is |
|---|---|
| [`firmware/ElectronicBrake/ElectronicBrake.ino`](firmware/ElectronicBrake/ElectronicBrake.ino) | The controller. The only thing that runs on the machine. |
| [`firmware/platformio.ini`](firmware/platformio.ini) | PlatformIO build config. Arduino IDE also works — see [firmware/README.md](firmware/README.md). |
| [`tools/brake_dashboard.py`](tools/brake_dashboard.py) | Live PC dashboard and CSV logger over USB serial |
| [`docs/HARDWARE.md`](docs/HARDWARE.md) | **Every component used**, what it does, and what the firmware assumes about it |
| [`docs/WIRING.md`](docs/WIRING.md) | Pin-by-pin connection tables, power and grounding |
| [`docs/CONTROL_LOGIC.md`](docs/CONTROL_LOGIC.md) | The state machine and the RPM maths, derived in full |
| [`docs/CALIBRATION.md`](docs/CALIBRATION.md) | How to measure and set the three thresholds on real hardware |
| [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) | Symptom, cause, fix |

---

## How it behaves, in one paragraph

At power-up the relay is **off** and the brake is **applied**. When the throttle
rises to `THROTTLE_ON_V` the relay energises, the brake releases, and the machine
is free to move. It stays free — the throttle can be let go, the shaft can coast
— until **both** the throttle is below `THROTTLE_OFF_V` **and** the shaft speed
is below `RPM_THRESHOLD`, and both stay that way continuously for
`BRAKE_DELAY_MS`. Only then does the relay drop out and the brake apply. Any
throttle blip or any residual rotation during that wait cancels the timer and
restarts the wait from zero.

The full state machine, including every transition and its timing, is in
**[docs/CONTROL_LOGIC.md](docs/CONTROL_LOGIC.md)**.

---

## The three numbers that define the behaviour

These live at the top of the sketch. They are the **single source of truth**:
every serial message is printed from them, so the log text cannot drift away from
what the code actually does.

| Constant | Value as committed | Meaning |
|---|---|---|
| `THROTTLE_ON_V` / `THROTTLE_OFF_V` | **0.80 V** | Throttle voltage that arms the system / counts as released |
| `RPM_THRESHOLD` | **20 rpm** | Below this absolute rpm the shaft counts as stopped |
| `BRAKE_DELAY_MS` | **1000 ms** | How long both stop conditions must hold before braking |

> **Historical note — read this before comparing against older copies.**
>
> Earlier versions of this sketch carried comments and serial messages saying
> *"RPM < 10"* and *"2 second timer"*, while the constants were already `20.0f`
> and `1000`. **The constants were what actually ran; the printed text was
> stale.** Those messages are now generated from the constants, so this class of
> confusion cannot recur. If you find a note, a screenshot or a report mentioning
> 10 rpm or a 2-second timer, it is describing the stale text, not the behaviour.

Set them for *your* hardware using **[docs/CALIBRATION.md](docs/CALIBRATION.md)**
— particularly `RPM_THRESHOLD`, which must sit above your encoder's noise floor
at standstill or the brake will never engage.

---

## Hardware at a glance

Full detail, including what the firmware assumes about each part and a table to
record your exact part numbers, is in **[docs/HARDWARE.md](docs/HARDWARE.md)**.

| # | Component | Role |
|---|---|---|
| 1 | Arduino Mega 2560 R3 | The controller. 10-bit ADC, 5 V reference. |
| 2 | 4-channel differential sin/cos encoder | Shaft angle, and from it speed. Outputs PCOS / NCOS / PSIN / NSIN. |
| 3 | Hall-effect throttle | Operator torque request, 0–5 V analogue on OUT1. |
| 4 | Single-channel relay module, **active-low** | Switches the brake coil. |
| 5 | Electromagnetic brake, **power-off-engaged** | The actuator. Spring applies it, current releases it. |
| 6 | Motor and its own drive | The thing being braked. Not commanded by this firmware. |
| 7 | PC over USB | Optional. Live dashboard and CSV logging. |

### Pin map

| Signal | Mega pin |
|---|---|
| Encoder **PCOS** | `A0` |
| Encoder **PSIN** | `A1` |
| Encoder **NCOS** | `A2` |
| Encoder **NSIN** | `A3` |
| Throttle **OUT1** | `A4` |
| Relay **IN** | `D7` |

Wiring detail, grounding and the star-ground rule are in
**[docs/WIRING.md](docs/WIRING.md)**.

---

## Getting it running

### 1. Flash the firmware

Arduino IDE — open `firmware/ElectronicBrake/ElectronicBrake.ino`, select
**Tools → Board → Arduino Mega or Mega 2560**, pick the port, upload.

PlatformIO:

```bash
cd firmware
pio run --target upload
```

No external libraries are needed. See [firmware/README.md](firmware/README.md).

### 2. Watch it work

Serial monitor at **115200 baud**, or the dashboard:

```bash
cd tools
pip install -r requirements.txt
python brake_dashboard.py --port COM3
```

Try it with no hardware attached first:

```bash
python brake_dashboard.py --demo
```

Close the Arduino Serial Monitor before starting the dashboard — on Windows only
one process may hold a COM port. See [tools/README.md](tools/README.md).

### 3. Commission it safely

**Do the first power-up with the shaft mechanically disconnected from anything
that matters.** Then follow [docs/CALIBRATION.md](docs/CALIBRATION.md) in order:
verify relay polarity, verify the fail-safe direction, measure the throttle,
measure the RPM noise floor, and only then set the thresholds.

---

## Serial output format

One status line every 200 ms:

```
Throttle = 0.812 V | RPM Raw = 143.20 | RPM = 138.44 | Relay = ON | Brake = OFF
```

and, while the brake timer is counting:

```
... | Relay = ON | Brake = OFF | TIMER = 350/1000 ms
```

State changes print a banner of their own. `brake_dashboard.py` parses these
lines; if you change the format, update the regexes at the top of that file.

---

## Known limits and design notes

These are measured or calculated properties of the design as built, not defects.
They are recorded here so nobody has to rediscover them.

- **Sampling rate is about 440 Hz.** Twenty `analogRead()` calls per loop at
  roughly 112 µs each is about 2.24 ms per iteration.
- **Usable speed ceiling is about 2600 rpm** at `PERIODS_PER_REV = 1.0`, allowing
  roughly ten samples per electrical cycle for a clean `atan2`. Above that the
  angle aliases and the reported speed becomes nonsense — it does not simply
  saturate at a maximum.
- **RPM noise floor is about 7–8 rpm** after the 8-sample moving average, for a
  1 Vpp encoder. This is why `RPM_THRESHOLD` is 20 and not 10: at 10 the
  standstill noise would keep cancelling the brake timer indefinitely.
- **Filter lag is about 8 ms** — the 8-sample moving average delays the reported
  speed by roughly 3.5 samples.
- **No throttle hysteresis.** `THROTTLE_ON_V` and `THROTTLE_OFF_V` are equal, so
  a throttle resting exactly on the threshold can chatter. Split them if you see
  it. Discussed in [docs/CONTROL_LOGIC.md](docs/CONTROL_LOGIC.md).
- **ADC channel crosstalk.** The Mega has one sample-and-hold multiplexed across
  every analogue input, so a channel can carry a little charge from the
  previously read one. The 4-sample average largely absorbs it. If the sin and
  cos pair appear to lean on each other, see
  [docs/TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md).
- **This is a parking brake, not a service brake.** It is designed to hold a
  stopped shaft, not to stop a moving one. The firmware deliberately refuses to
  apply it above `RPM_THRESHOLD`.

---

## Safety

This controls a physical brake on a machine that moves. Treat it accordingly.

- Keep the brake **power-off-engaged**. Never invert it so that holding the brake
  *off* becomes the powered state.
- Prove the fail-safe direction on the bench — pull the Arduino's power with the
  brake released and confirm it grabs — before the machine ever carries a load.
- The relay switches a real inductive coil. It needs a **flyback diode** across
  that coil and a **fuse** on the brake supply. See
  [docs/WIRING.md](docs/WIRING.md).
- This is **not** a certified functional-safety system. There is no redundant
  channel, no watchdog on the encoder, and no diagnostic coverage of the relay
  contacts. Do not rely on it as the sole protection where a failure could injure
  someone.

---

## License

MIT — see [LICENSE](LICENSE). Change it if this work needs to be closed, or if
your institution requires something else.
