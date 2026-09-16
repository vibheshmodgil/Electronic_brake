#!/usr/bin/env python3
"""
can_brake_dashboard.py - live dashboard for the ESP32-S3 / TWAI-CAN
electronic brake controller (EBrake_Control_ESP32S3_2ChRELAY_TJA1050.ino).

Same layout and conventions as brake_dashboard.py, but fed by the CAN
build's telemetry instead of the Mega's analogue one:

    BRAKE STATE   full-width banner + colour shading behind every trace
                  + a dedicated APPLIED/RELEASED strip
    TPS           throttle position [%] with the release/apply thresholds
    RPM           shaft speed from CAN 0x015, with the +/- threshold band
    PEDAL/TORQUE  raw accelerator sensors S1/S2 [mV] and torque request [Nm]

Expected serial line (printStatus() in the sketch):

    RPM: -12 | S1: 820 mV | S2: 815 mV | TPS: 4.32 % | Torque: 0.00 Nm |
    Relay: OFF | Brake: APPLIED | Timer: RESET

Two things the firmware does NOT send, reconstructed here:

  * Timer progress. The sketch prints only RUNNING/RESET, so elapsed time
    is measured on this end from the RUNNING edge and clamped to
    --brake-ms. It is an estimate, drawn dashed, not a device reading.

  * CAN health. The sketch announces "CAN signals unavailable" once on
    entering the fault and says nothing on recovery. The CAN chip here
    turns red on that message and clears again as soon as any decoded
    value changes, which can only happen if fresh frames are arriving.
    Inferred, not reported.

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

# ============================================================
# DEFAULTS - keep in sync with the sketch
# ============================================================

BAUD          = 115200
TPS_RELEASE   = 5.0       # TPS_RELEASE_THRESHOLD_PERCENT
TPS_APPLY     = 4.5       # TPS_APPLY_THRESHOLD_PERCENT
RPM_TH        = 20.0      # RPM_APPLY_THRESHOLD
BRAKE_MS      = 1000      # BRAKE_APPLY_DELAY_MS
CAN_TIMEOUT   = 500       # CAN_TIMEOUT_MS (shown, not enforced here)
WINDOW_S      = 30.0
FPS           = 20.0
MAX_POINTS    = 20000
CSV_FLUSH_SEC = 1.0

# autoscale behaviour
TPS_MIN_SPAN  = 12.0      # %    - never zoom tighter than this
RPM_MIN_SPAN  = 40.0      # rpm
MV_MIN_SPAN   = 200.0     # mV
PAD           = 0.18      # fraction of span added above and below
SHRINK_RATIO  = 2.2       # only zoom back in when the axis is this much too big

# palette
BG      = "#0e1216"
PANEL   = "#161b21"
GRID    = "#28313a"
FG      = "#e3eaf2"
MUTED   = "#7d8a97"
C_TPS   = "#4ea3ff"
C_RPM   = "#39d98a"
C_BRAKE = "#ff4d54"       # brake APPLIED
C_FREE  = "#2ecc71"       # brake RELEASED
C_TIMER = "#ffb020"
C_S1    = "#9b8cff"
C_S2    = "#5f6fd6"
C_TRQ   = "#ff8f4d"
C_WARN  = "#ffd166"

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

                if self.brake > 0.5:
                    self.timer_on = False
                    if tps >= TPS_RELEASE:
                        self.brake = 0.0
                else:
                    if tps < TPS_APPLY and abs(rpm) < RPM_TH:
                        if not self.timer_on:
                            self.timer_on, self.timer_t0 = True, t
                        elif (t - self.timer_t0) * 1000.0 >= BRAKE_MS:
                            self.brake, self.timer_on = 1.0, False
                    else:
                        self.timer_on = False
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


def style_axis(ax, ylabel):
    ax.set_facecolor(PANEL)
    ax.grid(True, color=GRID, lw=0.6, alpha=0.85)
    ax.set_axisbelow(True)
    ax.tick_params(colors=MUTED, labelsize=8)
    for s in ax.spines.values():
        s.set_color(GRID)
    ax.set_ylabel(ylabel, color=MUTED, fontsize=9)


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
    events = deque(maxlen=60)

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
    fig = plt.figure(figsize=(14.5, 9.5))
    try:
        fig.canvas.manager.set_window_title(
            "ESP32-S3 CAN - TPS / RPM / brake")
    except Exception:
        pass
    gs = GridSpec(5, 4, figure=fig,
                  height_ratios=[1.05, 1.45, 1.45, 1.05, 0.5],
                  hspace=0.42, wspace=0.22,
                  left=0.06, right=0.94, top=0.94, bottom=0.07)

    # --- brake banner ---
    ax_ban = fig.add_subplot(gs[0, 0])
    ax_ban.set_xticks([]); ax_ban.set_yticks([])
    for s in ax_ban.spines.values():
        s.set_color(GRID); s.set_linewidth(2)
    ax_ban.set_facecolor(C_BRAKE)
    ban_txt = ax_ban.text(0.5, 0.58, "APPLIED", color="#0b0e11", fontsize=24,
                          ha="center", va="center", fontweight="bold",
                          transform=ax_ban.transAxes)
    ban_sub = ax_ban.text(0.5, 0.16, "waiting for data", color="#0b0e11",
                          fontsize=9, ha="center", va="center",
                          transform=ax_ban.transAxes)

    def card(ax, title):
        ax.set_xticks([]); ax.set_yticks([])
        ax.set_facecolor(PANEL)
        for s in ax.spines.values():
            s.set_color(GRID)
        ax.text(0.06, 0.80, title, color=MUTED, fontsize=9,
                transform=ax.transAxes)
        val = ax.text(0.06, 0.42, "--", color=FG, fontsize=24,
                      transform=ax.transAxes)
        sub = ax.text(0.06, 0.15, "", color=MUTED, fontsize=8.5,
                      transform=ax.transAxes)
        return val, sub

    tps_val, tps_sub = card(fig.add_subplot(gs[0, 1]), "TPS  (CAN 0x0B7)")
    rpm_val, rpm_sub = card(fig.add_subplot(gs[0, 2]), "RPM  (CAN 0x015)")

    # --- CAN link card ---
    ax_can = fig.add_subplot(gs[0, 3])
    ax_can.set_xticks([]); ax_can.set_yticks([])
    ax_can.set_facecolor(PANEL)
    for s in ax_can.spines.values():
        s.set_color(GRID)
    ax_can.text(0.06, 0.80, "CAN LINK", color=MUTED, fontsize=9,
                transform=ax_can.transAxes)
    can_val = ax_can.text(0.06, 0.42, "--", color=FG, fontsize=24,
                          transform=ax_can.transAxes)
    can_sub = ax_can.text(0.06, 0.15, f"timeout {CAN_TIMEOUT} ms", color=MUTED,
                          fontsize=8.5, transform=ax_can.transAxes)

    # --- TPS ---
    ax_tps = fig.add_subplot(gs[1, :])
    style_axis(ax_tps, "TPS [%]")
    ax_tps.axhline(REL_TH, ls="--", lw=1.1, color=MUTED)
    ax_tps.axhline(APP_TH, ls=":", lw=1.1, color=MUTED)
    ax_tps.text(0.004, REL_TH, f" release {REL_TH:.1f} %", color=MUTED,
                fontsize=8, va="bottom",
                transform=ax_tps.get_yaxis_transform())
    ax_tps.text(0.004, APP_TH, f" apply {APP_TH:.1f} %", color=MUTED,
                fontsize=8, va="top", transform=ax_tps.get_yaxis_transform())
    l_tps, = ax_tps.plot([], [], lw=1.7, color=C_TPS)
    ax_tps.set_ylim(0, max(REL_TH * 3, TPS_MIN_SPAN))
    ax_tps.set_xlim(0, args.window)
    ax_tps.tick_params(labelbottom=False)   # only the strip carries the axis

    # --- RPM ---
    ax_rpm = fig.add_subplot(gs[2, :], sharex=ax_tps)
    style_axis(ax_rpm, "speed [rpm]")
    ax_rpm.axhline(RP_TH, ls="--", lw=1.0, color=MUTED)
    ax_rpm.axhline(-RP_TH, ls="--", lw=1.0, color=MUTED)
    ax_rpm.text(0.004, RP_TH, f" +/-{RP_TH:.0f} rpm", color=MUTED, fontsize=8,
                va="bottom", transform=ax_rpm.get_yaxis_transform())
    l_rpm, = ax_rpm.plot([], [], lw=1.8, color=C_RPM)
    ax_rpm.set_ylim(-RP_TH * 2, RP_TH * 2)
    ax_rpm.tick_params(labelbottom=False)

    # --- pedal sensors + torque request ---
    ax_ped = fig.add_subplot(gs[3, :], sharex=ax_tps)
    style_axis(ax_ped, "pedal [mV]")
    ax_ped.tick_params(labelbottom=False)
    l_s1, = ax_ped.plot([], [], lw=1.3, color=C_S1, label="S1")
    l_s2, = ax_ped.plot([], [], lw=1.3, color=C_S2, label="S2")
    ax_trq = ax_ped.twinx()
    ax_trq.set_facecolor("none")
    ax_trq.tick_params(colors=MUTED, labelsize=8)
    for s in ax_trq.spines.values():
        s.set_color(GRID)
    ax_trq.set_ylabel("torque req [Nm]", color=C_TRQ, fontsize=9)
    l_trq, = ax_trq.plot([], [], lw=1.4, color=C_TRQ, label="torque")
    leg = ax_ped.legend(handles=[l_s1, l_s2, l_trq], loc="upper left",
                        fontsize=8, facecolor=PANEL, edgecolor=GRID,
                        labelcolor=MUTED, framealpha=0.9)
    leg.get_frame().set_linewidth(0.6)

    # --- brake strip ---
    ax_st = fig.add_subplot(gs[4, :], sharex=ax_tps)
    ax_st.set_facecolor(PANEL)
    ax_st.set_yticks([]); ax_st.set_ylim(0, 1)
    ax_st.tick_params(colors=MUTED, labelsize=8)
    for s in ax_st.spines.values():
        s.set_color(GRID)
    ax_st.set_ylabel("brake", color=MUTED, fontsize=9)
    ax_st.set_xlabel("time [s]", color=MUTED, fontsize=9)
    l_tmr, = ax_st.plot([], [], lw=1.4, ls="--", color=C_TIMER)

    shading = {"tps": None, "rpm": None, "ped": None,
               "st_on": None, "st_off": None}
    title = fig.suptitle("waiting for data ...", color=FG, fontsize=13)

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

    try:
        while plt.fignum_exists(fig.number) and reader.running:
            recs, evs = reader.drain()

            for e in evs:
                events.append(e)
                if RE_CAN_LOST.search(e[1]):
                    can_fault, can_fault_t = True, e[0]

            for r in recs:
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

            if time.time() - last_flush > CSV_FLUSH_SEC:
                csv_f.flush(); last_flush = time.time()

            now = time.time()
            if now < next_draw:
                plt.pause(0.01)                  # pumps the GUI event loop
                continue
            next_draw = now + frame_dt

            if len(T) < 2 or last is None:
                title.set_text(f"waiting for data ...  "
                               f"{reader.n_bytes} bytes received")
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
            l_tmr.set_data(t, tmr)

            x0 = max(t[0], t[-1] - args.window)
            ax_tps.set_xlim(x0, max(t[-1], x0 + 1e-3))
            vis = t >= x0

            smart_ylim(ax_tps, tps[vis], TPS_MIN_SPAN,
                       include=(0.0, REL_TH, APP_TH))
            smart_ylim(ax_rpm, rpm[vis], RPM_MIN_SPAN,
                       include=(-RP_TH, RP_TH))
            smart_ylim(ax_ped, np.concatenate((s1[vis], s2[vis])), MV_MIN_SPAN)
            smart_ylim(ax_trq, trq[vis], 2.0, include=(0.0,))

            for key, ax in (("tps", ax_tps), ("rpm", ax_rpm),
                            ("ped", ax_ped)):
                if shading[key] is not None:
                    shading[key].remove()
                shading[key] = ax.fill_between(
                    t, 0, 1, where=brk > 0.5, step="post",
                    transform=ax.get_xaxis_transform(),
                    color=C_BRAKE, alpha=0.13, lw=0)
            for key, cond, col in (("st_on", brk > 0.5, C_BRAKE),
                                   ("st_off", brk <= 0.5, C_FREE)):
                if shading[key] is not None:
                    shading[key].remove()
                shading[key] = ax_st.fill_between(
                    t, 0, 1, where=cond, step="post",
                    color=col, alpha=0.55, lw=0)

            braking = last["brake"] > 0.5
            ax_ban.set_facecolor(C_BRAKE if braking else C_FREE)
            ban_txt.set_text("APPLIED" if braking else "RELEASED")
            ban_sub.set_text(("relay OFF - shaft held" if braking
                              else "relay ON - shaft free")
                             + f"    {brake_edges} changes")

            released = last["tps"] >= REL_TH
            low_tps = last["tps"] < APP_TH
            tps_val.set_text(f"{last['tps']:6.2f} %")
            tps_val.set_color(C_WARN if released else C_TPS)
            if released:
                tps_sub.set_text(f"ABOVE release {REL_TH:.1f} %")
            elif low_tps:
                tps_sub.set_text(f"below apply {APP_TH:.1f} %")
            else:
                tps_sub.set_text(f"in hysteresis band "
                                 f"{APP_TH:.1f}-{REL_TH:.1f} %")

            slow = abs(last["rpm"]) < RP_TH
            rpm_val.set_text(f"{last['rpm']:7.0f}")
            rpm_val.set_color(C_RPM if slow else C_WARN)
            rpm_sub.set_text(f"|rpm| {'<' if slow else '>='} {RP_TH:.0f}"
                             + (f"    timer {int(timer_ms)}/{BR_MS} ms"
                                if last["timer"] > 0.5 else ""))

            can_val.set_text("FAULT" if can_fault else "OK")
            can_val.set_color(C_BRAKE if can_fault else C_FREE)
            can_sub.set_text(f"lost at t={can_fault_t:.1f} s"
                             if can_fault and can_fault_t is not None
                             else f"timeout {CAN_TIMEOUT} ms")

            title.set_text(
                f"{'BRAKE APPLIED' if braking else 'BRAKE RELEASED'}     "
                f"TPS {last['tps']:.2f} %     "
                f"{last['rpm']:.0f} rpm     "
                f"torque {last['torque']:.2f} Nm     "
                f"{t[-1]:.0f} s     {reader.n_ok} lines")
            title.set_color(C_BRAKE if braking else C_FREE)

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
