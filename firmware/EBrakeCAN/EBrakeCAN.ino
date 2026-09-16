#include <Arduino.h>
#include "driver/twai.h"

// ============================================================
// ESP32-S3 CAN configuration
// Keep the pins and baud rate that already work.
// ============================================================
#define CAN_TX_PIN GPIO_NUM_5
#define CAN_RX_PIN GPIO_NUM_6

// DBC message IDs
#define ISAAC_STATE_CAN_ID 0x015  // Decimal 21
#define ACC_PEDALS_CAN_ID  0x0B7  // Decimal 183

// ============================================================
// HL-52S relay configuration
// ============================================================
#define RELAY_PIN GPIO_NUM_7

// Confirmed HL-52S behavior:
// LOW  = relay energized/ON  = brake released
// HIGH = relay de-energized/OFF = brake applied
#define RELAY_ON  LOW
#define RELAY_OFF HIGH

// ============================================================
// Web UI
// ============================================================
//
// Set to 0 to compile the access point and server out completely. Do
// that for anything past bench testing: a radio reaches further than the
// room, and the brake does not need WiFi to work.
//
#define ENABLE_WEB_UI 1

#define WEB_AP_SSID        "EBrake-Monitor"
#define WEB_AP_PASSWORD    "brake1234"   // WPA2 needs 8 characters. CHANGE IT.
#define WEB_AP_CHANNEL     1
#define WEB_AP_MAX_CLIENTS 4

// Core 0 runs the WiFi stack already. The Arduino loop - and therefore
// the brake algorithm - is on core 1 and stays there.
#define WEB_TASK_CORE      0

// ============================================================
// Algorithm calibration
// ============================================================
const float TPS_RELEASE_THRESHOLD_PERCENT = 5.0f;
const float TPS_APPLY_THRESHOLD_PERCENT = 4.5f;

// Brake can be applied only below this speed magnitude.
const int16_t RPM_APPLY_THRESHOLD = 20;

// Conditions must stay true continuously for this duration.
const unsigned long BRAKE_APPLY_DELAY_MS = 1000;

// Maximum allowed age of the required CAN signals.
const unsigned long CAN_TIMEOUT_MS = 500;

// Serial Monitor interval.
const unsigned long PRINT_INTERVAL_MS = 100;

// ============================================================
// Brake state
// ============================================================
enum BrakeState
{
  BRAKE_APPLIED,
  BRAKE_RELEASED
};

BrakeState brakeState = BRAKE_APPLIED;

// ============================================================
// Latest decoded CAN values
// ============================================================
int16_t latestAccPedS1_mV = 0;
int16_t latestAccPedS2_mV = 0;
float latestTPSPercent = 0.0f;
float latestTorqueRequestNm = 0.0f;
int16_t latestRPM = 0;

bool tpsFrameReceived = false;
bool rpmFrameReceived = false;

unsigned long lastTPSFrameTime = 0;
unsigned long lastRPMFrameTime = 0;

// ============================================================
// One-second brake-apply timer
// ============================================================
bool brakeTimerRunning = false;
unsigned long brakeTimerStartTime = 0;

// Prevent repetitive event messages.
bool canTimeoutReported = false;

// ============================================================
// Brake state changes, for telemetry
// ============================================================

unsigned long brakeStateChanges = 0;


// ============================================================
// Event log
// ============================================================
//
// Every event goes to the serial port exactly as before, and into a
// small ring so the web UI can show the same history to a phone that
// joined after the fact.
//
// logEventf() formats the thresholds from the constants themselves. That
// is the rule the Mega sketch already follows, for the reason documented
// in firmware/README.md: a printed threshold that disagrees with the
// constant gets believed over the code.
//

#define EVENT_SLOTS    8
#define EVENT_TEXT_MAX 88

static char eventText[EVENT_SLOTS][EVENT_TEXT_MAX];
static uint32_t eventTime[EVENT_SLOTS];
static uint8_t eventHead = 0;
static uint8_t eventCount = 0;

static portMUX_TYPE eventMux = portMUX_INITIALIZER_UNLOCKED;


void logEvent(const char *message)
{
  Serial.println(message);

  uint32_t now = millis();

  portENTER_CRITICAL(&eventMux);

  strncpy(eventText[eventHead], message, EVENT_TEXT_MAX - 1);
  eventText[eventHead][EVENT_TEXT_MAX - 1] = '\0';
  eventTime[eventHead] = now;

  eventHead = (uint8_t)((eventHead + 1) % EVENT_SLOTS);

  if (eventCount < EVENT_SLOTS)
  {
    eventCount++;
  }

  portEXIT_CRITICAL(&eventMux);
}


