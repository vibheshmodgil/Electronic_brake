/*
===============================================================================
ARDUINO MEGA 2560
4-CHANNEL DIFFERENTIAL SIN/COS ENCODER
+ RPM
+ THROTTLE
+ BRAKE RELAY
===============================================================================

ENCODER
-------------------------------------------------------------------------------
PCOS -> A0
PSIN -> A1
NCOS -> A2
NSIN -> A3

Differential signals:

    COS = PCOS - NCOS
    SIN = PSIN - NSIN


THROTTLE
-------------------------------------------------------------------------------
Throttle OUT1 -> A4


RELAY
-------------------------------------------------------------------------------
Relay IN -> D7


CONTROL LOGIC
-------------------------------------------------------------------------------

The three thresholds below are named constants. They are the single source
of truth: every serial message is built from them, so the printed text can
never drift away from the behaviour.

    THROTTLE_ON_V     arm threshold          (see CONFIGURATION)
    RPM_THRESHOLD     "stopped" threshold    (see CONFIGURATION)
    BRAKE_DELAY_MS    settle time            (see CONFIGURATION)


INITIAL:

    Relay OFF
    Brake ON       <- fail-safe: no power, brake applied


THROTTLE >= THROTTLE_ON_V:

    Relay ON
    Brake OFF


AFTER RELAY IS ON:

    Relay stays ON until BOTH:

        Throttle  <  THROTTLE_OFF_V
        AND
        |RPM|     <  RPM_THRESHOLD

    continuously for BRAKE_DELAY_MS milliseconds.


THEN:

    Relay OFF
    Brake ON


If throttle rises >= THROTTLE_OFF_V OR |RPM| rises >= RPM_THRESHOLD
while the timer is running:

    Timer cancelled
    Relay remains ON

===============================================================================
*/


#include <Arduino.h>
#include <math.h>


// ============================================================================
// ENCODER PINS
// ============================================================================

const uint8_t PCOS_PIN = A0;
const uint8_t PSIN_PIN = A1;
const uint8_t NCOS_PIN = A2;
const uint8_t NSIN_PIN = A3;


// ============================================================================
// THROTTLE
// ============================================================================

const uint8_t THROTTLE_PIN = A4;


// ============================================================================
// RELAY
// ============================================================================

const uint8_t RELAY_PIN = 7;


// ============================================================================
// RELAY POLARITY
// ============================================================================
//
// Most blue relay modules:
//
//     LOW  = ON
//     HIGH = OFF
//
// If your relay behaves opposite, change this to false.
//

const bool ACTIVE_LOW = true;


// ============================================================================
// FAIL-SAFE NOTE
// ============================================================================
//
// The brake must be wired so that DE-ENERGISED = BRAKE APPLIED.
//
// With that wiring a snapped wire, a dead Arduino, a blown fuse or a lost
// supply all end in the same safe state: the brake grabs. Never wire it the
// other way round, where holding the brake off is the powered state.
//
// Boot behaviour: the relay pin floats as an input for a few milliseconds
// between reset and setup(). Choose the relay module and pull resistor so
// that a floating pin leaves the relay OFF (brake applied). See
// docs/HARDWARE.md.
//


// ============================================================================
// ADC CONFIGURATION
// ============================================================================
//
// Arduino Mega:
//     10-bit ADC
//     0 ... 1023
//
// Default reference:
//     5 V
//

const float ADC_VREF = 5.00f;


// ============================================================================
// THROTTLE THRESHOLD
// ============================================================================
//
// THROTTLE_ON_V   arms the system (relay ON, brake released)
// THROTTLE_OFF_V  one of the two conditions for re-applying the brake
//
// These are deliberately EQUAL, so there is no hysteresis band. A throttle
// resting exactly on the threshold can therefore chatter. If that happens
// on your hardware, raise THROTTLE_ON_V slightly above THROTTLE_OFF_V,
// for example 0.85 / 0.75.
//
// A typical hall-effect throttle idles near 0.8...0.9 V and rises to
// ~4.2 V at full twist, so measure YOUR throttle before trusting these.
// See docs/CALIBRATION.md.
//

const float THROTTLE_ON_V  = 0.80f;
const float THROTTLE_OFF_V = 0.80f;


