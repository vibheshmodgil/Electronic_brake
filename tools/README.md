# Tools

Host-side tooling. Nothing here runs on the machine — it is all optional, for
watching and recording what the firmware is doing.

```
tools/
├── brake_dashboard.py     live dashboard + CSV logger
└── requirements.txt
```

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
