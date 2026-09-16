#!/usr/bin/env python3
"""
can_brake_dashboard.py - live dashboard for the ESP32-S3 / TWAI-CAN
electronic brake controller (firmware/EBrakeCAN/EBrakeCAN.ino).

Layout
------
  HEADER      brake state, TPS, RPM, the apply gate, and CAN link health
  TPS         throttle position [%], with the hysteresis band shaded
  RPM         shaft speed from CAN 0x015, with the apply deadband shaded
  PEDAL       accelerator sensors S1/S2 [mV] and the torque request [Nm]
  BRAKE       an APPLIED/RELEASED timeline, with the apply timer filling it
  EVENTS      the firmware's own messages, as they arrive

Expected serial line (printStatus() in the sketch):

    RPM: -12 | S1: 820 mV | S2: 815 mV | TPS: 4.32 % | Torque: 0.00 Nm |
    Relay: OFF | Brake: APPLIED | Timer: RESET

Two link indicators, and they mean different things
---------------------------------------------------
  SERIAL   host <-> board. Goes STALE when status lines stop arriving, so a
           frozen plot can never be mistaken for a quiet one.
  CAN      board <-> bus. Mirrors the firmware's own 500 ms frame timeout.

Three values the firmware does not send, reconstructed here
-----------------------------------------------------------
Each is marked in the UI as an estimate rather than a device reading:

  * Timer progress. The sketch prints only RUNNING/RESET, so the elapsed
    time is measured on this end from the RUNNING edge and clamped to
    --brake-ms. Its resolution is the sketch's 100 ms print interval.

  * CAN health. The sketch announces "CAN signals unavailable" once on
    entering the fault and says nothing on recovery. The CAN card turns red
    on that message and clears again as soon as any decoded value changes,
    which can only happen if fresh frames are arriving.

  * Pedal deviation. |S1 - S2| is shown because a dual-sensor pedal is
    redundant by design and a split between the two means a failing sensor.
    THE FIRMWARE DOES NOT CHECK THIS. It is a host-side observation only.

Usage
-----
  python can_brake_dashboard.py --port COM5
  python can_brake_dashboard.py --port COM5 --csv run_can.csv --window 30
  python can_brake_dashboard.py --rpm-threshold 20 --brake-ms 1000
  python can_brake_dashboard.py --demo          # fake data, no hardware

Close the Arduino Serial Monitor first - Windows gives the port to one
process only.

Requires: pyserial, matplotlib, numpy
"""

import argparse
import csv
import math
import re
import sys
import threading
import time
from collections import deque

import numpy as np
import matplotlib
import matplotlib.pyplot as plt
from matplotlib.gridspec import GridSpec
from matplotlib.patches import Patch, Rectangle

# ============================================================
# DEFAULTS - keep in sync with the sketch
# ============================================================

BAUD          = 115200
TPS_RELEASE   = 5.0       # TPS_RELEASE_THRESHOLD_PERCENT
TPS_APPLY     = 4.5       # TPS_APPLY_THRESHOLD_PERCENT
RPM_TH        = 20.0      # RPM_APPLY_THRESHOLD
BRAKE_MS      = 1000      # BRAKE_APPLY_DELAY_MS
CAN_TIMEOUT   = 500       # CAN_TIMEOUT_MS - shown, and used for the CAN card
WINDOW_S      = 30.0
FPS           = 20.0
MAX_POINTS    = 20000
CSV_FLUSH_SEC = 1.0

# The sketch prints every 100 ms. Fifteen missed prints is a dead link, not
# a slow one - at that point the traces are frozen and must be labelled so.
STALE_S       = 1.5

# Host-side plausibility hint only. The firmware does not compare S1 and S2.
PEDAL_DEV_WARN_MV = 200.0

# autoscale behaviour
TPS_MIN_SPAN  = 12.0      # %    - never zoom tighter than this
RPM_MIN_SPAN  = 40.0      # rpm
MV_MIN_SPAN   = 200.0     # mV
NM_MIN_SPAN   = 4.0       # Nm
PAD           = 0.18      # fraction of span added above and below
SHRINK_RATIO  = 2.2       # only zoom back in when the axis is this much too big

# ---- palette ----
BG      = "#0d1117"
PANEL   = "#161b22"
GRID    = "#283039"
EDGE    = "#30363d"
FG      = "#e6edf3"
MUTED   = "#8b949e"
DIM     = "#6e7681"
C_TPS   = "#4ea3ff"
C_RPM   = "#3fb950"
C_BRAKE = "#f85149"       # brake APPLIED
C_FREE  = "#3fb950"       # brake RELEASED
C_TIMER = "#d29922"
C_S1    = "#a371f7"
C_S2    = "#6a8bd6"
C_TRQ   = "#ff8f4d"
C_WARN  = "#d29922"
C_IDLE  = "#30363d"       # state not yet known
INK     = "#0d1117"       # text on a saturated banner