// ============================================================================
// RPM THRESHOLD
// ============================================================================
//
// The shaft counts as "stopped" when |rpm| is below this value.
//
// Compared against the FILTERED rpm, and against its ABSOLUTE value, so
// direction of rotation does not matter.
//
// Set this above the noise floor of your encoder at standstill, otherwise
// the brake will never engage. Watch the "RPM Raw" column with the motor
// stationary to find that floor.
//

const float RPM_THRESHOLD = 20.0f;


// ============================================================================
// BRAKE TIMER
// ============================================================================
//
// Both stop conditions must hold CONTINUOUSLY for this long before the
// brake is applied. Any breach restarts the wait from zero.
//
// Longer  = more tolerant of a brief throttle blip, slower to brake.
// Shorter = brakes sooner, more likely to grab during a momentary dip.
//

const uint32_t BRAKE_DELAY_MS = 1000;


// ============================================================================
// ENCODER CONFIGURATION
// ============================================================================
//
// Number of electrical SIN/COS cycles per mechanical revolution.
//
// If your encoder gives exactly one SIN/COS cycle per revolution:
//
//     1.0
//
// If it gives N cycles per revolution:
//
//     N
//

const float PERIODS_PER_REV = 1.0f;


// ============================================================================
// ADC AVERAGING
// ============================================================================

const uint8_t ADC_SAMPLES = 4;


// ============================================================================
// DIFFERENTIAL ENCODER CALIBRATION
// ============================================================================
//
// Each individual Mega ADC input is 0...1023.
//
// We calculate:
//
//     COSraw = PCOS - NCOS
//     SINraw = PSIN - NSIN
//
// Because the differential result is used, the center is approximately
// zero after subtraction.
//
// Example:
//
//     PCOS = 700
//     NCOS = 300
//
//     COSraw = 400
//
// Similarly:
//
//     PSIN = 300
//     NSIN = 700
//
//     SINraw = -400
//
// ============================================================================


// Gain scaling.
//
// Start with 1.0.
//
// You can adjust this later if you want normalized amplitude.
//

const float COS_SCALE = 1.0f;
const float SIN_SCALE = 1.0f;


// ============================================================================
// CONSTANTS
// ============================================================================

const float TWO_PI_F = 6.28318530718f;


// ============================================================================
// RPM STATE
// ============================================================================

float previousAngle = 0.0f;

uint32_t previousTime = 0;

float rpmRaw = 0.0f;

float rpmFiltered = 0.0f;


// ============================================================================
// RPM FILTER
// ============================================================================

const uint8_t RPM_FILTER_SIZE = 8;

float rpmBuffer[RPM_FILTER_SIZE];

uint8_t rpmBufferIndex = 0;

uint8_t rpmBufferCount = 0;


// ============================================================================
// RELAY STATE
// ============================================================================

bool relayOn = false;


// ============================================================================
// BRAKE TIMER
// ============================================================================

bool brakeTimerRunning = false;

uint32_t brakeTimerStart = 0;


// ============================================================================
// SERIAL PRINT
// ============================================================================

uint32_t lastPrint = 0;

const uint32_t PRINT_INTERVAL_MS = 200;


// ============================================================================
// MESSAGE HELPERS
// ============================================================================
//
// Every human-readable threshold is printed from the constant that the
// control logic actually compares against. Change a constant and the log
// text follows automatically.
//

void printThrottleThreshold(float volts)
{
    Serial.print(volts, 2);
    Serial.print(F(" V"));
}


void printRpmThreshold()
{
    Serial.print(F("|RPM| < "));
    Serial.print(RPM_THRESHOLD, 0);
}


void printBrakeDelay()
{
    Serial.print(BRAKE_DELAY_MS);
    Serial.print(F(" ms"));
}


// ============================================================================
// ADC READING
// ============================================================================

uint16_t readADC(uint8_t pin)
{
    uint32_t sum = 0;

    for (uint8_t i = 0; i < ADC_SAMPLES; i++)
    {
        sum += analogRead(pin);
    }

    return (uint16_t)(sum / ADC_SAMPLES);
}


// ============================================================================
// READ THROTTLE VOLTAGE
// ============================================================================

float readThrottleVoltage()
{
    uint16_t raw =
        readADC(THROTTLE_PIN);


    float voltage =
        ((float)raw * ADC_VREF) /
        1023.0f;


    return voltage;
}


// ============================================================================
// READ DIFFERENTIAL ENCODER
// ============================================================================
//
// PCOS - NCOS = COS
//
// PSIN - NSIN = SIN
//
// ============================================================================

