// ============================================================
// CanDbc.example.h - minimal public DBC. Copy to CanDbc.h.
// ============================================================
//
// CanDbc.h is gitignored: the real bus layout is not published with this
// repository. After cloning, copy this file to CanDbc.h, or the sketch
// will not build:
//
//   copy CanDbc.example.h CanDbc.h        (Windows)
//   cp   CanDbc.example.h CanDbc.h        (macOS / Linux)
//
// Everything between the rawliteral markers is an ordinary DBC file. The
// board serves it at /can.dbc and the page decodes with it. It holds only
// the two messages the brake logic reads, with the layouts EBrakeCAN.ino
// already decodes. Paste your own DBC text in CanDbc.h for a richer
// built-in view, or keep this and use the page's LOAD .DBC button.
//
// The page tags a signal "predicted" unless its CM_ SG_ comment starts
// with CONFIRMED.
//
#pragma once

static const char CAN_DBC[] PROGMEM = R"rawliteral(VERSION "EBrakeCAN minimal"

NS_ :
	CM_

BS_:

BU_: EBRAKE


BO_ 21 ISAAC_State: 8 Vector__XXX
 SG_ N_RPM_SIG : 31|16@0- (1,0) [-32768|32767] "rpm" EBRAKE

BO_ 183 Acc_Pedals: 8 Vector__XXX
 SG_ AccPed_S1_SIG : 7|16@0- (1,0) [-32768|32767] "" EBRAKE
 SG_ AccPed_S2_SIG : 23|16@0- (1,0) [-32768|32767] "" EBRAKE
 SG_ AccPed_TPS_pct_SIG : 39|16@0- (0.01,0) [0|100] "%" EBRAKE
 SG_ AccPed_TorqueReq_Nm_SIG : 55|16@0- (0.01,0) [-327.68|327.67] "Nm" EBRAKE

CM_ SG_ 21 N_RPM_SIG "CONFIRMED position: bytes 3-4, signed big-endian. Scale of 1 rpm not checked against a tachometer.";
CM_ SG_ 183 AccPed_S1_SIG "CONFIRMED position: bytes 0-1, signed big-endian. Unit not verified.";
CM_ SG_ 183 AccPed_S2_SIG "CONFIRMED position: bytes 2-3, signed big-endian. Unit not verified.";
CM_ SG_ 183 AccPed_TPS_pct_SIG "CONFIRMED: bytes 4-5, x0.01 percent - the value the brake logic compares.";
CM_ SG_ 183 AccPed_TorqueReq_Nm_SIG "PREDICTED scale: bytes 6-7, x0.01 Nm not verified.";
)rawliteral";
