# Tools

Host-side tooling. Nothing here runs on the machine — it is all optional, for
watching and recording what the firmware is doing.

```
tools/
├── brake_dashboard.py       live dashboard + CSV logger  (Arduino Mega, analogue)
├── can_brake_dashboard.py   live dashboard + CSV logger  (ESP32-S3, CAN)
└── requirements.txt
```

**Two dashboards, one per firmware.** They are deliberately separate files
rather than one script with a `--mode` flag: the two boards publish different
signals, so a merged parser would be mostly branches. Pick the one that matches
the board on the bench.

| Firmware | Dashboard | Signal source |
|---|---|---|
| [`firmware/ElectronicBrake`](../firmware/ElectronicBrake) (Mega 2560) | `brake_dashboard.py` | Analogue: sin/cos encoder + hall throttle |
| [`firmware/EBrakeCAN`](../firmware/EBrakeCAN) (ESP32-S3) | `can_brake_dashboard.py` | CAN frames `0x015` and `0x0B7` |

---

## `brake_dashboard.py`

Reads the Arduino's serial telemetry, draws it live, and logs every sample to
CSV.

Three panels:

| Panel | Shows |
|---|---|
| **Brake banner** | The current state, large. Red = applied, green = released. |
| **Throttle** | Voltage against time, with the arm threshold marked |
| **RPM** | Filtered speed against time, with the ± threshold band |
| **Brake strip** | A dedicated ON/OFF timeline along the bottom |

Brake state is also shaded behind every trace, so a plot tells you at a glance
which regions were braked without cross-referencing anything.

Both time axes autoscale to the visible window, with hysteresis so the axis does
not twitch on every frame.

### Install

```bash
pip install -r requirements.txt
```

Needs `pyserial`, `matplotlib`, `numpy`, and a working tkinter — which ships with
python.org builds on Windows and macOS, and is `python3-tk` on Debian/Ubuntu.

### Run

```bash
# auto-detect the port
python brake_dashboard.py

# name it explicitly
python brake_dashboard.py --port COM3

# log somewhere specific, 60-second window
python brake_dashboard.py --port COM3 --csv run1.csv --window 60

# no hardware needed - synthetic data, to check the GUI works
python brake_dashboard.py --demo
```

> **Close the Arduino Serial Monitor first.** On Windows only one process may
> hold a COM port.

Auto-detect prefers ports whose description mentions *mega*, *arduino*, *ch340*
or *usb-serial*, then lists everything it found and reports its choice.

### Options

| Flag | Default | Meaning |
|---|---|---|
| `--port` | auto-detect | Serial port, e.g. `COM3` or `/dev/ttyACM0` |
| `--baud` | `115200` | Must match `Serial.begin()` in the sketch |
| `--csv` | `mega_log.csv` | Log file. Overwritten each run. |
| `--window` | `30` | Seconds of history visible |
| `--throttle-threshold` | `0.80` | Drawn as the arm line |
| `--rpm-threshold` | `20.0` | Drawn as the ± band |
| `--brake-ms` | `1000` | Full scale for the timer trace |
| `--echo` | off | Print every received line — use when parsing fails |
| `--demo` | off | Synthetic data, no serial port opened |

> **The three threshold flags are display only.** They draw the reference lines;
> they do **not** configure the Arduino, which uses its own compiled-in
> constants. If you change a threshold in the sketch, either pass the matching
> flag or edit the defaults at the top of this file — otherwise the plot shows a
> line the firmware is not actually using.

### CSV output

One row per received status line:

| Column | Meaning |
|---|---|
| `t_s` | Seconds since the first parsed line |
| `throttle_V` | Throttle voltage |
| `rpm_filt` | Filtered rpm — the value the logic acted on |
| `brake_on` | 1 = applied, 0 = released |
| `relay_on` | Inverse of `brake_on` |
| `timer_ms` | Brake timer elapsed, 0 when not counting |

Flushed once a second, so a log survives an unclean exit. Timestamps come from
the **PC's** clock at the moment a line was parsed, not from the Arduino, so
treat them as accurate to the 200 ms print interval, not better.

### How it parses

Regexes at the top of the file, matched case-insensitively against each line:

