#include <Arduino.h>
#include "esp_camera.h"
#include <WiFi.h>
#include <WebServer.h>          // <<< ADDED : small server for the alert buttons

// ===========================
// Select camera model in board_config.h
// ===========================
#include "board_config.h"

// ===========================
// Enter your WiFi credentials
// ===========================
const char *ssid = "DoorTest";
const char *password = "12345678";

void startCameraServer();
void setupLedFlash();


/* =========================================================================
   ================  ADDED SECTION : ALERT LINE TO ARDUINO  ================
   =========================================================================

   GPIO21 is CONFIRMED FREE on your board.
   Your camera model is CAMERA_MODEL_ESP32S3_EYE, which uses these pins:
       15, 4, 5, 11, 9, 8, 10, 12, 18, 17, 16, 6, 7, 13
   GPIO21 is not in that list, and no flash LED is defined. Safe to use.

   WIRING:  ESP32 GPIO21 -> Arduino D7
            ESP32 GND    -> Arduino GND

   LOGIC:   wire released (HIGH) = no alert
            wire pulled down (LOW) = ALERT
            This is on purpose. LOW means alert.
   ========================================================================= */

#define ALERT_PIN        21
#define ALERT_HOLD_MS    3000UL     // how long each alert lasts

// Set this to 1 if you want the camera to fire alerts by itself.
// Leave it at 0 for a manual/button-triggered demo.
#define ENABLE_MOTION_DETECT 0

WebServer alertServer(8080);        // buttons live at http://<ip>:8080

bool alertActive = false;
unsigned long alertStartTime = 0;


// Call this ONCE, at the very start of setup()
void alertPinInit() {
  pinMode(ALERT_PIN, OUTPUT_OPEN_DRAIN);
  digitalWrite(ALERT_PIN, HIGH);          // released = no alert
}

void raiseAlert() {
  digitalWrite(ALERT_PIN, LOW);           // pull the wire down = ALERT
  alertActive = true;
  alertStartTime = millis();
  Serial.println("ALERT sent to Arduino (GPIO21 pulled LOW)");
}

void clearAlert() {
  digitalWrite(ALERT_PIN, HIGH);          // let go of the wire
  alertActive = false;
  Serial.println("Alert cleared (GPIO21 released)");
}

// Automatically ends the alert after ALERT_HOLD_MS. Never blocks.
void updateAlert() {
  if (alertActive && (millis() - alertStartTime > ALERT_HOLD_MS)) {
    clearAlert();
  }
}


/* ---------------- the little web page with two buttons ---------------- */

void handleAlertRoot() {
  String page = F(
    "<!DOCTYPE html><html><head><meta name='viewport' "
    "content='width=device-width,initial-scale=1'>"
    "<title>Door Camera Alert</title></head>"
    "<body style='font-family:sans-serif;text-align:center;padding:40px'>"
    "<h2>Door Camera Alert</h2>"
    "<p>Status: ");
  page += alertActive ? F("<b style='color:red'>ALERT ACTIVE</b>")
                      : F("<b style='color:green'>normal</b>");
  page += F(
    "</p>"
    "<p><a href='/alert'><button style='font-size:24px;padding:20px 40px;"
    "background:#c00;color:#fff;border:none;border-radius:8px'>"
    "SEND ALERT</button></a></p>"
    "<p><a href='/clear'><button style='font-size:18px;padding:12px 24px'>"
    "Clear</button></a></p>"
    "</body></html>");
  alertServer.send(200, "text/html", page);
}

void handleAlertOn() {
  raiseAlert();
  alertServer.sendHeader("Location", "/");
  alertServer.send(302, "text/plain", "");
}

void handleAlertOff() {
  clearAlert();
  alertServer.sendHeader("Location", "/");
  alertServer.send(302, "text/plain", "");
}


/* ---------------- optional: let the camera trigger itself ------------- */
#if ENABLE_MOTION_DETECT
unsigned long lastMotionCheck = 0;
size_t lastFrameSize = 0;

