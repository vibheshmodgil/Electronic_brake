# EBrakeCAN — ESP32-S3 / CAN controller

The CAN variant of the controller. Same braking rule as the Mega build, but
every input arrives as a CAN frame instead of an analogue voltage.

```
firmware/EBrakeCAN/
├── EBrakeCAN.ino      the controller - CAN decoding and the state machine
├── Telemetry.h        the snapshot struct shared by both sides
├── WebUI.h            WiFi access point + read-only telemetry server
└── WebPage.h          the dashboard page, served from flash
```

> **Why `Telemetry.h` exists.** The Arduino IDE hoists a generated prototype
> for every `.ino` function to the top of the file, above your own code. Any
> type or macro named in a function *signature* must therefore be declared in
> a header included near the top, or the build fails with
> `'BrakeSnapshot' was not declared in this scope`. `snapshotGet()` and
> `eventsSnapshot()` both name one, so those declarations live here. If you
> add a function that takes or returns a `BrakeSnapshot`, declare it here too.

Uses only `Arduino.h`, the ESP-IDF `driver/twai.h`, and the `WiFi` /
`WebServer` / `DNSServer` libraries that all ship with the ESP32 Arduino core
— no external libraries.

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
| `ENABLE_WEB_UI` | `1` | Set `0` to compile the WiFi dashboard out entirely |
| `WEB_AP_SSID` / `WEB_AP_PASSWORD` | `EBrake-Monitor` / `brake1234` | Always — do not ship the default password |
| `WEB_AP_CHANNEL` | `1` | The channel is congested where you are testing |
| `WEB_AP_MAX_CLIENTS` | `4` | More or fewer devices need to watch at once |
| `WEB_TASK_CORE` | `0` | Never, unless you have moved the Arduino loop |

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

## Watching it on a phone

The board raises its own WiFi access point and serves a dashboard. Join the
network, open the page, and you get the same picture as the desktop tool with
no laptop, no cable and no serial monitor.

| | |
|---|---|
| Network | `EBrake-Monitor` |
| Password | `brake1234` — **change it**, `WEB_AP_PASSWORD` in the sketch |
| Address | `http://192.168.4.1/` |

Most phones offer a "sign in to network" notification that opens the page
directly; any address you type lands there too, because unknown routes
redirect to `/`.

### One page, three shapes

The panels are a CSS grid whose column count follows the width, so the same
page suits whatever you open it on. It reflows on rotation and on a resized
browser window — there is no separate mobile page to keep in step.

| Width | Layout |
|---|---|
| under 700 px — phone | Single column, TPS and RPM side by side |
| 700–1100 px — tablet | Four columns, charts two across |
| over 1100 px — laptop | Every card on one row, charts two across |

### What it shows

The brake state, TPS, RPM, the apply gate and CAN link health, plus live
charts and the firmware's recent event messages. Two things it can do that
the serial dashboard cannot:

- **The apply timer is real, not estimated.** The board reports its own
  elapsed milliseconds, so the progress bar is the firmware's actual timer.
  The serial dashboard has to reconstruct it from RUNNING/RESET.
- **CAN frame ages are reported directly**, per message ID, against the
  firmware's own `CAN_TIMEOUT_MS`. No inference.

The thresholds drawn on the charts travel in the same JSON, taken from the
constants the algorithm compares against — so the page cannot drift from the
firmware the way a hardcoded copy would.

History is kept by the browser, not the board: the ESP32 only ever sends the
present moment. Firmware RAM stays flat however long a session runs, and the
only cost is that a device joining late starts with an empty chart.

### It cannot move the brake

There are three routes — the page, `/api/status` and `/api/events`. All three
are GET, and none of them touch the relay, the state machine or any constant.
There is deliberately no endpoint that can release the brake, and adding one
would put a WiFi client in the safety path.

### It cannot slow the brake down either

The server runs in its own FreeRTOS task pinned to **core 0**, beside the
WiFi stack Arduino already puts there. The brake algorithm is the Arduino
loop, which keeps **core 1** to itself. A stalled request, a phone that drops
mid-transfer, four phones polling at once — none of it can delay
`twai_receive()` or the apply timer. The two sides meet only at a spinlocked
copy of a ~40-byte struct.

If the access point fails to start, `webUiBegin()` says so on the serial port
and returns; the controller then behaves exactly as if the web UI had been
compiled out. `webUiBegin()` is also called **last** in `setup()`, so a WiFi
problem cannot delay the brake reaching its fail-safe state or the CAN driver
coming up.

### Turning it off

```cpp
#define ENABLE_WEB_UI 0
```

Compiles out the access point, the server and the page. **Do this for
anything past bench testing.** A radio reaches beyond the walls of the room,
the AP password is a default until you change it, and the brake does not need
WiFi to work. Range is not security.

---

## Event messages, and the rule they now follow

Every event goes through `logEvent()` / `logEventf()`, which prints to serial
**and** keeps the last eight for the web UI. `logEventf()` formats each
threshold from its constant:

```cpp
logEventf("TPS >= %.1f %%: relay ON, BRAKE RELEASED",
          (double)TPS_RELEASE_THRESHOLD_PERCENT);
```

This replaced two strings that disagreed with the code — one printed
`"TPS >= 0%"` while the constant read `5.0f`, and one hardcoded
`"below 20 RPM"` and `"one-second"`. The constants were always what ran, but
this is exactly the failure the Mega sketch already had once, where printed
text saying `"RPM < 10"` was believed over a constant reading `20.0f`.

**If you add a message that mentions a threshold, use `logEventf()` and pass
the constant.** Never type the number into the string. See
[the single-source-of-truth rule](../README.md#the-single-source-of-truth-rule).

The text the host-side dashboard keys on — `CAN signals unavailable` — is
unchanged, so `tools/can_brake_dashboard.py` still detects the fault.

---

## Verification status

**Not compiled or hardware-tested in this repository** — no ESP32 toolchain was
available where it was packaged. What *has* been checked here:

| Checked | How |
|---|---|
| Brace, paren and bracket balance across all three files | Static parse |
| `/api/status` format string vs its 19 arguments | Static parse |
| Every JSON key the page reads is one the firmware sends | Cross-check of both sides |
| Every DOM id the script touches exists in the page | Cross-check |
| The server has no write route and never names the relay | Static parse |
| The page fetches nothing external | Static parse |
| The page renders and reflows at 320/390/768/1280/1600 px | Headless Chrome against a mock board serving this exact `WebPage.h` |
| No function signature trips the Arduino prototype hoist | Static parse of every file-scope function against the types the sketch body declares |

**None of that is a compile.** Build it before flashing, and commission with
the brake mechanically disconnected.
