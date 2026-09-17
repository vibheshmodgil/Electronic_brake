// ============================================================
// EBrakeCam - ESP32-S3 CAM board, a camera for the EBrake dashboard
// ============================================================
//
// A separate board from the brake controller. It joins the controller's
// access point (EBrake-Monitor) as an ordinary WiFi client with a fixed
// address, and serves JPEGs the dashboard page shows in its CAMERA card.
//
//   http://192.168.4.50/capture     one JPEG - what the dashboard polls
//   http://192.168.4.50:81/stream   MJPEG, one viewer at a time - the
//                                   dashboard's "full" button
//
// WHY A SECOND BOARD
// -------------------------------------------------------------------
// The camera bus on these boards uses GPIO 4-18, including 5, 6 and 7 -
// the controller's CAN TX, CAN RX and relay pins. And a camera driver on
// the brake board would share its memory, its WiFi and its reset with the
// safety path. This board has no wire to the brake, CAN or the relay; if
// it crashes, the only thing that stops is the picture.
//
// All three routes (/, /capture, :81/stream) are GET and only read the
// camera.
//
// BUILD (Arduino IDE, esp32 core 3.x)
//   Board             ESP32S3 Dev Module
//   PSRAM             OPI PSRAM
//   Flash Size        the module's (8MB or 16MB on most CAM boards)
//   Partition Scheme  Huge APP (3MB No OTA/1MB SPIFFS)
//   USB CDC On Boot   Enabled   (serial on the native USB port)
//
#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include "esp_http_server.h"

// Must match EBrakeCAN.ino: WEB_AP_SSID, WEB_AP_PASSWORD and CAM_HOST.
#define WIFI_SSID     "EBrake-Monitor"
#define WIFI_PASSWORD "brake1234"

const IPAddress CAM_IP(192, 168, 4, 50);   // above the AP's DHCP leases
const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress SUBNET(255, 255, 255, 0);

// ============================================================
// Camera pins - ESP32-S3 CAM / Freenove ESP32-S3-WROOM / ESP32-S3-EYE
// ============================================================
// If the serial port says "camera init failed", your board is wired
// differently: copy its block from the esp32 core's
// CameraWebServer/camera_pins.h.
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM  4
#define SIOC_GPIO_NUM  5
#define Y2_GPIO_NUM    11
#define Y3_GPIO_NUM    9
#define Y4_GPIO_NUM    8
#define Y5_GPIO_NUM    10
#define Y6_GPIO_NUM    12
#define Y7_GPIO_NUM    18
#define Y8_GPIO_NUM    17
#define Y9_GPIO_NUM    16
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM  7
#define PCLK_GPIO_NUM  13

// VGA is readable on a phone and still ~20-40 kB a frame. The frames go
// through the brake board's radio, so keep them small.
#define CAM_FRAME_SIZE   FRAMESIZE_VGA
#define CAM_JPEG_QUALITY 14   // 10 = best, 63 = worst