# ---- type scale ----
FS_TITLE = 11.5
FS_BADGE = 8.5
FS_CARD  = 8.0            # card heading
FS_VAL   = 21             # card value
FS_SUB   = 8.0            # card sub-line
FS_AXIS  = 8.5
FS_TICK  = 7.5
FS_NOTE  = 7.5            # in-plot annotation

NUM = r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?"


def rx(p):
    return re.compile(p, re.IGNORECASE)


RE_RPM    = rx(rf"RPM:\s*({NUM})")
RE_S1     = rx(rf"S1:\s*({NUM})\s*mV")
RE_S2     = rx(rf"S2:\s*({NUM})\s*mV")
RE_TPS    = rx(rf"TPS:\s*({NUM})\s*%")
RE_TORQUE = rx(rf"Torque:\s*({NUM})\s*Nm")
RE_RELAY  = rx(r"Relay:\s*(ON|OFF)")
RE_BRAKE  = rx(r"Brake:\s*(APPLIED|RELEASED)")
RE_TIMER  = rx(r"Timer:\s*(RUNNING|RESET)")

RE_CAN_LOST = rx(r"CAN signals unavailable")


def clock(seconds):
    s = int(max(0.0, seconds))
    return f"{s // 3600:02d}:{(s // 60) % 60:02d}:{s % 60:02d}"


# ============================================================
# SERIAL
# ============================================================

def pick_port(requested):
    import serial.tools.list_ports
    ports = list(serial.tools.list_ports.comports())
    if requested:
        return requested
    if not ports:
        sys.exit("No serial ports found.")

    def score(p):
        blob = f"{p.description} {p.manufacturer or ''} {p.hwid}".lower()
        return 0 if any(k in blob for k in
                        ("esp32", "esp", "cp210", "ch340", "jtag",
                         "usb serial", "usb-serial")) else 1

    ports.sort(key=score)
    print("Available ports:")
    for p in ports:
        print(f"   {p.device:12s} {p.description}")
    print(f"-> using {ports[0].device}\n")
    return ports[0].device


def open_port(port, baud):
    import serial
    try:
        return serial.Serial(port, baud, timeout=0.05)
    except serial.SerialException as e:
        if "Access is denied" in str(e) or "PermissionError" in str(e):
            sys.exit(f"\n{port} is busy - close the Arduino Serial Monitor "
                     "and any other python instance.\n")
        sys.exit(f"Could not open {port}: {e}")


class Reader(threading.Thread):
    """Drains the port continuously so the driver buffer never backs up;
    the GUI then renders at its own pace with no display lag."""

    def __init__(self, ser, echo=False):
        super().__init__(daemon=True)
        self.ser = ser
        self.echo = echo
        self.running = True
        self._buf = bytearray()
        self._data, self._events = [], []
        self._lock = threading.Lock()
        self.t0 = None
        self.n_ok = self.n_bytes = self.n_nonprint = 0
        self.first_lines = []

    def drain(self):
        with self._lock:
            d, self._data = self._data, []
            e, self._events = self._events, []
        return d, e

    def stop(self):
        self.running = False

    def run(self):
        while self.running:
            try:
                n = self.ser.in_waiting
                chunk = self.ser.read(max(1, min(n, 65536)))
            except Exception as e:
                print("serial read failed:", e)
                self.running = False
                return
            if not chunk:
                continue
            self.n_bytes += len(chunk)
            self.n_nonprint += sum(1 for b in chunk
                                   if b not in (9, 10, 13) and not 32 <= b < 127)
            self._buf += chunk
            while True:
                i = self._buf.find(b"\n")
                if i < 0:
                    if len(self._buf) > 8192:
                        self._buf.clear()
                    break
                line = bytes(self._buf[:i]).decode("utf-8", "ignore").strip()
                del self._buf[:i + 1]
                if line:
                    self._handle(line)

    def _handle(self, line):
        if len(self.first_lines) < 8:
            self.first_lines.append(line)
        if self.echo:
            print("RX:", line)

        mrpm, mtps = RE_RPM.search(line), RE_TPS.search(line)
        if not (mrpm and mtps):
            # Anything that is not a status line is an event: the CAN-fault
            # notice, the state-change messages, the startup banner.
            if len(line.strip("= ")) > 3:
                if self.t0 is None:
                    self.t0 = time.time()
                with self._lock:
                    self._events.append((time.time() - self.t0, line))
            return

        if self.t0 is None:
            self.t0 = time.time()

        ms1, ms2 = RE_S1.search(line), RE_S2.search(line)
        mtrq = RE_TORQUE.search(line)
        mrl, mbk = RE_RELAY.search(line), RE_BRAKE.search(line)
        mtm = RE_TIMER.search(line)

        relay = 1.0 if (mrl and mrl.group(1).upper() == "ON") else 0.0
        brake = (1.0 if mbk.group(1).upper() == "APPLIED" else 0.0) if mbk \
            else 1.0 - relay
        rec = {
            "t": time.time() - self.t0,
            "rpm": float(mrpm.group(1)),
            "s1": float(ms1.group(1)) if ms1 else float("nan"),
            "s2": float(ms2.group(1)) if ms2 else float("nan"),
            "tps": float(mtps.group(1)),
            "torque": float(mtrq.group(1)) if mtrq else float("nan"),
            "relay": relay,
            "brake": brake,
            "timer": 1.0 if (mtm and mtm.group(1).upper() == "RUNNING") else 0.0,
        }
        self.n_ok += 1
        with self._lock:
            self._data.append(rec)