void readDifferentialEncoder(
    float &cosSignal,
    float &sinSignal
)
{
    uint16_t rawPCOS =
        readADC(PCOS_PIN);

    uint16_t rawPSIN =
        readADC(PSIN_PIN);

    uint16_t rawNCOS =
        readADC(NCOS_PIN);

    uint16_t rawNSIN =
        readADC(NSIN_PIN);


    // ------------------------------------------------------------------------
    // DIFFERENTIAL CALCULATION
    // ------------------------------------------------------------------------

    float differentialCOS =
        (float)rawPCOS -
        (float)rawNCOS;


    float differentialSIN =
        (float)rawPSIN -
        (float)rawNSIN;


    // ------------------------------------------------------------------------
    // SCALING
    // ------------------------------------------------------------------------

    cosSignal =
        differentialCOS *
        COS_SCALE;


    sinSignal =
        differentialSIN *
        SIN_SCALE;
}


// ============================================================================
// GET ENCODER ANGLE
// ============================================================================

float getEncoderAngle()
{
    float cosSignal;
    float sinSignal;


    readDifferentialEncoder(
        cosSignal,
        sinSignal
    );


    // ------------------------------------------------------------------------
    // ANGLE
    // ------------------------------------------------------------------------

    float angle =
        atan2f(
            sinSignal,
            cosSignal
        );


    // ------------------------------------------------------------------------
    // CONVERT -PI...+PI
    // TO
    // 0...2PI
    // ------------------------------------------------------------------------

    if (angle < 0.0f)
    {
        angle += TWO_PI_F;
    }


    return angle;
}


// ============================================================================
// RPM FILTER
// ============================================================================

float filterRPM(float rpm)
{
    rpmBuffer[rpmBufferIndex] =
        rpm;


    rpmBufferIndex++;


    if (rpmBufferIndex >= RPM_FILTER_SIZE)
    {
        rpmBufferIndex = 0;
    }


    if (rpmBufferCount < RPM_FILTER_SIZE)
    {
        rpmBufferCount++;
    }


    float sum = 0.0f;


    for (
        uint8_t i = 0;
        i < rpmBufferCount;
        i++
    )
    {
        sum += rpmBuffer[i];
    }


    return sum /
           (float)rpmBufferCount;
}


// ============================================================================
// UPDATE RPM
// ============================================================================

void updateRPM()
{
    uint32_t now =
        micros();


    float angle =
        getEncoderAngle();


    // ------------------------------------------------------------------------
    // FIRST SAMPLE
    // ------------------------------------------------------------------------

    if (previousTime == 0)
    {
        previousAngle =
            angle;

        previousTime =
            now;

        rpmRaw =
            0.0f;

        rpmFiltered =
            0.0f;

        return;
    }


    // ------------------------------------------------------------------------
    // TIME DIFFERENCE
    // ------------------------------------------------------------------------

    uint32_t dt_us =
        now -
        previousTime;


    if (dt_us == 0)
    {
        return;
    }


    float dt =
        (float)dt_us /
        1000000.0f;


    // ------------------------------------------------------------------------
    // ANGLE DIFFERENCE
    // ------------------------------------------------------------------------

    float deltaAngle =
        angle -
        previousAngle;


    // ------------------------------------------------------------------------
    // ANGLE UNWRAP
    // ------------------------------------------------------------------------

    if (deltaAngle > PI)
    {
        deltaAngle -= TWO_PI_F;
    }

    else if (deltaAngle < -PI)
    {
        deltaAngle += TWO_PI_F;
    }


    // ------------------------------------------------------------------------
    // ANGULAR VELOCITY
    // ------------------------------------------------------------------------

    float angularVelocity =
        deltaAngle /
        dt;


    // ------------------------------------------------------------------------
    // MECHANICAL SPEED
    // ------------------------------------------------------------------------

    float mechanicalAngularVelocity =
        angularVelocity /
        PERIODS_PER_REV;


    // ------------------------------------------------------------------------
    // RPM
    // ------------------------------------------------------------------------

    rpmRaw =
        mechanicalAngularVelocity *
        60.0f /
        TWO_PI_F;


    // ------------------------------------------------------------------------
    // FILTER
    // ------------------------------------------------------------------------

    rpmFiltered =
        filterRPM(
            rpmRaw
        );


    // ------------------------------------------------------------------------
    // SAVE
    // ------------------------------------------------------------------------

    previousAngle =
        angle;

    previousTime =
        now;
}