void checkMotion() {
  if (millis() - lastMotionCheck < 500) return;   // check twice per second
  lastMotionCheck = millis();

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return;
  size_t thisSize = fb->len;
  esp_camera_fb_return(fb);

  if (lastFrameSize > 0) {
    long diff = (long)thisSize - (long)lastFrameSize;
    if (diff < 0) diff = -diff;
    // a big change in JPEG size usually means the scene changed
    if (diff > (long)(lastFrameSize / 5) && !alertActive) {
      Serial.println("Camera saw a big scene change");
      raiseAlert();
    }
  }
  lastFrameSize = thisSize;
}
#endif

/* ==================  END OF ADDED SECTION  ============================= */


void setup() {
  Serial.begin(115200);

  // <<< CHANGED : was pinMode(21, OUTPUT); digitalWrite(21, LOW);
  //     That old version meant "alert is ON" permanently under the new logic.
  alertPinInit();

  Serial.setDebugOutput(true);
  Serial.println();

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size = FRAMESIZE_UXGA;
  config.pixel_format = PIXFORMAT_JPEG;  // for streaming
  //config.pixel_format = PIXFORMAT_RGB565; // for face detection/recognition
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count = 1;

  // if PSRAM IC present, init with UXGA resolution and higher JPEG quality
  //                      for larger pre-allocated frame buffer.
  if (config.pixel_format == PIXFORMAT_JPEG) {
    if (psramFound()) {
      config.jpeg_quality = 10;
      config.fb_count = 2;
      config.grab_mode = CAMERA_GRAB_LATEST;
    } else {
      // Limit the frame size when PSRAM is not available
      config.frame_size = FRAMESIZE_SVGA;
      config.fb_location = CAMERA_FB_IN_DRAM;
    }
  } else {
    // Best option for face detection/recognition
    config.frame_size = FRAMESIZE_240X240;
#if CONFIG_IDF_TARGET_ESP32S3
    config.fb_count = 2;
#endif
  }

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

  // camera init
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  sensor_t *s = esp_camera_sensor_get();
  // initial sensors are flipped vertically and colors are a bit saturated
  if (s->id.PID == OV3660_PID) {
    s->set_vflip(s, 1);        // flip it back
    s->set_brightness(s, 1);   // up the brightness just a bit
    s->set_saturation(s, -2);  // lower the saturation
  }
  // drop down frame size for higher initial frame rate
  if (config.pixel_format == PIXFORMAT_JPEG) {
    s->set_framesize(s, FRAMESIZE_QVGA);
  }

#if defined(CAMERA_MODEL_M5STACK_WIDE) || defined(CAMERA_MODEL_M5STACK_ESP32CAM)
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);
#endif

#if defined(CAMERA_MODEL_ESP32S3_EYE)
  s->set_vflip(s, 1);
#endif

// Setup LED FLash if LED pin is defined in camera_pins.h
#if defined(LED_GPIO_NUM)
  setupLedFlash();
#endif

  WiFi.begin(ssid, password);
  WiFi.setSleep(false);

  Serial.print("WiFi connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected");

  startCameraServer();

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");

  // <<< ADDED : start the alert button page
  alertServer.on("/",      handleAlertRoot);
  alertServer.on("/alert", handleAlertOn);
  alertServer.on("/clear", handleAlertOff);
  alertServer.begin();

  Serial.print("Alert buttons at 'http://");
  Serial.print(WiFi.localIP());
  Serial.println(":8080'");
  Serial.println("Or type 'a' in this Serial Monitor to send an alert.");
}


void loop() {

  // <<< CHANGED : was just delay(10000), which meant the alert never fired.
  alertServer.handleClient();
  updateAlert();

  // type 'a' to alert, 'c' to clear
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'a' || c == 'A') raiseAlert();
    if (c == 'c' || c == 'C') clearAlert();
  }

#if ENABLE_MOTION_DETECT
  checkMotion();
#endif

  delay(2);
}