```python
RE_THROTTLE = throttle\s*=\s*(number)
RE_RPM_FILT = (?:^|\|)\s*RPM\s*=\s*(number)     # the '|' prevents matching "RPM Raw ="
RE_RELAY    = relay\s*=\s*(ON|OFF)
RE_BRAKE    = brake\s*=\s*(ON|OFF)
RE_TIMER    = timer\s*=\s*(\d+)\s*/\s*(\d+)
```

A line needs **both** throttle and RPM to count as a data row. Anything else with
no `=` in it is captured as an event. **If you change the serial output format in
the sketch, update these.**

### Architecture

A reader thread drains the serial port continuously so the OS buffer never backs
up, while the GUI renders at its own pace. Without that split, a slow redraw
would stall the port and the display would fall progressively further behind real
time.

### Diagnostics on exit

If nothing parsed, the script says why rather than just ending:

| Message | Meaning |
|---|---|
| `Nothing arrived` | Wrong port, or the sketch is not printing |
| `Mostly unprintable bytes -> BAUD MISMATCH` | Baud rate is wrong |
| `Lines arrived but did not match` | Format changed — it prints the first few lines |

More in [../docs/TROUBLESHOOTING.md](../docs/TROUBLESHOOTING.md#serial-and-dashboard).


---

## `can_brake_dashboard.py`

> **You may not need this one.** The ESP32 build also serves its own
> dashboard over WiFi — join `EBrake-Monitor` and open `http://192.168.4.1/`.
> That needs no PC at all, and its apply timer and CAN frame ages are reported
> by the board rather than inferred. Use the script below when you want the
> CSV log, a bigger screen, or a record of the run. See
> [../firmware/EBrakeCAN/README.md](../firmware/EBrakeCAN/README.md).

The **ESP32-S3 / TWAI-CAN** build's dashboard. It shares `brake_dashboard.py`'s
palette, autoscaling and reader-thread split, but the panels differ: this
firmware gets everything over CAN instead of from its own ADC, and it publishes
signals the Mega build has no equivalent for.

### Layout

A header row of status cards, four stacked time plots sharing one time axis, and
a live event log.

| | Shows |
|---|---|
| **BRAKE** banner | APPLIED / RELEASED, large, with the relay state and a running count of state changes |
| **TPS** card | Throttle position, and which side of the hysteresis band it is on |
| **RPM** card | Shaft speed, and whether it counts as stopped |
| **APPLY GATE** card | *Why the brake has not engaged yet* — see below |
| **CAN LINK** card | OK / FAULT / unknown |
| **TPS** plot | Throttle [%], with the hysteresis band shaded amber |
| **RPM** plot | Speed from CAN `0x015`, with the stopped deadband shaded green |
| **PEDAL** plot | Sensors S1 and S2 [mV], plus the torque request [Nm] on the right axis |
| **BRAKE** strip | An APPLIED/RELEASED timeline, with the apply timer filling it as an amber wedge |
| **EVENTS** | The firmware's own messages as they arrive, CAN faults in red |

All four plots share one time axis, so a moment can be traced vertically through
every signal. Brake-applied regions are shaded faintly red behind each trace.

### The apply gate

The most useful panel when commissioning. When the brake is released, the
question is always *what is stopping it from engaging* — and the answer is one
of exactly three things. The card shows all of them:

- two condition chips, **TPS ok/high** and **RPM ok/high**, so the blocking
  condition is named rather than inferred;
- a progress bar and a `0.6/1.0 s` readout once both conditions hold and the
  firmware's timer is counting.

When the brake is already applied the card reads **HELD**, and tells you the
throttle level that would release it.

### Two link indicators, and they are not the same thing

| Indicator | Link | Meaning |
|---|---|---|
| **LIVE / SERIAL STALE** (top right) | host ↔ board | Status lines have stopped arriving |
| **CAN LINK** card | board ↔ bus | Mirrors the firmware's own 500 ms frame timeout |

This distinction matters. A frozen plot looks identical to a quiet one, so when
no status line has arrived for 1.5 s the badge turns red, the banner goes neutral
grey and reads **STALE**, and every card dims and relabels itself *last value* —
because at that point nothing on screen is current. The CAN card goes to `?`,
since with no telemetry the state of the bus is genuinely unknown.

### What it reads

The status line from `printStatus()`:

```
RPM: -12 | S1: 820 mV | S2: 815 mV | TPS: 4.32 % | Torque: 0.00 Nm | Relay: OFF | Brake: APPLIED | Timer: RESET
```

A line needs **both** `RPM:` and `TPS:` to count as a data row. Everything else
— the startup banner, the state-change messages, the CAN-fault notice — is
captured as an event, shown in the log panel and printed again on exit.

Events and status lines are merged and **replayed in timestamp order**. Handling
all events first would let a value change from *before* a CAN fault clear that
fault immediately, because a single read can span the moment the link dropped.

### Three values the firmware does not send

All three are reconstructed host-side, and all three are marked in the UI rather
than presented as device readings.

**Timer progress.** The sketch prints only `Timer: RUNNING` or `Timer: RESET`,
never the elapsed milliseconds. The dashboard times the RUNNING stretch itself
and clamps it to `--brake-ms`. Its resolution is the sketch's 100 ms print
interval, so expect it to read up to 100 ms low. The card is headed `est.`

**CAN health.** The sketch announces `CAN signals unavailable` once on entering
the fault and says nothing at all on recovery. So the card turns red on that
message and clears again only when a decoded value changes, which can happen
only if fresh frames are arriving. Values frozen at their last decoded reading
keep it red — correct, because that is exactly what a dead link looks like.

**Pedal deviation.** `|S1 - S2|` is shown in the pedal panel because a
dual-sensor accelerator is redundant by design, and a split between the two
sensors means one is failing. **The firmware does not check this** — it decodes
both sensors but compares neither. The readout is labelled *(not checked by
firmware)* so it can never be mistaken for an interlock, and it turns amber past
200 mV purely as a prompt to go and look.

### Run

```bash
# auto-detect the port
python can_brake_dashboard.py

# name it explicitly
python can_brake_dashboard.py --port COM5

# log somewhere specific, 60-second window
python can_brake_dashboard.py --port COM5 --csv run_can.csv --window 60

# no hardware needed - synthetic data, including a periodic CAN dropout
python can_brake_dashboard.py --demo
```

Auto-detect prefers ports whose description mentions *esp32*, *cp210*, *ch340*
or *jtag*. On an ESP32-S3 using **native USB CDC** the port disappears and
reappears across a reset — if nothing arrives, reset the board, then start the
dashboard.

### Options

| Flag | Default | Meaning |
|---|---|---|
| `--port` | auto-detect | Serial port, e.g. `COM5` or `/dev/ttyACM0` |
| `--baud` | `115200` | Must match `Serial.begin()` in the sketch |
| `--csv` | `esp32_can_log.csv` | Log file. Overwritten each run. |
| `--window` | `30` | Seconds of history visible |
| `--tps-release` | `5.0` | Drawn as the release line — `TPS_RELEASE_THRESHOLD_PERCENT` |
| `--tps-apply` | `4.5` | Drawn as the apply line — `TPS_APPLY_THRESHOLD_PERCENT` |
| `--rpm-threshold` | `20` | Drawn as the ± band — `RPM_APPLY_THRESHOLD` |
| `--brake-ms` | `1000` | Full scale for the timer trace — `BRAKE_APPLY_DELAY_MS` |
| `--echo` | off | Print every received line — use when parsing fails |
| `--demo` | off | Synthetic data, no serial port opened |

> **The threshold flags are display only**, exactly as in the Mega dashboard.
> They draw reference lines; the ESP32 uses its own compiled-in constants. Change
> one in the sketch and you must pass the matching flag, or the plot shows a line
> the firmware is not using.

### CSV output

One row per received status line:

| Column | Meaning |
|---|---|
| `t_s` | Seconds since the first parsed line (PC clock) |
| `rpm` | Signed shaft speed, CAN `0x015` bytes 3–4 |
| `s1_mV`, `s2_mV` | Accelerator sensors 1 and 2 |
| `tps_pct` | Throttle position |
| `torque_Nm` | Torque request |
| `relay_on` | 1 = energised |
| `brake_applied` | 1 = applied. Inverse of `relay_on`. |
| `timer_running` | 1 while the apply timer counts |
| `timer_ms_est` | Reconstructed timer elapsed — see above, an estimate |

Flushed once a second, so a log survives an unclean exit.