// ============================================================================
// RELAY CONTROL
// ============================================================================

void setRelay(bool state)
{
    relayOn =
        state;


    if (ACTIVE_LOW)
    {
        digitalWrite(
            RELAY_PIN,
            state ? LOW : HIGH
        );
    }
    else
    {
        digitalWrite(
            RELAY_PIN,
            state ? HIGH : LOW
        );
    }
}


// ============================================================================
// RELAY + BRAKE LOGIC
// ============================================================================

void updateRelayLogic(
    float throttleVoltage,
    float rpm
)
{
    // ========================================================================
    // RELAY OFF
    //
    // BRAKE ON
    // ========================================================================

    if (!relayOn)
    {
        brakeTimerRunning =
            false;


        // ------------------------------------------------------------
        // THROTTLE REACHES THROTTLE_ON_V
        // ------------------------------------------------------------

        if (
            throttleVoltage >=
            THROTTLE_ON_V
        )
        {
            setRelay(true);


            Serial.println();

            Serial.println(
                F("================================")
            );

            Serial.print(
                F("THROTTLE >= ")
            );

            printThrottleThreshold(
                THROTTLE_ON_V
            );

            Serial.println();

            Serial.println(
                F("RELAY ON")
            );

            Serial.println(
                F("BRAKE OFF")
            );

            Serial.println(
                F("================================")
            );

            Serial.println();
        }


        return;
    }


    // ========================================================================
    // RELAY ON
    //
    // BRAKE OFF
    // ========================================================================

    bool throttleLow =
        throttleVoltage <
        THROTTLE_OFF_V;


    bool rpmLow =
        fabsf(rpm) <
        RPM_THRESHOLD;


    bool lowCondition =
        throttleLow &&
        rpmLow;


    // ========================================================================
    // LOW RPM + LOW THROTTLE
    // ========================================================================

    if (lowCondition)
    {
        // ------------------------------------------------------------
        // START TIMER
        // ------------------------------------------------------------

        if (!brakeTimerRunning)
        {
            brakeTimerRunning =
                true;


            brakeTimerStart =
                millis();


            Serial.println();

            printRpmThreshold();

            Serial.print(
                F(" AND THROTTLE < ")
            );

            printThrottleThreshold(
                THROTTLE_OFF_V
            );

            Serial.println();


            Serial.print(
                F("Starting ")
            );

            printBrakeDelay();

            Serial.println(
                F(" timer...")
            );
        }


        // ------------------------------------------------------------
        // BRAKE_DELAY_MS ELAPSED
        // ------------------------------------------------------------

        if (
            millis() -
            brakeTimerStart
            >=
            BRAKE_DELAY_MS
        )
        {
            setRelay(false);


            brakeTimerRunning =
                false;


            Serial.println();

            Serial.println(
                F("================================")
            );

            Serial.println(
                F("BRAKE CONDITION COMPLETE")
            );

            printRpmThreshold();

            Serial.println();


            Serial.print(
                F("Throttle < ")
            );

            printThrottleThreshold(
                THROTTLE_OFF_V
            );

            Serial.println();


            Serial.print(
                F("held for ")
            );

            printBrakeDelay();

            Serial.println();

            Serial.println(
                F("RELAY OFF")
            );

            Serial.println(
                F("BRAKE ON")
            );

            Serial.println(
                F("================================")
            );

            Serial.println();
        }
    }


    // ========================================================================
    // CONDITION NOT SATISFIED
    // ========================================================================

    else
    {
        if (brakeTimerRunning)
        {
            brakeTimerRunning =
                false;


            Serial.print(
                F("Brake timer CANCELLED after ")
            );

            Serial.print(
                millis() -
                brakeTimerStart
            );

            Serial.println(
                F(" ms")
            );
        }
    }
}


// ============================================================================
// SETUP
// ============================================================================

