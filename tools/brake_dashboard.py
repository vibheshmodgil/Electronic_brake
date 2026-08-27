#!/usr/bin/env python3
"""
brake_dashboard.py - live dashboard for the Arduino Mega 2560
throttle / RPM / brake-relay sketch.

Shows three things only:

    BRAKE STATE   full-width banner + colour shading behind every trace
                  + a dedicated ON/OFF strip
    THROTTLE      voltage vs time, with the arm threshold
    RPM           filtered speed vs time, with the +/- threshold band

Both time plots autoscale to the visible window, with hysteresis so the
axis does not twitch on every frame. Everything is logged to CSV too.

Usage
-----
  python brake_dashboard.py --port COM3
  python brake_dashboard.py --port COM3 --csv run1.csv --window 30
  python brake_dashboard.py --port COM3 --rpm-threshold 20 --brake-ms 1000
  python brake_dashboard.py --demo          # fake data, no hardware needed

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
THROTTLE_TH   = 0.80      # THROTTLE_ON_V / THROTTLE_OFF_V
RPM_TH        = 20.0      # RPM_THRESHOLD
BRAKE_MS      = 1000      # BRAKE_DELAY_MS
WINDOW_S      = 30.0
FPS           = 20.0
MAX_POINTS    = 20000
CSV_FLUSH_SEC = 1.0

# autoscale behaviour
THR_MIN_SPAN  = 0.40      # V   - never zoom tighter than this
RPM_MIN_SPAN  = 40.0      # rpm
PAD           = 0.18      # fraction of span added above and below
SHRINK_RATIO  = 2.2       # only zoom back in when the axis is this much too big

# palette
BG      = "#0e1216"
PANEL   = "#161b21"
GRID    = "#28313a"
FG      = "#e3eaf2"
MUTED   = "#7d8a97"
C_THR   = "#4ea3ff"
C_RPM   = "#39d98a"
C_BRAKE = "#ff4d54"       # brake APPLIED
C_FREE  = "#2ecc71"       # brake RELEASED
C_TIMER = "#ffb020"

NUM = r"[-+]?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][-+]?\d+)?"


def rx(p):
    return re.compile(p, re.IGNORECASE)


RE_THROTTLE = rx(rf"throttle\s*=\s*({NUM})")
RE_RPM_FILT = rx(rf"(?:^|\|)\s*RPM\s*=\s*({NUM})")
RE_RELAY    = rx(r"relay\s*=\s*(ON|OFF)")
RE_BRAKE    = rx(r"brake\s*=\s*(ON|OFF)")
RE_TIMER    = rx(r"timer\s*=\s*(\d+)\s*/\s*(\d+)")


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
                        ("mega", "arduino", "ch340", "usb-serial")) else 1

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

        mt, mr = RE_THROTTLE.search(line), RE_RPM_FILT.search(line)
        if not (mt and mr):
            if "=" not in line and len(line.strip("= ")) > 3:
                if self.t0 is None:
                    self.t0 = time.time()
                with self._lock:
                    self._events.append((time.time() - self.t0, line))
            return

        if self.t0 is None:
            self.t0 = time.time()
        mrl, mbk = RE_RELAY.search(line), RE_BRAKE.search(line)
        mtm = RE_TIMER.search(line)
        relay = 1.0 if (mrl and mrl.group(1).upper() == "ON") else 0.0
        brake = (1.0 if mbk.group(1).upper() == "ON" else 0.0) if mbk \
            else 1.0 - relay
        rec = {
            "t": time.time() - self.t0,
            "throttle": float(mt.group(1)),
            "rpm": float(mr.group(1)),
            "relay": relay,
            "brake": brake,
            "timer_ms": float(mtm.group(1)) if mtm else 0.0,
        }
        self.n_ok += 1
        with self._lock:
            self._data.append(rec)


class DemoReader(threading.Thread):
    """Synthetic source so the GUI can be verified without hardware."""

    def __init__(self):
        super().__init__(daemon=True)
        self.running = True
        self._data, self._events = [], []
        self._lock = threading.Lock()
        self.t0 = time.time()
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
        relay = 0.0
        while self.running:
            t = time.time() - self.t0
            duty = 0.5 * (1 + math.sin(t / 6.0))
            thr = 0.75 + 2.6 * max(0.0, duty - 0.45)
            rpm = 1200 * max(0.0, duty - 0.45) + np.random.randn() * 6
            relay = 1.0 if thr >= THROTTLE_TH else relay
            if thr < THROTTLE_TH and abs(rpm) < RPM_TH:
                relay = 0.0
            self.n_ok += 1
            with self._lock:
                self._data.append({"t": t, "throttle": thr, "rpm": float(rpm),
                                   "relay": relay, "brake": 1.0 - relay,
                                   "timer_ms": 0.0})
            time.sleep(0.2)


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
    ap.add_argument("--csv", default="mega_log.csv")
    ap.add_argument("--window", type=float, default=WINDOW_S)
    ap.add_argument("--throttle-threshold", type=float, default=THROTTLE_TH)
    ap.add_argument("--rpm-threshold", type=float, default=RPM_TH)
    ap.add_argument("--brake-ms", type=int, default=BRAKE_MS)
    ap.add_argument("--echo", action="store_true")
    ap.add_argument("--demo", action="store_true",
                    help="synthetic data - check the GUI without hardware")
    args = ap.parse_args()

    TH_TH, RP_TH, BR_MS = (args.throttle_threshold, args.rpm_threshold,
                           args.brake_ms)

    # ---- GUI backend sanity check (this is what was broken before) ----
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
        time.sleep(2.0)                  # Mega auto-resets on DTR
        ser.reset_input_buffer()
        print(f"Connected {port} @ {args.baud}   logging -> {args.csv}\n"
              "Close the window or press Ctrl-C to stop.\n")
        reader = Reader(ser, echo=args.echo)
    reader.start()

    n = MAX_POINTS
    T = deque(maxlen=n); THR = deque(maxlen=n); RPM = deque(maxlen=n)
    BRK = deque(maxlen=n); TMR = deque(maxlen=n)
    events = deque(maxlen=60)

    csv_f = open(args.csv, "w", newline="")
    csv_w = csv.writer(csv_f)
    csv_w.writerow(["t_s", "throttle_V", "rpm_filt", "brake_on",
                    "relay_on", "timer_ms"])
    last_flush = time.time()

    # ---------------- figure ----------------
    plt.rcParams.update({"figure.facecolor": BG, "savefig.facecolor": BG,
                         "text.color": FG, "font.family": "monospace"})
    plt.ion()                                    # <-- interactive mode ON
    fig = plt.figure(figsize=(14.5, 8.5))
    try:
        fig.canvas.manager.set_window_title("MEGA 2560 - throttle / RPM / brake")
    except Exception:
        pass
    gs = GridSpec(4, 3, figure=fig, height_ratios=[1.05, 1.5, 1.5, 0.55],
                  hspace=0.38, wspace=0.22,
                  left=0.06, right=0.985, top=0.94, bottom=0.08)

    # --- brake banner ---
    ax_ban = fig.add_subplot(gs[0, 0])
    ax_ban.set_xticks([]); ax_ban.set_yticks([])
    for s in ax_ban.spines.values():
        s.set_color(GRID); s.set_linewidth(2)
    ax_ban.set_facecolor(C_BRAKE)
    ban_txt = ax_ban.text(0.5, 0.55, "BRAKE ON", color="#0b0e11", fontsize=28,
                          ha="center", va="center", fontweight="bold",
                          transform=ax_ban.transAxes)
    ban_sub = ax_ban.text(0.5, 0.16, "waiting for data", color="#0b0e11",
                          fontsize=10, ha="center", va="center",
                          transform=ax_ban.transAxes)

    def card(ax, title):
        ax.set_xticks([]); ax.set_yticks([])
        ax.set_facecolor(PANEL)
        for s in ax.spines.values():
            s.set_color(GRID)
        ax.text(0.06, 0.80, title, color=MUTED, fontsize=9,
                transform=ax.transAxes)
        val = ax.text(0.06, 0.42, "--", color=FG, fontsize=27,
                      transform=ax.transAxes)
        sub = ax.text(0.06, 0.15, "", color=MUTED, fontsize=9,
                      transform=ax.transAxes)
        return val, sub

    thr_val, thr_sub = card(fig.add_subplot(gs[0, 1]), "THROTTLE")
    rpm_val, rpm_sub = card(fig.add_subplot(gs[0, 2]), "RPM  (filtered)")

    ax_thr = fig.add_subplot(gs[1, :])
    style_axis(ax_thr, "throttle [V]")
    ax_thr.axhline(TH_TH, ls="--", lw=1.1, color=MUTED)
    ax_thr.text(0.004, TH_TH, f" arm {TH_TH:.2f} V", color=MUTED, fontsize=8,
                va="bottom", transform=ax_thr.get_yaxis_transform())
    l_thr, = ax_thr.plot([], [], lw=1.7, color=C_THR)
    ax_thr.set_ylim(0, TH_TH * 2)
    ax_thr.set_xlim(0, args.window)

    ax_rpm = fig.add_subplot(gs[2, :], sharex=ax_thr)
    style_axis(ax_rpm, "speed [rpm]")
    ax_rpm.axhline(RP_TH, ls="--", lw=1.0, color=MUTED)
    ax_rpm.axhline(-RP_TH, ls="--", lw=1.0, color=MUTED)
    ax_rpm.text(0.004, RP_TH, f" +/-{RP_TH:.0f} rpm", color=MUTED, fontsize=8,
                va="bottom", transform=ax_rpm.get_yaxis_transform())
    l_rpm, = ax_rpm.plot([], [], lw=1.8, color=C_RPM)
    ax_rpm.set_ylim(-RP_TH * 2, RP_TH * 2)

    ax_st = fig.add_subplot(gs[3, :], sharex=ax_thr)
    ax_st.set_facecolor(PANEL)
    ax_st.set_yticks([]); ax_st.set_ylim(0, 1)
    ax_st.tick_params(colors=MUTED, labelsize=8)
    for s in ax_st.spines.values():
        s.set_color(GRID)
    ax_st.set_ylabel("brake", color=MUTED, fontsize=9)
    ax_st.set_xlabel("time [s]", color=MUTED, fontsize=9)
    l_tmr, = ax_st.plot([], [], lw=1.4, color=C_TIMER)

    shading = {"thr": None, "rpm": None, "st_on": None, "st_off": None}
    title = fig.suptitle("waiting for data ...", color=FG, fontsize=13)

    plt.show(block=False)                        # <-- actually map the window
    fig.canvas.draw()
    plt.pause(0.05)

    # ---------------- loop ----------------
    frame_dt = 1.0 / FPS
    next_draw = 0.0
    last = None
    brake_edges = 0
    prev_brake = None

    try:
        while plt.fignum_exists(fig.number) and reader.running:
            recs, evs = reader.drain()
            for r in recs:
                T.append(r["t"]); THR.append(r["throttle"]); RPM.append(r["rpm"])
                BRK.append(r["brake"]); TMR.append(min(1.0, r["timer_ms"] / BR_MS))
                if prev_brake is not None and r["brake"] != prev_brake:
                    brake_edges += 1
                prev_brake = r["brake"]
                csv_w.writerow([f"{r['t']:.3f}", f"{r['throttle']:.4f}",
                                f"{r['rpm']:.3f}", int(r["brake"]),
                                int(r["relay"]), int(r["timer_ms"])])
            if recs:
                last = recs[-1]
            for e in evs:
                events.append(e)

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
            thr = np.fromiter(THR, float)
            rpm = np.fromiter(RPM, float)
            brk = np.fromiter(BRK, float)
            tmr = np.fromiter(TMR, float)

            l_thr.set_data(t, thr)
            l_rpm.set_data(t, rpm)
            l_tmr.set_data(t, tmr)

            x0 = max(t[0], t[-1] - args.window)
            ax_thr.set_xlim(x0, max(t[-1], x0 + 1e-3))
            vis = t >= x0

            smart_ylim(ax_thr, thr[vis], THR_MIN_SPAN, include=(TH_TH,))
            smart_ylim(ax_rpm, rpm[vis], RPM_MIN_SPAN, include=(-RP_TH, RP_TH))

            for key, ax in (("thr", ax_thr), ("rpm", ax_rpm)):
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
            ban_txt.set_text("BRAKE ON" if braking else "BRAKE OFF")
            ban_sub.set_text(("relay OFF - motor braked" if braking
                              else "relay ON - motor free")
                             + f"    {brake_edges} changes")

            armed = last["throttle"] >= TH_TH
            thr_val.set_text(f"{last['throttle']:5.3f} V")
            thr_val.set_color("#ffd166" if armed else C_THR)
            thr_sub.set_text(f"{'ABOVE' if armed else 'below'} "
                             f"{TH_TH:.2f} V threshold")

            slow = abs(last["rpm"]) < RP_TH
            rpm_val.set_text(f"{last['rpm']:7.1f}")
            rpm_val.set_color(C_RPM if slow else "#ffd166")
            rpm_sub.set_text(f"|rpm| {'<' if slow else '>='} {RP_TH:.0f}"
                             + (f"    timer {int(last['timer_ms'])}/{BR_MS} ms"
                                if last["timer_ms"] > 0 else ""))

            title.set_text(
                f"{'BRAKE APPLIED' if braking else 'BRAKE RELEASED'}     "
                f"throttle {last['throttle']:.3f} V     "
                f"{last['rpm']:.1f} rpm     "
                f"{t[-1]:.0f} s     {reader.n_ok} lines")
            title.set_color(C_BRAKE if braking else C_FREE)

            fig.canvas.draw_idle()
            plt.pause(0.001)                     # <-- services the redraw

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
                print("  Nothing arrived - wrong port or sketch not printing.")
            elif reader.n_nonprint > reader.n_bytes * 0.2:
                print("  Mostly unprintable bytes -> BAUD MISMATCH.")
            else:
                print("  Lines arrived but did not match. First lines:")
                for L in reader.first_lines:
                    print("   ", repr(L))
        else:
            print(f"\nlines {reader.n_ok}   brake state changes {brake_edges}")
        print("csv:", args.csv)


if __name__ == "__main__":
    main()