class DemoReader(threading.Thread):
    """Synthetic source so the GUI can be verified without hardware.

    Runs the sketch's own state machine, and drops the CAN link
    periodically so the fault path is visible too."""

    def __init__(self):
        super().__init__(daemon=True)
        self.running = True
        self._data, self._events = [], []
        self._lock = threading.Lock()
        self.t0 = time.time()
        self.n_ok = self.n_bytes = self.n_nonprint = 0
        self.first_lines = []
        self.brake = 1.0          # APPLIED at start, like the sketch
        self.timer_on = False
        self.timer_t0 = 0.0
        self.can_ok = True

    def drain(self):
        with self._lock:
            d, self._data = self._data, []
            e, self._events = self._events, []
        return d, e

    def stop(self):
        self.running = False

    def run(self):
        rpm = 0.0
        tps = 0.0
        while self.running:
            t = time.time() - self.t0

            # a 4-second CAN dropout every 45 s
            can_ok = not (36.0 < (t % 45.0) < 40.0)
            if can_ok != self.can_ok and not can_ok:
                with self._lock:
                    self._events.append(
                        (t, "CAN signals unavailable: commanding relay OFF"))
            self.can_ok = can_ok

            if can_ok:
                duty = 0.5 * (1 + math.sin(t / 7.0))
                tps = 1.5 + 34.0 * max(0.0, duty - 0.45)
                rpm = 900 * max(0.0, duty - 0.45) + np.random.randn() * 5

                was = self.brake
                if self.brake > 0.5:
                    self.timer_on = False
                    if tps >= TPS_RELEASE:
                        self.brake = 0.0
                else:
                    if tps < TPS_APPLY and abs(rpm) < RPM_TH:
                        if not self.timer_on:
                            self.timer_on, self.timer_t0 = True, t
                            with self._lock:
                                self._events.append(
                                    (t, "TPS low and speed below 20 RPM: "
                                        "one-second timer started"))
                        elif (t - self.timer_t0) * 1000.0 >= BRAKE_MS:
                            self.brake, self.timer_on = 1.0, False
                    else:
                        self.timer_on = False
                if was != self.brake:
                    with self._lock:
                        self._events.append(
                            (t, "TPS >= 0%: relay ON, BRAKE RELEASED"
                                if self.brake < 0.5 else
                                "Conditions true for 1 second: "
                                "relay OFF, BRAKE APPLIED"))
            else:
                # fault: values freeze, brake commanded on, timer cancelled
                self.brake, self.timer_on = 1.0, False

            self.n_ok += 1
            with self._lock:
                self._data.append({
                    "t": t, "rpm": float(rpm),
                    "s1": 800.0 + tps * 62.0, "s2": 795.0 + tps * 61.0,
                    "tps": tps, "torque": max(0.0, tps * 1.8),
                    "relay": 1.0 - self.brake, "brake": self.brake,
                    "timer": 1.0 if self.timer_on else 0.0,
                })
            time.sleep(0.1)


# ============================================================
# AUTOSCALE
# ============================================================

def smart_ylim(ax, y, min_span, include=()):
    """Fit the axis to the visible data with padding.

    Rescales only when the data leaves the current limits, or when the
    current limits are more than SHRINK_RATIO times too large - so the
    axis follows the signal without twitching every frame.
    """
    y = y[np.isfinite(y)]
    if y.size == 0:
        return
    lo, hi = float(y.min()), float(y.max())
    for v in include:
        lo, hi = min(lo, v), max(hi, v)
    span = hi - lo
    if span < min_span:
        mid = 0.5 * (lo + hi)
        lo, hi = mid - min_span / 2, mid + min_span / 2
        span = min_span
    lo -= span * PAD
    hi += span * PAD
    cur_lo, cur_hi = ax.get_ylim()
    if lo < cur_lo or hi > cur_hi or (cur_hi - cur_lo) > (hi - lo) * SHRINK_RATIO:
        ax.set_ylim(lo, hi)


# ============================================================
# CHROME
# ============================================================

def style_axis(ax, ylabel, last_row=False):
    ax.set_facecolor(PANEL)
    ax.grid(True, color=GRID, lw=0.6, alpha=0.8)
    ax.set_axisbelow(True)
    ax.tick_params(colors=MUTED, labelsize=FS_TICK)
    for s in ax.spines.values():
        s.set_color(EDGE)
    ax.set_ylabel(ylabel, color=MUTED, fontsize=FS_AXIS)
    if not last_row:
        # one time axis for the whole stack, on the bottom panel
        ax.tick_params(labelbottom=False)


def blank_panel(ax):
    ax.set_xticks([]); ax.set_yticks([])
    ax.set_facecolor(PANEL)
    for s in ax.spines.values():
        s.set_color(EDGE)
    return ax


