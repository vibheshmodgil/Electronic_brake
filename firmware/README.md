# Firmware

The controller. This is the only code that runs on the machine.

```
firmware/
├── platformio.ini                        PlatformIO build config (Mega)
├── ElectronicBrake/
│   └── ElectronicBrake.ino               Mega 2560 build - analogue inputs
└── EBrakeCAN/
    ├── EBrakeCAN.ino                     ESP32-S3 build - CAN inputs
    └── README.md                         its pins, CAN decoding and constants
```

**Two builds, one braking rule.** Both release the brake on throttle and
re-apply it only after throttle *and* speed have stayed low for a continuous
second. They differ in where those two numbers come from.

| | Mega build | CAN build |
|---|---|---|
| Directory | `ElectronicBrake/` | `EBrakeCAN/` |
| Board | Arduino Mega 2560 | ESP32-S3 + TJA1050 transceiver |
| Speed from | Differential sin/cos encoder on `A0`–`A3` | CAN `0x015`, signed 16-bit |
| Throttle from | Hall throttle on `A4`, in **volts** | CAN `0x0B7`, in **percent** |
| Release / apply | 0.80 V / 0.80 V — no hysteresis | 5.0 % / 4.5 % — 0.5 % hysteresis |
| Speed threshold | 20 rpm | 20 rpm |
| Settle time | 1000 ms | 1000 ms |
| Relay | `D7`, active-low | `GPIO 7`, active-low |
| Extra fail-safe | — | **CAN timeout** — 500 ms of silence applies the brake |
| Dashboard | [`tools/brake_dashboard.py`](../tools/brake_dashboard.py) | [`tools/can_brake_dashboard.py`](../tools/can_brake_dashboard.py) |

The CAN build has its own README: **[EBrakeCAN/README.md](EBrakeCAN/README.md)**.
Everything below this line is about the **Mega build**.

---

## Building

### Arduino IDE

1. Open `ElectronicBrake/ElectronicBrake.ino`.
2. **Tools → Board → Arduino AVR Boards → Arduino Mega or Mega 2560**
3. **Tools → Processor → ATmega2560 (Mega 2560)**
4. Select the port, then Upload.
5. **Tools → Serial Monitor**, set to **115200 baud**.

### PlatformIO

```bash
cd firmware
pio run                  # compile
pio run --target upload  # compile and flash
pio device monitor       # serial at 115200
```

`platformio.ini` points `src_dir` at the `ElectronicBrake/` folder, so both
toolchains build the same file. There is no duplicated copy of the source to keep
in sync.

---

## Structure of the sketch

Read in this order:

| Section | What it holds |
|---|---|
| Header comment | Pin map and the control logic, in prose |
| Pin definitions | `A0`–`A4`, `D7` |
| Configuration constants | **Everything you would want to change** |
| Message helpers | Print thresholds from their constants |
| `readADC` | Averages `ADC_SAMPLES` reads of one pin |
| `readThrottleVoltage` | Counts to volts |
| `readDifferentialEncoder` | `P − N` for both channels |
| `getEncoderAngle` | `atan2`, wrapped to 0…2π |
| `filterRPM` | 8-sample moving average |
| `updateRPM` | Angle difference, unwrap, differentiate, filter |
| `setRelay` | Applies `ACTIVE_LOW` polarity |
| `updateRelayLogic` | **The state machine** |
| `setup` | Pin modes, fail-safe initial state, banner |
| `loop` | Read, compute, decide, print |

The maths behind `updateRPM`, and the reasoning behind `updateRelayLogic`, are
derived in [../docs/CONTROL_LOGIC.md](../docs/CONTROL_LOGIC.md).

---

## Constants

All at the top of the file, each with a comment explaining what it encodes about
the hardware.

| Constant | Default | Change it when |
|---|---|---|
| `ACTIVE_LOW` | `true` | Your relay module is active-high |
| `ADC_VREF` | `5.00f` | You measured the actual 5 V rail |
| `THROTTLE_ON_V` | `0.80f` | You measured your throttle's idle voltage |
| `THROTTLE_OFF_V` | `0.80f` | You need hysteresis — set it below `THROTTLE_ON_V` |
| `RPM_THRESHOLD` | `20.0f` | You measured your encoder's noise floor |
| `BRAKE_DELAY_MS` | `1000` | The machine needs longer or shorter to settle |
| `PERIODS_PER_REV` | `1.0f` | Your encoder gives more than one cycle per revolution |
| `ADC_SAMPLES` | `4` | You need more speed range, or less noise |
| `COS_SCALE` / `SIN_SCALE` | `1.0f` | The sin and cos amplitudes differ |
| `RPM_FILTER_SIZE` | `8` | You need less lag, or less noise |
| `PRINT_INTERVAL_MS` | `200` | You want faster or slower telemetry |

How to measure each of these on real hardware:
**[../docs/CALIBRATION.md](../docs/CALIBRATION.md)**.

---

## The single-source-of-truth rule

**Never write a threshold value into a string literal.** Every serial message
prints its threshold from the constant, through the three helpers near the top:

```cpp
void printThrottleThreshold(float volts);
void printRpmThreshold();
void printBrakeDelay();
```

This is not a style preference. An earlier revision of this sketch printed
`"RPM < 10"` and `"Starting 2 second timer..."` while the constants read `20.0f`
and `1000` — and the printed text was believed over the code. The helpers make
that impossible: change a constant, and the log changes with it.

If you add a message that mentions a threshold, use a helper or add one.

---

## Design constraints worth preserving

These properties are why the controller is trustworthy. Keep them.

- **No dynamic allocation.** Fixed buffers only. Nothing to fragment or exhaust
  in a system that runs for weeks.
- **No interrupts.** The sin/cos angle is absolute within a cycle, so a missed
  sample costs one reading rather than permanent position error. That is what
  makes a plain polling loop adequate — and a plain loop has no race conditions
  to reason about.
- **`loop()` never blocks.** No `delay()`. The brake timer is a `millis()`
  comparison, so the state machine keeps running while it counts.
- **Unsigned subtraction for all timing.** `millis() - start` and
  `micros() - previous` are correct across rollover (49.7 days and 71 minutes
  respectively). Do not replace them with `>` comparisons against an absolute
  deadline.
- **The relay is set only in `setRelay()`.** One place writes `D7`, one place
  applies the `ACTIVE_LOW` polarity. Do not call `digitalWrite(RELAY_PIN, ...)`
  from anywhere else.
- **The fail-safe state is the reset state.** `setup()` calls `setRelay(false)`
  before anything else can run.

---

## Resource use

Comfortable on a Mega 2560 (256 KB flash, 8 KB SRAM). The RPM filter is the only
buffer of any size: `8 × float` = 32 bytes. Everything else is scalars, and all
constant strings are wrapped in `F()` so they stay in flash rather than SRAM.

---

## Verification status

Structurally validated — balanced, no unterminated strings or comments — but
**not compiled or hardware-tested in this repository**, because no AVR toolchain
was available where it was packaged. Compile it before flashing, and commission
it with [../docs/CALIBRATION.md](../docs/CALIBRATION.md).