void setup()
{
    Serial.begin(115200);


    // ------------------------------------------------------------------------
    // ENCODER PINS
    // ------------------------------------------------------------------------

    pinMode(
        PCOS_PIN,
        INPUT
    );

    pinMode(
        PSIN_PIN,
        INPUT
    );

    pinMode(
        NCOS_PIN,
        INPUT
    );

    pinMode(
        NSIN_PIN,
        INPUT
    );


    // ------------------------------------------------------------------------
    // THROTTLE
    // ------------------------------------------------------------------------

    pinMode(
        THROTTLE_PIN,
        INPUT
    );


    // ------------------------------------------------------------------------
    // RELAY
    // ------------------------------------------------------------------------

    pinMode(
        RELAY_PIN,
        OUTPUT
    );


    // ------------------------------------------------------------------------
    // INITIAL STATE
    //
    // RELAY OFF
    // BRAKE ON
    // ------------------------------------------------------------------------

    setRelay(false);


    // ------------------------------------------------------------------------
    // RPM FILTER
    // ------------------------------------------------------------------------

    for (
        uint8_t i = 0;
        i < RPM_FILTER_SIZE;
        i++
    )
    {
        rpmBuffer[i] =
            0.0f;
    }


    // ------------------------------------------------------------------------
    // STARTUP MESSAGE
    // ------------------------------------------------------------------------

    Serial.println();

    Serial.println(
        F("==========================================")
    );

    Serial.println(
        F("ARDUINO MEGA 2560")
    );

    Serial.println(
        F("4-CHANNEL DIFFERENTIAL SIN/COS")
    );

    Serial.println(
        F("RPM + THROTTLE + BRAKE RELAY")
    );

    Serial.println(
        F("==========================================")
    );

    Serial.println();

    Serial.println(
        F("PCOS = A0")
    );

    Serial.println(
        F("PSIN = A1")
    );

    Serial.println(
        F("NCOS = A2")
    );

    Serial.println(
        F("NSIN = A3")
    );

    Serial.println(
        F("Throttle = A4")
    );

    Serial.println(
        F("Relay = D7")
    );

    Serial.println();

    Serial.println(
        F("Initial: RELAY OFF / BRAKE ON")
    );

    Serial.println();

    Serial.print(
        F("Throttle >= ")
    );

    printThrottleThreshold(
        THROTTLE_ON_V
    );

    Serial.println(
        F("  ->  RELAY ON / BRAKE OFF")
    );


    printRpmThreshold();

    Serial.print(
        F(" AND throttle < ")
    );

    printThrottleThreshold(
        THROTTLE_OFF_V
    );

    Serial.println();


    Serial.print(
        F("held for ")
    );

    printBrakeDelay();

    Serial.println(
        F("  ->  RELAY OFF / BRAKE ON")
    );

    Serial.println();

    Serial.println(
        F("READY")
    );

    Serial.println();
}


// ============================================================================
// LOOP
// ============================================================================

void loop()
{
    // ------------------------------------------------------------------------
    // THROTTLE
    // ------------------------------------------------------------------------

    float throttleVoltage =
        readThrottleVoltage();


    // ------------------------------------------------------------------------
    // RPM
    // ------------------------------------------------------------------------

    updateRPM();


    // ------------------------------------------------------------------------
    // RELAY LOGIC
    // ------------------------------------------------------------------------

    updateRelayLogic(
        throttleVoltage,
        rpmFiltered
    );


    // ------------------------------------------------------------------------
    // SERIAL OUTPUT
    // ------------------------------------------------------------------------

    uint32_t now =
        millis();


    if (
        now -
        lastPrint
        >=
        PRINT_INTERVAL_MS
    )
    {
        lastPrint =
            now;


        Serial.print(
            F("Throttle = ")
        );

        Serial.print(
            throttleVoltage,
            3
        );

        Serial.print(
            F(" V")
        );


        Serial.print(
            F(" | RPM Raw = ")
        );

        Serial.print(
            rpmRaw,
            2
        );


        Serial.print(
            F(" | RPM = ")
        );

        Serial.print(
            rpmFiltered,
            2
        );


        Serial.print(
            F(" | Relay = ")
        );

        Serial.print(
            relayOn
            ? F("ON")
            : F("OFF")
        );


        Serial.print(
            F(" | Brake = ")
        );

        Serial.print(
            relayOn
            ? F("OFF")
            : F("ON")
        );


        if (brakeTimerRunning)
        {
            uint32_t elapsed =
                millis() -
                brakeTimerStart;


            Serial.print(
                F(" | TIMER = ")
            );

            Serial.print(
                elapsed
            );

            Serial.print(
                F("/")
            );

            Serial.print(
                BRAKE_DELAY_MS
            );

            Serial.print(
                F(" ms")
            );
        }


        Serial.println();
    }
}