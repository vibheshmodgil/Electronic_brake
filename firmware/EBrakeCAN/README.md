# EBrakeCAN — ESP32-S3 / CAN controller

The CAN variant of the controller. Same braking rule as the Mega build, but
every input arrives as a CAN frame instead of an analogue voltage.

```
firmware/EBrakeCAN/
└── EBrakeCAN.ino      the entire controller
```

Single file. Uses only `Arduino.h` and the ESP-IDF `driver/twai.h` that ships
with the ESP32 Arduino core — no external libraries.

> **TWAI is CAN.** Espressif calls its controller TWAI (Two-Wire Automotive
> Interface) for trademark reasons. It is CAN 2.0 and it talks to ordinary CAN
> networks.

---

## Hardware

| Component | Role |
|---|---|
| ESP32-S3 dev board | The controller |
| TJA1050 CAN transceiver | Converts the ESP32's TX/RX logic to differential CAN H/L |
| HL-52S relay module, **active-low** | Switches the brake coil |
| Electromagnetic brake, **power-off-engaged** | The actuator. Energise to release. |
| Flyback diode across the coil | **Not optional.** See [../../docs/HARDWARE.md](../../docs/HARDWARE.md) |

### Pin map

| Signal | ESP32-S3 pin | Constant |
|---|---|---|
| CAN TX → transceiver TXD | `GPIO 5` | `CAN_TX_PIN` |
| CAN RX ← transceiver RXD | `GPIO 6` | `CAN_RX_PIN` |
| Relay IN | `GPIO 7` | `RELAY_PIN` |

The brake wiring itself — normally-open contacts, flyback diode, fuse, star
ground — is identical to the Mega build and is documented in
**[../../docs/WIRING.md](../../docs/WIRING.md)**. Everything in that file from
"Relay contacts to the brake" onward applies here unchanged.

### Three things to verify on the bench

**1. The transceiver needs its bus termination.** A CAN bus wants 120 Ω at each
physical end, and no more. Many TJA1050 breakout boards carry a fixed 120 Ω
resistor — if this node is not at the end of the bus, remove or bypass it.

**2. `GPIO 7` floats during boot.** Between reset and the first line of
`setup()` the pin is not driving. For an active-low relay, add a pull-up to the
module's logic rail so a floating input leaves the relay **off**, brake applied.
Without it the brake blips open on every reset and every flash.

**3. 3.3 V logic into a 5 V relay module.** The ESP32-S3 drives 3.3 V; HL-52S
boards are 5 V parts. Pulling `IN` low works fine, but confirm the module
reliably stays **off** when the ESP32 drives it high — a 3.3 V "high" against a
5 V rail can leave enough differential to trigger some boards. Verify with a
meter and the brake disconnected before trusting it.

---

## Building

### Arduino IDE

1. Install the **esp32** board package by Espressif (Boards Manager).
2. Open `EBrakeCAN.ino`.
3. **Tools → Board → ESP32 Arduino → ESP32S3 Dev Module**
4. Select the port, then Upload.
5. **Tools → Serial Monitor**, set to **115200 baud**.

`driver/twai.h` comes with the board package. Nothing to install separately.

> **Native USB CDC.** On an ESP32-S3 using the built-in USB, the serial port
> disappears and reappears across a reset. If the monitor shows nothing, close
> and reopen it after the board has booted. The sketch's `delay(1000)` in
> `setup()` exists to give the host time to re-enumerate before the banner
> prints.

---

## What it reads off the bus

Two message IDs, both standard (11-bit) frames with a DLC of exactly 8. Anything
else — extended frames, remote frames, wrong length, any other ID — is ignored.

All signals are **signed 16-bit, big-endian (Motorola)**, decoded by
`readSigned16BigEndian()`.

### `0x015` — ISAAC_STATE

| Bytes | Signal | Scaling |
|---|---|---|
| 3–4 | `N_RPM_SIG` — shaft speed | 1, signed |

Negative means the other direction. The algorithm uses `abs()`, so direction
does not affect braking.

### `0x0B7` — ACC_PEDALS

| Bytes | Signal | Scaling |
|---|---|---|
| 0–1 | Accelerator sensor 1 | mV |
| 2–3 | Accelerator sensor 2 | mV |
| 4–5 | Throttle position | ×0.01 → percent |
| 6–7 | Torque request | ×0.01 → Nm |

**Only TPS and RPM drive the logic.** S1, S2 and torque are decoded and printed
for diagnostics — they are not part of the braking decision.

### Listen-only mode

```cpp
TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_PIN, CAN_RX_PIN, TWAI_MODE_LISTEN_ONLY)
```

The controller **never transmits** — not even acknowledgement bits. It cannot
disturb the bus it is monitoring, which is the right default for a safety device
that only needs to observe.

> **This bites on a two-node bench setup.** A CAN transmitter needs at least one
> other node to ACK its frames. If the only other device on the bus is this
> listen-only node, the sender will never see an ACK, will retransmit, and will
> eventually go error-passive. If you are bench-testing with one sender and this
> board, either put a third node on the bus or temporarily switch to
> `TWAI_MODE_NORMAL`. **Switch it back** before connecting to a real vehicle
> network unless you intend this node to ACK.

