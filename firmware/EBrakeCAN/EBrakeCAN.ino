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

  brakeState = BRAKE_APPLIED;
  brakeTimerRunning = false;
}

void commandBrakeReleased()
{
  // HL-52S LOW = relay ON.
  digitalWrite(RELAY_PIN, RELAY_ON);

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
      Serial.println(
          "CAN signals unavailable: commanding relay OFF");

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

      Serial.println(
          "TPS >= 0%: relay ON, BRAKE RELEASED");
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
      Serial.println(
          "Apply condition interrupted: timer reset");
    }

    brakeTimerRunning = false;
    return;
  }

  // Start timer once.
  if (!brakeTimerRunning)
  {
    brakeTimerRunning = true;
    brakeTimerStartTime = now;

    Serial.println(
        "TPS low and speed below 20 RPM: "
        "one-second timer started");

    return;
  }

  // Apply brake after continuously satisfying conditions.
  if ((now - brakeTimerStartTime) >=
      BRAKE_APPLY_DELAY_MS)
  {
    commandBrakeApplied();

    Serial.println(
        "Conditions true for 1 second: "
        "relay OFF, BRAKE APPLIED");
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
  printStatus();
}