def corner_note(ax, text):
    """Threshold legend in the top-right corner of a plot.

    Deliberately not drawn beside the threshold lines: the release and
    apply levels are 0.5 % apart, so labels placed on them overlap each
    other and sit on top of the trace.
    """
    return ax.text(0.995, 0.94, text, transform=ax.transAxes,
                   ha="right", va="top", color=DIM, fontsize=FS_NOTE,
                   bbox=dict(boxstyle="round,pad=0.32", fc=BG, ec=EDGE,
                             lw=0.6, alpha=0.92))


def make_card(ax, heading):
    blank_panel(ax)
    ax.text(0.07, 0.79, heading, color=MUTED, fontsize=FS_CARD,
            transform=ax.transAxes)
    val = ax.text(0.07, 0.44, "--", color=DIM, fontsize=FS_VAL,
                  va="center", transform=ax.transAxes)
    sub = ax.text(0.07, 0.15, "waiting for data", color=DIM, fontsize=FS_SUB,
                  transform=ax.transAxes)
    return val, sub


# ============================================================
# MAIN
# ============================================================

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None)
    ap.add_argument("--baud", type=int, default=BAUD)
    ap.add_argument("--csv", default="esp32_can_log.csv")
    ap.add_argument("--window", type=float, default=WINDOW_S)
    ap.add_argument("--tps-release", type=float, default=TPS_RELEASE,
                    help="TPS_RELEASE_THRESHOLD_PERCENT in the sketch")
    ap.add_argument("--tps-apply", type=float, default=TPS_APPLY,
                    help="TPS_APPLY_THRESHOLD_PERCENT in the sketch")
    ap.add_argument("--rpm-threshold", type=float, default=RPM_TH)
    ap.add_argument("--brake-ms", type=int, default=BRAKE_MS)
    ap.add_argument("--echo", action="store_true")
    ap.add_argument("--demo", action="store_true",
                    help="synthetic data - check the GUI without hardware")
    args = ap.parse_args()

    REL_TH, APP_TH = args.tps_release, args.tps_apply
    RP_TH, BR_MS = args.rpm_threshold, args.brake_ms

    # ---- GUI backend sanity check ----
    if matplotlib.get_backend().lower() in ("agg", "pdf", "ps", "svg"):
        try:
            matplotlib.use("TkAgg", force=True)
        except Exception:
            sys.exit("matplotlib has no GUI backend available.\n"
                     "  pip install matplotlib --upgrade   (and tkinter)")
    print("matplotlib backend:", matplotlib.get_backend())

    if args.demo:
        reader = DemoReader()
        print("DEMO MODE - synthetic data, no serial port opened.\n")
        ser = None
    else:
        port = pick_port(args.port)
        ser = open_port(port, args.baud)
        time.sleep(1.5)                  # boards with a USB bridge reset on DTR
        ser.reset_input_buffer()
        print(f"Connected {port} @ {args.baud}   logging -> {args.csv}\n"
              "Close the window or press Ctrl-C to stop.\n")
        reader = Reader(ser, echo=args.echo)
    reader.start()

    n = MAX_POINTS
    T = deque(maxlen=n); TPS = deque(maxlen=n); RPM = deque(maxlen=n)
    S1 = deque(maxlen=n); S2 = deque(maxlen=n); TRQ = deque(maxlen=n)
    BRK = deque(maxlen=n); TMR = deque(maxlen=n)
    events = deque(maxlen=200)
    rate_window = deque(maxlen=120)      # (wall_time, n_lines) for lines/s

    csv_f = open(args.csv, "w", newline="")
    csv_w = csv.writer(csv_f)
    csv_w.writerow(["t_s", "rpm", "s1_mV", "s2_mV", "tps_pct", "torque_Nm",
                    "relay_on", "brake_applied", "timer_running",
                    "timer_ms_est"])
    last_flush = time.time()

    # ---------------- figure ----------------
    plt.rcParams.update({"figure.facecolor": BG, "savefig.facecolor": BG,
                         "text.color": FG, "font.family": "monospace"})
    plt.ion()
    fig = plt.figure(figsize=(15.0, 9.6))
    try:
        fig.canvas.manager.set_window_title(
            "ESP32-S3 CAN brake monitor")
    except Exception:
        pass
    gs = GridSpec(6, 12, figure=fig,
                  height_ratios=[1.00, 1.30, 1.30, 1.05, 0.40, 0.42],
                  hspace=0.34, wspace=0.55,
                  left=0.055, right=0.926, top=0.915, bottom=0.062)

    # ---- title bar ----
    fig.text(0.055, 0.962, "ESP32-S3  CAN BRAKE MONITOR", color=FG,
             fontsize=FS_TITLE, fontweight="bold", va="center")
    src_txt = fig.text(0.055, 0.936,
                       "demo - synthetic data" if args.demo else "",
                       color=DIM, fontsize=FS_BADGE, va="center")
    link_txt = fig.text(0.935, 0.962, "WAITING", color=DIM,
                        fontsize=FS_BADGE, ha="right", va="center",
                        fontweight="bold")
    stat_txt = fig.text(0.935, 0.936, "", color=DIM, fontsize=FS_BADGE,
                        ha="right", va="center")

    # ---- brake banner ----
    ax_ban = blank_panel(fig.add_subplot(gs[0, 0:4]))
    for s in ax_ban.spines.values():
        s.set_linewidth(1.6)
    ax_ban.set_facecolor(C_IDLE)
    ban_head = ax_ban.text(0.045, 0.80, "BRAKE", color=MUTED,
                           fontsize=FS_CARD, transform=ax_ban.transAxes)
    ban_txt = ax_ban.text(0.045, 0.45, "NO DATA", color=MUTED, fontsize=26,
                          va="center", fontweight="bold",
                          transform=ax_ban.transAxes)
    ban_sub = ax_ban.text(0.045, 0.14, "waiting for the first status line",
                          color=MUTED, fontsize=FS_SUB,
                          transform=ax_ban.transAxes)

    tps_val, tps_sub = make_card(fig.add_subplot(gs[0, 4:6]),
                                 "TPS   CAN 0x0B7")
    rpm_val, rpm_sub = make_card(fig.add_subplot(gs[0, 6:8]),
                                 "RPM   CAN 0x015")

    # ---- apply gate: the "why is it not braking yet" card ----
    ax_gate = blank_panel(fig.add_subplot(gs[0, 8:10]))
    ax_gate.text(0.07, 0.79, "APPLY GATE   est.", color=MUTED,
                 fontsize=FS_CARD, transform=ax_gate.transAxes)
    gate_val = ax_gate.text(0.07, 0.50, "--", color=DIM, fontsize=FS_VAL,
                            va="center", transform=ax_gate.transAxes)
    bar_bg = Rectangle((0.07, 0.235), 0.86, 0.085, transform=ax_gate.transAxes,
                       fc=GRID, ec="none", zorder=2)
    bar_fg = Rectangle((0.07, 0.235), 0.0, 0.085, transform=ax_gate.transAxes,
                       fc=C_TIMER, ec="none", zorder=3)
    ax_gate.add_patch(bar_bg); ax_gate.add_patch(bar_fg)
    gate_c1 = ax_gate.text(0.07, 0.10, "", color=DIM, fontsize=FS_SUB,
                           transform=ax_gate.transAxes)
    gate_c2 = ax_gate.text(0.53, 0.10, "", color=DIM, fontsize=FS_SUB,
                           transform=ax_gate.transAxes)

    can_val, can_sub = make_card(fig.add_subplot(gs[0, 10:12]),
                                 "CAN LINK   est.")

    # ---- TPS ----
    ax_tps = fig.add_subplot(gs[1, :])
    style_axis(ax_tps, "TPS  [%]")
    ax_tps.axhspan(APP_TH, REL_TH, color=C_WARN, alpha=0.20, lw=0, zorder=0)
    ax_tps.axhline(REL_TH, ls="--", lw=1.0, color=C_WARN, alpha=0.85, zorder=1)
    ax_tps.axhline(APP_TH, ls="--", lw=1.0, color=C_WARN, alpha=0.85, zorder=1)
    corner_note(ax_tps, f"release >= {REL_TH:.1f} %   "
                        f"apply < {APP_TH:.1f} %   (band = hysteresis)")
    l_tps, = ax_tps.plot([], [], lw=1.7, color=C_TPS, zorder=4)
    ax_tps.set_ylim(0, max(REL_TH * 3, TPS_MIN_SPAN))
    ax_tps.set_xlim(0, args.window)

    # ---- RPM ----
    ax_rpm = fig.add_subplot(gs[2, :], sharex=ax_tps)
    style_axis(ax_rpm, "speed  [rpm]")
    ax_rpm.axhspan(-RP_TH, RP_TH, color=C_RPM, alpha=0.13, lw=0, zorder=0)
    ax_rpm.axhline(RP_TH, ls="--", lw=1.0, color=C_RPM, alpha=0.8, zorder=1)
    ax_rpm.axhline(-RP_TH, ls="--", lw=1.0, color=C_RPM, alpha=0.8, zorder=1)
    corner_note(ax_rpm, f"band = stopped:  |rpm| < {RP_TH:.0f}")
    l_rpm, = ax_rpm.plot([], [], lw=1.7, color=C_RPM, zorder=4)
    ax_rpm.set_ylim(-RP_TH * 2, RP_TH * 2)

    # ---- pedal sensors + torque request ----
    ax_ped = fig.add_subplot(gs[3, :], sharex=ax_tps)
    style_axis(ax_ped, "pedal  [mV]")
    l_s1, = ax_ped.plot([], [], lw=1.3, color=C_S1, label="S1")
    l_s2, = ax_ped.plot([], [], lw=1.3, color=C_S2, ls="--", label="S2")
    ax_trq = ax_ped.twinx()
    ax_trq.set_facecolor("none")
    ax_trq.tick_params(colors=MUTED, labelsize=FS_TICK)
    for s in ax_trq.spines.values():
        s.set_color(EDGE)
    ax_trq.set_ylabel("torque  [Nm]", color=C_TRQ, fontsize=FS_AXIS,
                      labelpad=7)
    l_trq, = ax_trq.plot([], [], lw=1.4, color=C_TRQ, label="torque req")
    # legend above the panel: inside it, this sat on top of the traces
    ax_ped.legend(handles=[l_s1, l_s2, l_trq], loc="lower left",
                  bbox_to_anchor=(0.0, 1.01), ncol=3, frameon=False,
                  fontsize=FS_NOTE, labelcolor=MUTED,
                  handlelength=1.8, columnspacing=1.6)
    dev_note = corner_note(ax_ped, "")

    # ---- brake strip ----
    ax_st = fig.add_subplot(gs[4, :], sharex=ax_tps)
    ax_st.set_facecolor(PANEL)
    for s_ in ax_st.spines.values():
        s_.set_color(EDGE)
    ax_st.set_yticks([])                 # y is meaningless here; x is not
    ax_st.set_ylim(0, 1)
    ax_st.tick_params(colors=MUTED, labelsize=FS_TICK)
    ax_st.set_ylabel("brake", color=MUTED, fontsize=FS_AXIS)
    ax_st.set_xlabel("time since first status line  [s]", color=MUTED,
                     fontsize=FS_AXIS)
    # a colour key, because the state is otherwise carried by hue alone
    ax_st.legend(handles=[
        Patch(fc=C_BRAKE, alpha=0.55, ec="none", label="APPLIED"),
        Patch(fc=C_FREE, alpha=0.45, ec="none", label="RELEASED"),
        Patch(fc=C_TIMER, alpha=0.9, ec="none", label="apply timer")],
        loc="lower left", bbox_to_anchor=(0.0, 1.04), ncol=3, frameon=False,
        fontsize=FS_NOTE, labelcolor=MUTED, handlelength=1.4,
        handleheight=0.9, columnspacing=1.6)

    # ---- event log ----
    ax_ev = blank_panel(fig.add_subplot(gs[5, :]))
    ax_ev.text(0.006, 0.80, "FIRMWARE EVENTS", color=MUTED, fontsize=FS_CARD,
               transform=ax_ev.transAxes)
    ev_lines = [ax_ev.text(0.006, y, "", color=DIM, fontsize=FS_SUB,
                           transform=ax_ev.transAxes)
                for y in (0.50, 0.26, 0.04)]

    shading = {"tps": None, "rpm": None, "ped": None,
               "st_on": None, "st_off": None, "timer": None}

    plt.show(block=False)
    fig.canvas.draw()
    plt.pause(0.05)

    # ---------------- loop ----------------
    frame_dt = 1.0 / FPS
    next_draw = 0.0
    last = None
    brake_edges = 0
    prev_brake = None
    timer_t0 = None          # start of the current RUNNING stretch
    timer_ms = 0.0
    can_fault = False
    can_fault_t = None
    prev_vals = None
    last_rx = None           # wall clock of the most recent status line

    try:
        while plt.fignum_exists(fig.number) and reader.running:
            recs, evs = reader.drain()

            # Events and status lines must be replayed in the order the board
            # produced them. Handling all events first would let a value
            # change from BEFORE a CAN fault clear that fault immediately,
            # because one drain can span the moment the link dropped.
            stream = ([(e[0], 0, e) for e in evs] +
                      [(r["t"], 1, r) for r in recs])
            stream.sort(key=lambda item: item[0])

            for _, kind, item in stream:
                if kind == 0:
                    events.append(item)
                    if RE_CAN_LOST.search(item[1]):
                        can_fault, can_fault_t = True, item[0]
                        # never compare across the boundary: the last good
                        # value and the first frozen one legitimately differ
                        prev_vals = None
                    continue

                r = item
                # timer progress: the sketch sends RUNNING/RESET only
                if r["timer"] > 0.5:
                    if timer_t0 is None:
                        timer_t0 = r["t"]
                    timer_ms = min(BR_MS, (r["t"] - timer_t0) * 1000.0)
                else:
                    timer_t0, timer_ms = None, 0.0

                # any decoded value that moves proves fresh CAN frames
                vals = (r["rpm"], r["tps"], r["s1"], r["s2"], r["torque"])
                if can_fault and prev_vals is not None and vals != prev_vals:
                    can_fault = False
                prev_vals = vals

                T.append(r["t"]); TPS.append(r["tps"]); RPM.append(r["rpm"])
                S1.append(r["s1"]); S2.append(r["s2"]); TRQ.append(r["torque"])
                BRK.append(r["brake"]); TMR.append(timer_ms / BR_MS)
                if prev_brake is not None and r["brake"] != prev_brake:
                    brake_edges += 1
                prev_brake = r["brake"]
                csv_w.writerow([f"{r['t']:.3f}", f"{r['rpm']:.0f}",
                                f"{r['s1']:.0f}", f"{r['s2']:.0f}",
                                f"{r['tps']:.2f}", f"{r['torque']:.2f}",
                                int(r["relay"]), int(r["brake"]),
                                int(r["timer"]), f"{timer_ms:.0f}"])
            if recs:
                last = recs[-1]
                last_rx = time.time()
                rate_window.append((last_rx, len(recs)))

            if time.time() - last_flush > CSV_FLUSH_SEC:
                csv_f.flush(); last_flush = time.time()

            now = time.time()
            if now < next_draw:
                plt.pause(0.01)                  # pumps the GUI event loop
                continue
            next_draw = now + frame_dt

            # ---- link badge: host <-> board, independent of CAN ----
            stale = last_rx is None or (now - last_rx) > STALE_S
            if last_rx is None:
                link_txt.set_text("WAITING"); link_txt.set_color(C_WARN)
                stat_txt.set_text(f"{reader.n_bytes} bytes in")
            elif stale:
                link_txt.set_text("SERIAL STALE"); link_txt.set_color(C_BRAKE)
                stat_txt.set_text(f"no line for {now - last_rx:4.1f} s"
                                  f"   {reader.n_ok} total")
            else:
                while rate_window and now - rate_window[0][0] > 3.0:
                    rate_window.popleft()
                rate = (sum(c for _, c in rate_window) /
                        max(1e-3, now - rate_window[0][0])) if rate_window else 0.0
                link_txt.set_text("LIVE"); link_txt.set_color(C_FREE)
                stat_txt.set_text(f"{rate:4.1f} lines/s   {reader.n_ok} total"
                                  f"   {clock(T[-1] if T else 0)}")

            # ---- event log ----
            recent = list(events)[-3:]
            for slot, txt in zip(ev_lines, recent + [None] * (3 - len(recent))):
                if txt is None:
                    slot.set_text("")
                    continue
                et, msg = txt
                slot.set_text(f"{et:7.1f} s   {msg[:150]}")
                slot.set_color(C_BRAKE if RE_CAN_LOST.search(msg) else MUTED)

            if len(T) < 2 or last is None:
                plt.pause(0.01)
                continue

            t = np.fromiter(T, float)
            tps = np.fromiter(TPS, float)
            rpm = np.fromiter(RPM, float)
            s1 = np.fromiter(S1, float)
            s2 = np.fromiter(S2, float)
            trq = np.fromiter(TRQ, float)
            brk = np.fromiter(BRK, float)
            tmr = np.fromiter(TMR, float)

            l_tps.set_data(t, tps)
            l_rpm.set_data(t, rpm)
            l_s1.set_data(t, s1)
            l_s2.set_data(t, s2)
            l_trq.set_data(t, trq)

            x0 = max(t[0], t[-1] - args.window)
            ax_tps.set_xlim(x0, max(t[-1], x0 + 1e-3))
            vis = t >= x0

            smart_ylim(ax_tps, tps[vis], TPS_MIN_SPAN,
                       include=(0.0, REL_TH, APP_TH))
            smart_ylim(ax_rpm, rpm[vis], RPM_MIN_SPAN,
                       include=(-RP_TH, RP_TH))
            smart_ylim(ax_ped, np.concatenate((s1[vis], s2[vis])), MV_MIN_SPAN)
            smart_ylim(ax_trq, trq[vis], NM_MIN_SPAN, include=(0.0,))

            # brake shading behind every trace
            for key, ax in (("tps", ax_tps), ("rpm", ax_rpm), ("ped", ax_ped)):
                if shading[key] is not None:
                    shading[key].remove()
                shading[key] = ax.fill_between(
                    t, 0, 1, where=brk > 0.5, step="post",
                    transform=ax.get_xaxis_transform(),
                    color=C_BRAKE, alpha=0.085, lw=0, zorder=0)

            # strip: state as colour, timer as a filling wedge on top
            for key, cond, col, alpha in (
                    ("st_on", brk > 0.5, C_BRAKE, 0.55),
                    ("st_off", brk <= 0.5, C_FREE, 0.45)):
                if shading[key] is not None:
                    shading[key].remove()
                shading[key] = ax_st.fill_between(
                    t, 0, 1, where=cond, step="post",
                    color=col, alpha=alpha, lw=0, zorder=1)
            if shading["timer"] is not None:
                shading["timer"].remove()
            shading["timer"] = ax_st.fill_between(
                t, 0, tmr, step="post", color=C_TIMER, alpha=0.9, lw=0,
                zorder=2)

            braking = last["brake"] > 0.5
            state_col = C_BRAKE if braking else C_FREE

            # ---- banner ----
            if stale:
                ax_ban.set_facecolor(C_IDLE)
                ban_txt.set_text("STALE"); ban_txt.set_color(C_BRAKE)
                ban_sub.set_text(f"last state was "
                                 f"{'APPLIED' if braking else 'RELEASED'}"
                                 f" - serial has stopped")
                ban_sub.set_color(MUTED); ban_head.set_color(MUTED)
            else:
                ax_ban.set_facecolor(state_col)
                ban_txt.set_text("APPLIED" if braking else "RELEASED")
                ban_txt.set_color(INK)
                ban_sub.set_text(("relay OFF - shaft held" if braking
                                  else "relay ON - shaft free")
                                 + f"      {brake_edges} state changes")
                ban_sub.set_color(INK); ban_head.set_color(INK)

            # ---- TPS card ----
            above_rel = last["tps"] >= REL_TH
            below_app = last["tps"] < APP_TH
            tps_val.set_text(f"{last['tps']:.2f} %")
            tps_val.set_color(C_WARN if above_rel else C_TPS)
            tps_sub.set_text(
                f"at/above release {REL_TH:.1f} %" if above_rel else
                (f"below apply {APP_TH:.1f} %" if below_app else
                 f"in band {APP_TH:.1f} - {REL_TH:.1f} %"))
            tps_sub.set_color(MUTED)

            # ---- RPM card ----
            slow = abs(last["rpm"]) < RP_TH
            rpm_val.set_text(f"{last['rpm']:.0f}")
            rpm_val.set_color(C_RPM if slow else C_WARN)
            rpm_sub.set_text(f"stopped   |rpm| < {RP_TH:.0f}" if slow
                             else f"turning   |rpm| >= {RP_TH:.0f}")
            rpm_sub.set_color(MUTED)

            # ---- apply gate ----
            if braking:
                gate_val.set_text("HELD"); gate_val.set_color(MUTED)
                bar_fg.set_width(0.0)
                gate_c1.set_text(f"releases at TPS >= {REL_TH:.1f} %")
                gate_c1.set_color(DIM)
                gate_c2.set_text("")
            else:
                running = last["timer"] > 0.5
                if running:
                    gate_val.set_text(f"{timer_ms / 1000.0:.1f}/"
                                      f"{BR_MS / 1000.0:.1f} s")
                    gate_val.set_color(C_TIMER)
                    bar_fg.set_width(0.86 * (timer_ms / BR_MS))
                else:
                    gate_val.set_text("open"); gate_val.set_color(MUTED)
                    bar_fg.set_width(0.0)
                gate_c1.set_text("TPS ok" if below_app else "TPS high")
                gate_c1.set_color(C_FREE if below_app else C_WARN)
                gate_c2.set_text("RPM ok" if slow else "RPM high")
                gate_c2.set_color(C_FREE if slow else C_WARN)

            # ---- CAN card ----
            if can_fault:
                can_val.set_text("FAULT"); can_val.set_color(C_BRAKE)
                can_sub.set_text(f"lost at t = {can_fault_t:.1f} s"
                                 if can_fault_t is not None else "no frames")
            else:
                can_val.set_text("OK"); can_val.set_color(C_FREE)
                can_sub.set_text(f"firmware timeout {CAN_TIMEOUT} ms")
            can_sub.set_color(MUTED)

            # ---- nothing above is current if the feed has stopped ----
            if stale:
                for v in (tps_val, rpm_val, gate_val):
                    v.set_color(DIM)
                tps_sub.set_text("last value (stale)")
                rpm_sub.set_text("last value (stale)")
                tps_sub.set_color(DIM); rpm_sub.set_color(DIM)
                gate_val.set_text("--")
                bar_fg.set_width(0.0)
                gate_c1.set_text(""); gate_c2.set_text("")
                can_val.set_text("?"); can_val.set_color(DIM)
                can_sub.set_text("unknown (stale)")
                can_sub.set_color(DIM)

            # ---- pedal deviation (host-side only) ----
            if np.isfinite(last["s1"]) and np.isfinite(last["s2"]):
                dev = abs(last["s1"] - last["s2"])
                wide = dev > PEDAL_DEV_WARN_MV
                dev_note.set_text(f"S1-S2 spread {dev:.0f} mV"
                                  + ("   WIDE" if wide else "")
                                  + "   (not checked by firmware)")
                dev_note.set_color(DIM if stale else (C_WARN if wide else DIM))
            else:
                dev_note.set_text("S1/S2 not present in the status line")
                dev_note.set_color(DIM)

            fig.canvas.draw_idle()
            plt.pause(0.001)                     # services the redraw

    except KeyboardInterrupt:
        pass
    finally:
        reader.stop()
        time.sleep(0.15)
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass
        csv_f.close()
        if reader.n_ok == 0:
            print("\nNo status lines parsed.")
            if reader.n_bytes == 0:
                print("  Nothing arrived - wrong port, or the sketch is not "
                      "printing (native-USB boards need the port reopened "
                      "after a reset).")
            elif reader.n_nonprint > reader.n_bytes * 0.2:
                print("  Mostly unprintable bytes -> BAUD MISMATCH.")
            else:
                print("  Lines arrived but did not match. First lines:")
                for L in reader.first_lines:
                    print("   ", repr(L))
        else:
            print(f"\nlines {reader.n_ok}   brake state changes {brake_edges}")
        if events:
            print("\nlast events:")
            for et, txt in list(events)[-6:]:
                print(f"   {et:7.1f} s  {txt}")
        print("csv:", args.csv)


if __name__ == "__main__":
    main()