void logEventf(const char *format, ...)
{
  char buffer[EVENT_TEXT_MAX];

  va_list args;
  va_start(args, format);
  vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);

  logEvent(buffer);
}


// Oldest first. Returns how many were copied.
uint8_t eventsSnapshot(
    char out[][EVENT_TEXT_MAX],
    uint32_t *times,
    uint8_t maximum)
{
  portENTER_CRITICAL(&eventMux);

  uint8_t n = eventCount < maximum ? eventCount : maximum;

  for (uint8_t i = 0; i < n; i++)
  {
    uint8_t index =
        (uint8_t)((eventHead + EVENT_SLOTS - n + i) % EVENT_SLOTS);

    memcpy(out[i], eventText[index], EVENT_TEXT_MAX);
    times[i] = eventTime[index];
  }

  portEXIT_CRITICAL(&eventMux);

  return n;
}


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

static portMUX_TYPE snapshotMux = portMUX_INITIALIZER_UNLOCKED;
static BrakeSnapshot snapshotShared;


void snapshotGet(BrakeSnapshot *out)
{
  portENTER_CRITICAL(&snapshotMux);
  *out = snapshotShared;
  portEXIT_CRITICAL(&snapshotMux);
}


// ============================================================
// Decode signed 16-bit Motorola/big-endian value
// ============================================================
int16_t readSigned16BigEndian(
    const uint8_t *data,
    uint8_t firstByte)
{
  uint16_t rawValue =
      ((uint16_t)data[firstByte] << 8) |
      (uint16_t)data[firstByte + 1];

  return (int16_t)rawValue;
}

// ============================================================
// Relay/brake command functions
// ============================================================
void commandBrakeApplied()
{
  // HL-52S HIGH = relay OFF.
  digitalWrite(RELAY_PIN, RELAY_OFF);

  // Count transitions only. The CAN-fault path calls this every pass,
  // and a counter that ticked each time would be meaningless.
  if (brakeState != BRAKE_APPLIED)
  {
    brakeStateChanges++;
  }

  brakeState = BRAKE_APPLIED;
  brakeTimerRunning = false;
}

void commandBrakeReleased()
{
  // HL-52S LOW = relay ON.
  digitalWrite(RELAY_PIN, RELAY_ON);

  if (brakeState != BRAKE_RELEASED)
  {
    brakeStateChanges++;
  }

  brakeState = BRAKE_RELEASED;
  brakeTimerRunning = false;
}

// ============================================================
// Decode CAN ID 0x015
//
// SG_ N_RPM_SIG : 31|16@0- (1,0)
//
// Bytes 3 and 4 are the signed RPM value.
// ============================================================
void decodeIsaacState(const twai_message_t &message)
{
  if (message.data_length_code != 8)
  {
    return;
  }

  latestRPM =
      readSigned16BigEndian(message.data, 3);

  rpmFrameReceived = true;
  lastRPMFrameTime = millis();
}

// ============================================================
// Decode CAN ID 0x0B7
// ============================================================
void decodeAccPedals(const twai_message_t &message)
{
  if (message.data_length_code != 8)
  {
    return;
  }

  latestAccPedS1_mV =
      readSigned16BigEndian(message.data, 0);

  latestAccPedS2_mV =
      readSigned16BigEndian(message.data, 2);

  int16_t tpsRaw =
      readSigned16BigEndian(message.data, 4);

  int16_t torqueRaw =
      readSigned16BigEndian(message.data, 6);

  latestTPSPercent =
      (float)tpsRaw * 0.01f;

  latestTorqueRequestNm =
      (float)torqueRaw * 0.01f;

  tpsFrameReceived = true;
  lastTPSFrameTime = millis();
}

// ============================================================
// Validate required CAN signals
// ============================================================
bool requiredCANSignalsValid()
{
  if (!tpsFrameReceived || !rpmFrameReceived)
  {
    return false;
  }

  unsigned long now = millis();

  if ((now - lastTPSFrameTime) > CAN_TIMEOUT_MS)
  {
    return false;
  }

  if ((now - lastRPMFrameTime) > CAN_TIMEOUT_MS)
  {
    return false;
  }

  return true;
}

