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
// So: the struct and the two event-buffer sizes live here, because
// snapshotGet() and eventsSnapshot() name them in their signatures. The
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
// Defined in EBrakeCAN.ino
// ============================================================

void logEvent(const char *message);

void logEventf(const char *format, ...);

uint8_t eventsSnapshot(
    char out[][EVENT_TEXT_MAX],
    uint32_t *times,
    uint8_t maximum);

void snapshotGet(BrakeSnapshot *out);

void publishSnapshot();