static bool cameraBegin()
{
  camera_config_t c = {};
  c.pin_pwdn = PWDN_GPIO_NUM;
  c.pin_reset = RESET_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_d0 = Y2_GPIO_NUM;
  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;
  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;
  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;
  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href = HREF_GPIO_NUM;
  c.pin_pclk = PCLK_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.ledc_timer = LEDC_TIMER_0;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.pixel_format = PIXFORMAT_JPEG;
  c.grab_mode = CAMERA_GRAB_LATEST;   // never serve a stale buffered frame

  if (psramFound())
  {
    c.frame_size = CAM_FRAME_SIZE;
    c.jpeg_quality = CAM_JPEG_QUALITY;
    c.fb_count = 2;
    c.fb_location = CAMERA_FB_IN_PSRAM;
  }
  else
  {
    Serial.println("No PSRAM - falling back to QVGA. Set Tools > PSRAM > OPI PSRAM.");
    c.frame_size = FRAMESIZE_QVGA;
    c.jpeg_quality = CAM_JPEG_QUALITY;
    c.fb_count = 1;
    c.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK)
  {
    Serial.printf("camera init failed: 0x%x - check the ribbon cable and the pin map\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  Serial.printf("camera sensor PID 0x%04x (OV2640=0x0026, OV3660=0x3660, OV5640=0x5640)\n",
                s->id.PID);

  if (s->id.PID == OV3660_PID)
  {
    s->set_vflip(s, 1);   // mounted upside down on most S3 boards
  }

  return true;
}

// ------------------------------------------------------------
// GET /capture - one JPEG
// ------------------------------------------------------------
static esp_err_t handleCapture(httpd_req_t *req)
{
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb)
  {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  // the dashboard is served from 192.168.4.1 and fetch()es this
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

// ------------------------------------------------------------
// GET :81/stream - MJPEG until the viewer leaves
// ------------------------------------------------------------
#define BOUNDARY "ebrakecamframe"

static esp_err_t handleStream(httpd_req_t *req)
{
  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" BOUNDARY);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  char head[80];

  for (;;)
  {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb)
    {
      return ESP_FAIL;
    }

    int n = snprintf(head, sizeof(head),
                     "\r\n--" BOUNDARY "\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                     (unsigned)fb->len);

    esp_err_t res = httpd_resp_send_chunk(req, head, n);
    if (res == ESP_OK)
    {
      res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    }
    esp_camera_fb_return(fb);

    if (res != ESP_OK)
    {
      return res;   // viewer closed the page
    }
  }
}

static esp_err_t handleRoot(httpd_req_t *req)
{
  static const char page[] =
      "<!DOCTYPE html><meta name=viewport content='width=device-width'>"
      "<body style='margin:0;background:#000'>"
      "<img style='width:100%' src='' id=i>"
      "<script>i.src='http://'+location.hostname+':81/stream'</script>";
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static void route(httpd_handle_t server, const char *uri,
                  esp_err_t (*handler)(httpd_req_t *))
{
  httpd_uri_t u = {};
  u.uri = uri;
  u.method = HTTP_GET;
  u.handler = handler;
  httpd_register_uri_handler(server, &u);
}

// A stream holds its server's only task for as long as it is open, so it
// gets its own server: /capture keeps answering the dashboard meanwhile.
static void serversBegin()
{
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  httpd_handle_t main = NULL;
  httpd_handle_t stream = NULL;

  if (httpd_start(&main, &cfg) == ESP_OK)
  {
    route(main, "/", handleRoot);
    route(main, "/capture", handleCapture);
  }

  cfg.server_port = 81;
  cfg.ctrl_port += 1;

  if (httpd_start(&stream, &cfg) == ESP_OK)
  {
    route(stream, "/stream", handleStream);
  }
}

void setup()
{
  Serial.begin(115200);
  delay(1000);   // native USB re-enumerates after reset
  Serial.println("\nEBrakeCam");

  if (!cameraBegin())
  {
    return;   // nothing to serve, so do not join the network either
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);   // power save adds 100+ ms per frame
  WiFi.config(CAM_IP, AP_IP, SUBNET);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  serversBegin();
}

void loop()
{
  if (WiFi.getMode() == WIFI_OFF)
  {
    delay(500);   // camera init failed in setup()
    return;
  }

  static wl_status_t last = WL_IDLE_STATUS;
  wl_status_t now = WiFi.status();

  if (now != last)
  {
    last = now;
    if (now == WL_CONNECTED)
    {
      Serial.printf("joined %s - http://%s/capture  http://%s:81/stream  RSSI %d dBm\n",
                    WIFI_SSID, CAM_IP.toString().c_str(), CAM_IP.toString().c_str(),
                    WiFi.RSSI());
    }
    else
    {
      Serial.printf("waiting for %s (is the brake board powered?)\n", WIFI_SSID);
    }
  }

  delay(500);
}