// ============================================================
// Brake flowchart algorithm
// ============================================================
void updateBrakeAlgorithm()
{
  unsigned long now = millis();

  // ----------------------------------------------------------
  // CAN fault or startup condition
  //
  // Cancel the timer and command relay OFF.
  // According to the flowchart:
  // relay OFF = brake applied.
  // ----------------------------------------------------------
  if (!requiredCANSignalsValid())
  {
    if (!canTimeoutReported)
    {
      logEvent("CAN signals unavailable: commanding relay OFF");

      canTimeoutReported = true;
    }

    commandBrakeApplied();
    return;
  }

  canTimeoutReported = false;

  /*
   * RPM is signed, so use its magnitude.
   *
   * Examples:
   * +15 RPM -> 15 RPM
   * -15 RPM -> 15 RPM
   */
  int32_t absoluteRPM = abs((int32_t)latestRPM);

  // ----------------------------------------------------------
  // BRAKE APPLIED
  //
  // Flowchart:
  // If TPS >= 5%, turn relay ON and release the brake.
  // ----------------------------------------------------------
  if (brakeState == BRAKE_APPLIED)
  {
    brakeTimerRunning = false;

    if (latestTPSPercent >=
        TPS_RELEASE_THRESHOLD_PERCENT)
    {
      commandBrakeReleased();

      logEventf(
          "TPS >= %.1f %%: relay ON, BRAKE RELEASED",
          (double)TPS_RELEASE_THRESHOLD_PERCENT);
    }

    return;
  }

  // ----------------------------------------------------------
  // BRAKE RELEASED
  //
  // Start the one-second timer only when:
  // TPS < 4.5% and |RPM| < 20 RPM.
  //
  // A 4.5% apply threshold provides 0.5% hysteresis relative
  // to the 5% release threshold.
  // ----------------------------------------------------------
  bool lowTPS =
      latestTPSPercent <
      TPS_APPLY_THRESHOLD_PERCENT;

  bool lowSpeed =
      absoluteRPM < RPM_APPLY_THRESHOLD;

  bool applyConditionsTrue =
      lowTPS && lowSpeed;

  if (!applyConditionsTrue)
  {
    if (brakeTimerRunning)
    {
      logEvent("Apply condition interrupted: timer reset");
    }

    brakeTimerRunning = false;
    return;
  }

  // Start timer once.
  if (!brakeTimerRunning)
  {
    brakeTimerRunning = true;
    brakeTimerStartTime = now;

    logEventf(
        "TPS < %.1f %% and |RPM| < %d: %lu ms timer started",
        (double)TPS_APPLY_THRESHOLD_PERCENT,
        (int)RPM_APPLY_THRESHOLD,
        (unsigned long)BRAKE_APPLY_DELAY_MS);

    return;
  }

  // Apply brake after continuously satisfying conditions.
  if ((now - brakeTimerStartTime) >=
      BRAKE_APPLY_DELAY_MS)
  {
    commandBrakeApplied();

    logEventf(
        "Conditions held %lu ms: relay OFF, BRAKE APPLIED",
        (unsigned long)BRAKE_APPLY_DELAY_MS);
  }
}

// ============================================================
// Serial Monitor
// ============================================================
void printStatus()
{
  static unsigned long lastPrintTime = 0;

  unsigned long now = millis();

  if ((now - lastPrintTime) < PRINT_INTERVAL_MS)
  {
    return;
  }

  lastPrintTime = now;

  const char *brakeText =
      brakeState == BRAKE_APPLIED
          ? "APPLIED"
          : "RELEASED";

  const char *relayText =
      brakeState == BRAKE_APPLIED
          ? "OFF"
          : "ON";

  Serial.printf(
      "RPM: %d | "
      "S1: %d mV | "
      "S2: %d mV | "
      "TPS: %.2f %% | "
      "Torque: %.2f Nm | "
      "Relay: %s | "
      "Brake: %s | "
      "Timer: %s\n",
      latestRPM,
      latestAccPedS1_mV,
      latestAccPedS2_mV,
      latestTPSPercent,
      latestTorqueRequestNm,
      relayText,
      brakeText,
      brakeTimerRunning ? "RUNNING" : "RESET");
}

