// ============================================================
// Telemetry.h - types shared between the brake loop and the web UI
// ============================================================
//
// WHY THIS FILE EXISTS
// -------------------------------------------------------------------
// The Arduino IDE generates a prototype for every function in a .ino and
// inserts them all at the TOP of the file, above your own code. A
// function whose signature mentions a type or macro declared further
// down therefore gets prototyped before that type exists, and the build
// fails with something like:
//
//     error: variable or field 'snapshotGet' declared void
//     error: 'BrakeSnapshot' was not declared in this scope
//
// Anything named in a function SIGNATURE has to be visible before those
// generated prototypes - which means it belongs in a header included at
// the top of the sketch, not in the sketch body.
//
// So: BrakeSnapshot, CanFrameSlot and the buffer sizes live here, because
// snapshotGet(), canFramesSnapshot() and eventsSnapshot() name them in
// their signatures. The
// storage and the function bodies stay in EBrakeCAN.ino.
//
// If you add a function that takes or returns BrakeSnapshot, declare it
// here too.
//
#pragma once

#include <Arduino.h>


// ============================================================
// Event log sizing
// ============================================================
//
// Eight is enough to see how the machine arrived at its current state
// without the ring costing meaningful RAM: 8 x 88 = 704 bytes.
//
#define EVENT_SLOTS    8
#define EVENT_TEXT_MAX 88


// ============================================================
// Telemetry snapshot
// ============================================================
//
// The only thing the web server is allowed to see. Published by the
// brake loop on core 1, read by the server task on core 0, handed over
// under a spinlock as one struct copy so a phone can never observe half
// of one loop and half of the next.
//
struct BrakeSnapshot
{
  int32_t  rpm;
  int32_t  s1_mV;
  int32_t  s2_mV;
  float    tps;
  float    torque;
  uint8_t  relayOn;
  uint8_t  brakeApplied;
  uint8_t  timerRunning;
  uint32_t timerElapsedMs;
  uint8_t  canValid;
  int32_t  ageTpsMs;       // -1 = no frame has ever arrived
  int32_t  ageRpmMs;
  uint32_t uptimeMs;
  uint32_t stateChanges;
};


// ============================================================
// Every CAN message seen on the bus, for the web UI's signal view
// ============================================================
//
// The latest payload per ID, nothing decoded. The page decodes it with
// the DBC in CanDbc.h, so adding a signal only changes that text (it is in
// flash, so still a reflash). 64 x 24 = 1536 bytes - room above the 48 IDs
// an earlier bus survey counted. Frames from IDs beyond the table are
// counted in canFramesOverflow and not shown.
//
#define CAN_FRAME_SLOTS 64

struct CanFrameSlot
{
  uint32_t id;             // bit 31 set = extended, as a DBC writes it
  uint32_t count;
  uint32_t lastMs;
  uint8_t  dlc;
  uint8_t  data[8];
};


// ============================================================
// Defined in EBrakeCAN.ino
// ============================================================

uint8_t canFramesSnapshot(CanFrameSlot *out, uint32_t *overflow);

void logEvent(const char *message);

void logEventf(const char *format, ...);

uint8_t eventsSnapshot(
    char out[][EVENT_TEXT_MAX],
    uint32_t *times,
    uint8_t maximum);

void snapshotGet(BrakeSnapshot *out);

void publishSnapshot();
