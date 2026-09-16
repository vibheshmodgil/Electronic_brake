// ============================================================
// WebUI.h - WiFi access point + read-only telemetry server
// ============================================================
//
// Include this after ENABLE_WEB_UI and the WEB_AP_* settings are defined
// in EBrakeCAN.ino. The types it needs come from Telemetry.h, which it
// includes itself. It reads the telemetry; it never writes anything the
// brake logic depends on.
//
//
// WHY THIS CANNOT SLOW THE BRAKE DOWN
// -------------------------------------------------------------------
// The server runs in its own FreeRTOS task pinned to core 0, alongside
// the WiFi stack that Arduino already puts there. The brake loop is the
// Arduino loopTask, which stays on core 1 with a whole core to itself.
//
// So a stalled HTTP request, a phone that drops mid-transfer, five phones
// polling at once - none of it can delay twai_receive() or the apply
// timer. The two sides meet only at a spinlocked struct copy of about
// forty bytes.
//
// If the access point fails to start, webUiBegin() says so and returns.
// The controller then runs exactly as it would with the web UI compiled
// out. Nothing in the brake path waits on anything in this file.
//
//
// WHY IT IS READ-ONLY
// -------------------------------------------------------------------
// There are three routes: the page, a status document and an event list.
// All three are GET, and none of them touch the relay, the state machine
// or any calibration constant. There is deliberately no endpoint that can
// release the brake, and adding one would put a WiFi client in the safety
// path. Do not add one.
//
//
// RANGE IS NOT SECURITY
// -------------------------------------------------------------------
// The AP is WPA2 with the password below, which you should change. But a
// radio reaches past the walls of the room. Treat the web UI as bench
// instrumentation: compile it out with ENABLE_WEB_UI 0 for anything that
// matters, and never rely on it to be absent.
//
#pragma once

#if ENABLE_WEB_UI

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>

#include "Telemetry.h"
#include "WebPage.h"

static WebServer webServer(80);
static DNSServer dnsServer;

// ------------------------------------------------------------
// GET /api/status - the present moment, as JSON
// ------------------------------------------------------------
//
// The thresholds travel with the values. The page therefore draws the
// limits this firmware actually compares against, and cannot disagree
// with it the way a hardcoded copy would.
//
static void handleStatus()
{
  BrakeSnapshot s;
  snapshotGet(&s);

  char body[640];

  snprintf(
      body, sizeof(body),
      "{\"rpm\":%ld,\"s1\":%ld,\"s2\":%ld,"
      "\"tps\":%.2f,\"torque\":%.2f,"
      "\"relay\":%u,\"brake\":%u,"
      "\"timer\":%u,\"timerMs\":%lu,"
      "\"canOk\":%u,\"ageTps\":%ld,\"ageRpm\":%ld,"
      "\"uptime\":%lu,\"changes\":%lu,"
      "\"relTh\":%.2f,\"appTh\":%.2f,\"rpmTh\":%d,"
      "\"delayMs\":%lu,\"canTimeout\":%lu}",
      (long)s.rpm, (long)s.s1_mV, (long)s.s2_mV,
      s.tps, s.torque,
      (unsigned)s.relayOn, (unsigned)s.brakeApplied,
      (unsigned)s.timerRunning, (unsigned long)s.timerElapsedMs,
      (unsigned)s.canValid, (long)s.ageTpsMs, (long)s.ageRpmMs,
      (unsigned long)s.uptimeMs, (unsigned long)s.stateChanges,
      (double)TPS_RELEASE_THRESHOLD_PERCENT,
      (double)TPS_APPLY_THRESHOLD_PERCENT,
      (int)RPM_APPLY_THRESHOLD,
      (unsigned long)BRAKE_APPLY_DELAY_MS,
      (unsigned long)CAN_TIMEOUT_MS);

  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(200, "application/json", body);
}

// ------------------------------------------------------------
// GET /api/events - the firmware's own messages, oldest first
// ------------------------------------------------------------
static void handleEvents()
{
  char text[EVENT_SLOTS][EVENT_TEXT_MAX];
  uint32_t when[EVENT_SLOTS];

  uint8_t n = eventsSnapshot(text, when, EVENT_SLOTS);

  String body = "[";

  for (uint8_t i = 0; i < n; i++)
  {
    if (i)
    {
      body += ',';
    }

    body += "{\"t\":";
    body += when[i];
    body += ",\"m\":\"";

    // JSON string escaping. The messages are plain ASCII today, but a
    // future one containing a quote must not be able to break the
    // document the page parses.
    for (const char *p = text[i]; *p; p++)
    {
      if (*p == '"' || *p == '\\')
      {
        body += '\\';
        body += *p;
      }
      else if ((uint8_t)*p >= 0x20)
      {
        body += *p;
      }
    }

    body += "\"}";
  }

  body += ']';

  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(200, "application/json", body);
}

static void handleRoot()
{
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send_P(200, "text/html", DASHBOARD_HTML);
}

// Any other address lands on the dashboard, which is what makes the
// phone's "sign in to this network" prompt open straight to it.
static void handleNotFound()
{
  webServer.sendHeader("Location", "/", true);
  webServer.send(302, "text/plain", "");
}

// ------------------------------------------------------------
// The server task - core 0, never touches the brake
// ------------------------------------------------------------
static void webUiTask(void *arg)
{
  (void)arg;

  for (;;)
  {
    dnsServer.processNextRequest();
    webServer.handleClient();

    // Yield every pass. Without this the task never blocks and starves
    // the idle task on this core, which trips the watchdog.
    vTaskDelay(1);
  }
}

// ------------------------------------------------------------
// Start it. Failure here is not fatal to the controller.
// ------------------------------------------------------------
static void webUiBegin()
{
  WiFi.mode(WIFI_AP);

  bool up = WiFi.softAP(
      WEB_AP_SSID,
      WEB_AP_PASSWORD,
      WEB_AP_CHANNEL,
      0,                    // not hidden
      WEB_AP_MAX_CLIENTS);

  if (!up)
  {
    Serial.println(
        "SoftAP failed to start - continuing WITHOUT the web UI");
    return;
  }

  IPAddress ip = WiFi.softAPIP();

  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", ip);

  webServer.on("/", HTTP_GET, handleRoot);
  webServer.on("/api/status", HTTP_GET, handleStatus);
  webServer.on("/api/events", HTTP_GET, handleEvents);
  webServer.onNotFound(handleNotFound);
  webServer.begin();

  BaseType_t made = xTaskCreatePinnedToCore(
      webUiTask,
      "ebrake-web",
      8192,
      NULL,
      1,
      NULL,
      WEB_TASK_CORE);

  if (made != pdPASS)
  {
    Serial.println(
        "Web task would not start - continuing WITHOUT the web UI");
    return;
  }

  Serial.println();
  Serial.println("Web UI ready");
  Serial.printf("  join WiFi : %s\n", WEB_AP_SSID);
  Serial.printf("  password  : %s\n", WEB_AP_PASSWORD);
  Serial.printf("  browse to : http://%s/\n", ip.toString().c_str());
  Serial.println("  read-only - no route here can move the brake");
  Serial.println();
}

#else   // ENABLE_WEB_UI

static inline void webUiBegin() {}

#endif  // ENABLE_WEB_UI