// ============================================================
// Publish the snapshot
// ============================================================
//
// Called once per loop from core 1. The critical section is one struct
// copy: long enough to be atomic, short enough to be invisible.
//
void publishSnapshot()
{
#if ENABLE_WEB_UI

  uint32_t now = millis();

  BrakeSnapshot s;

  s.rpm    = (int32_t)latestRPM;
  s.s1_mV  = (int32_t)latestAccPedS1_mV;
  s.s2_mV  = (int32_t)latestAccPedS2_mV;
  s.tps    = latestTPSPercent;
  s.torque = latestTorqueRequestNm;

  s.relayOn      = (brakeState == BRAKE_RELEASED) ? 1 : 0;
  s.brakeApplied = (brakeState == BRAKE_APPLIED) ? 1 : 0;

  s.timerRunning = brakeTimerRunning ? 1 : 0;

  // The board knows the real elapsed time, so the phone shows it rather
  // than reconstructing it the way the serial dashboard has to.
  uint32_t elapsed = 0;

  if (brakeTimerRunning)
  {
    elapsed = now - brakeTimerStartTime;

    if (elapsed > BRAKE_APPLY_DELAY_MS)
    {
      elapsed = BRAKE_APPLY_DELAY_MS;
    }
  }

  s.timerElapsedMs = elapsed;

  s.canValid = requiredCANSignalsValid() ? 1 : 0;

  s.ageTpsMs = tpsFrameReceived
                   ? (int32_t)(now - lastTPSFrameTime)
                   : -1;

  s.ageRpmMs = rpmFrameReceived
                   ? (int32_t)(now - lastRPMFrameTime)
                   : -1;

  s.uptimeMs     = now;
  s.stateChanges = brakeStateChanges;

  portENTER_CRITICAL(&snapshotMux);
  snapshotShared = s;
  portEXIT_CRITICAL(&snapshotMux);

#endif
}


// Needs the snapshot and the event log above it.
#include "WebUI.h"


// ============================================================
// Setup
// ============================================================
void setup()
{
  Serial.begin(115200);
  delay(1000);

  /*
   * Set the relay output before CAN initialization.
   * GPIO HIGH keeps the active-low HL-52S relay OFF.
   */
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_OFF);

  brakeState = BRAKE_APPLIED;
  brakeTimerRunning = false;

  Serial.println();
  Serial.println("ESP32-S3 CAN brake algorithm bench test");
  Serial.println("HL-52S IN1 connected to GPIO 7");
  Serial.println("LOW = relay ON, HIGH = relay OFF");
  Serial.println("Startup: relay OFF, brake applied");

  twai_general_config_t generalConfig =
      TWAI_GENERAL_CONFIG_DEFAULT(
          CAN_TX_PIN,
          CAN_RX_PIN,
          TWAI_MODE_LISTEN_ONLY);

  // Keep the baud rate that worked with your CAN network.
  twai_timing_config_t timingConfig =
      TWAI_TIMING_CONFIG_500KBITS();

  twai_filter_config_t filterConfig =
      TWAI_FILTER_CONFIG_ACCEPT_ALL();

  esp_err_t result = twai_driver_install(
      &generalConfig,
      &timingConfig,
      &filterConfig);

  if (result != ESP_OK)
  {
    Serial.printf(
        "TWAI installation failed: 0x%X\n",
        result);

    commandBrakeApplied();
    return;
  }

  result = twai_start();

  if (result != ESP_OK)
  {
    Serial.printf(
        "TWAI start failed: 0x%X\n",
        result);

    twai_driver_uninstall();
    commandBrakeApplied();
    return;
  }

  Serial.println("TWAI driver started.");

  // Last, so a WiFi problem cannot delay the brake reaching its
  // fail-safe state or the CAN driver coming up.
  webUiBegin();
}

// ============================================================
// Main loop
// ============================================================
void loop()
{
  twai_message_t message;

  /*
   * Use a short receive timeout so that timer and CAN-timeout
   * processing continue even when no frame is received.
   */
  esp_err_t result = twai_receive(
      &message,
      pdMS_TO_TICKS(10));

  if (result == ESP_OK)
  {
    // Only process standard data frames.
    if (!message.extd && !message.rtr)
    {
      if (message.identifier == ISAAC_STATE_CAN_ID)
      {
        decodeIsaacState(message);
      }
      else if (message.identifier == ACC_PEDALS_CAN_ID)
      {
        decodeAccPedals(message);
      }
    }
  }

  updateBrakeAlgorithm();
  publishSnapshot();
  printStatus();
}