Bit rate is **500 kbit/s** (`TWAI_TIMING_CONFIG_500KBITS`) and the filter accepts
everything. Both must match your network.

---

## The algorithm

Two states, same rule as the Mega build.

**BRAKE_APPLIED** (the power-on state):
- TPS ≥ `TPS_RELEASE_THRESHOLD_PERCENT` → relay ON, brake releases.

**BRAKE_RELEASED**:
- Both `TPS < TPS_APPLY_THRESHOLD_PERCENT` and `|RPM| < RPM_APPLY_THRESHOLD`
  start a timer.
- Both must hold **continuously** for `BRAKE_APPLY_DELAY_MS`. Any breach resets
  the timer to zero.
- Timer completes → relay OFF, brake applies.

**Above both states — the CAN interlock.** `requiredCANSignalsValid()` runs
first, every pass. It applies the brake and cancels the timer if:

- either required frame has never arrived since boot, **or**
- either frame is older than `CAN_TIMEOUT_MS`.

So a dead bus, an unplugged transceiver, a silent sender or a failed TWAI driver
all end with the brake **on**. The Mega build has no equivalent, because its
sensors are wired straight to its own ADC.

This also covers driver failure at startup: if `twai_driver_install()` or
`twai_start()` fails, `setup()` calls `commandBrakeApplied()` and returns, and
`loop()` then finds no valid signals forever after — the brake stays applied.

---

## Constants

All at the top of the file.

| Constant | Default | Change it when |
|---|---|---|
| `CAN_TX_PIN` / `CAN_RX_PIN` | `GPIO 5` / `GPIO 6` | You wired the transceiver elsewhere |
| `RELAY_PIN` | `GPIO 7` | You wired the relay elsewhere |
| `RELAY_ON` / `RELAY_OFF` | `LOW` / `HIGH` | Your relay module is active-high |
| `ISAAC_STATE_CAN_ID` | `0x015` | Your DBC uses a different RPM message |
| `ACC_PEDALS_CAN_ID` | `0x0B7` | Your DBC uses a different pedal message |
| `TPS_RELEASE_THRESHOLD_PERCENT` | `5.0f` | You measured your pedal's idle position |
| `TPS_APPLY_THRESHOLD_PERCENT` | `4.5f` | You want more or less hysteresis |
| `RPM_APPLY_THRESHOLD` | `20` | Your RPM signal is noisier at standstill |
| `BRAKE_APPLY_DELAY_MS` | `1000` | The machine needs longer or shorter to settle |
| `CAN_TIMEOUT_MS` | `500` | Your senders publish slower than 2 Hz |
| `PRINT_INTERVAL_MS` | `100` | You want faster or slower telemetry |

The bit rate is not a constant — it is the
`TWAI_TIMING_CONFIG_500KBITS()` macro in `setup()`.

### Unlike the Mega build, this one has hysteresis

5.0 % to release, 4.5 % to re-arm. A pedal resting exactly on the threshold
cannot chatter the relay. The Mega's `THROTTLE_ON_V` and `THROTTLE_OFF_V` are
both 0.80 V and can.

---

## Telemetry

Every `PRINT_INTERVAL_MS`, at 115200 baud:

```
RPM: -12 | S1: 820 mV | S2: 815 mV | TPS: 4.32 % | Torque: 0.00 Nm | Relay: OFF | Brake: APPLIED | Timer: RESET
```

Plus one-shot event lines on state changes and on entering the CAN fault.

**[`tools/can_brake_dashboard.py`](../../tools/can_brake_dashboard.py)** plots
this live and logs it to CSV. Alongside the signals above it shows an **apply
gate** — which of the two conditions is currently blocking the brake — and it
distinguishes a dead serial link from a dead CAN bus, so a frozen plot is never
mistaken for a quiet one.

Three values the sketch does not send are reconstructed host-side and labelled
as inferences: timer progress, CAN link health, and the `|S1 - S2|` pedal
spread. **The firmware does not compare S1 and S2** — that readout is a
host-side observation only. See [../../tools/README.md](../../tools/README.md).

---

## Known issues

### Stale text in two serial messages

The constants are correct; two printed strings disagree with them.

```cpp
if (latestTPSPercent >= TPS_RELEASE_THRESHOLD_PERCENT)   // 5.0
    Serial.println("TPS >= 0%: relay ON, BRAKE RELEASED");   // prints 0%
```

and:

```cpp
Serial.println("TPS low and speed below 20 RPM: one-second timer started");
```

which hardcodes `20` rather than printing `RPM_APPLY_THRESHOLD`.

**The constants are what runs.** But this is exactly the failure the Mega sketch
already had once — printed text saying `"RPM < 10"` while the constant read
`20.0f`, and the text being believed over the code. The fix there was to print
every threshold from its constant through a helper. See
[the single-source-of-truth rule](../README.md#the-single-source-of-truth-rule).
Worth doing here too.

### Verification status

**Not compiled or hardware-tested in this repository** — no ESP32 toolchain was
available where it was packaged. The host-side dashboard has been tested against
this exact output format. Compile before flashing, and commission with the brake
mechanically disconnected.
