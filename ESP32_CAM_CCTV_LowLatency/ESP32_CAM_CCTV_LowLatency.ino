/*
   ============================================================
   โปรเจกต์ : ESP32-CAM กล้องวงจรปิดผ่าน Browser — ปรับลดภาพดีเลย์
   บอร์ด     : AI Thinker ESP32-CAM
   โหมด WiFi : Access Point (ESP32-CAM ปล่อย WiFi เอง)
   วิธีใช้   : เปิดไฟล์นี้ใน Arduino IDE เลือกบอร์ด AI Thinker ESP32-CAM
              ใช้แพ็กเกจ esp32 by Espressif Systems (มีไลบรารีกล้องมาให้)
              อัปโหลดทั้งไฟล์ แล้วเชื่อม WiFi และเปิด http://192.168.4.1
   ค่าเริ่มต้น: QVGA 320x240 / JPEG quality 20 / ส่งไม่เกินประมาณ 15 FPS
   ทดสอบก่อน : ดูจากมือถือ 1 เครื่อง เปิดหน้ากล้องเพียง 1 แท็บ อยู่ใกล้บอร์ด
   หมายเหตุ  : FPS และเวลาหน่วงจริงขึ้นกับ WiFi แสง กล้อง และเบราว์เซอร์
              ยังไม่ได้คอมไพล์ด้วยชุดเครื่องมือ ESP32 หรือทดสอบบอร์ดจริง
   อ้างอิง  : https://github.com/espressif/esp32-camera
              https://docs.espressif.com/projects/esp-faq/en/latest/application-solution/camera-application.html
   ============================================================
*/

#include <Arduino.h>
#include "esp_camera.h"
#include "esp_http_server.h"
#include <WiFi.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

// ============================================================
// ตั้งค่า WiFi ที่ ESP32-CAM จะปล่อยออกมาเอง
// ============================================================
const char *AP_SSID = "ESP32-CAM-CCTV350";
const char *AP_PASSWORD = "12345678";

// กำหนด IP Address
IPAddress local_IP(192, 168, 4, 1);
IPAddress gateway(192, 168, 4, 1);
IPAddress subnet(255, 255, 255, 0);

// JPEG: ตัวเลขมากขึ้น = บีบอัดมากขึ้น ภาพเล็กลง แต่รายละเอียดลดลง
const int JPEG_QUALITY = 20;
const uint32_t STREAM_MAX_FPS = 15;
const uint32_t STREAM_FRAME_INTERVAL_MS = (1000 + STREAM_MAX_FPS - 1) / STREAM_MAX_FPS;
const uint8_t AP_CHANNEL = 1;  // ถ้ามีสัญญาณรบกวน ลองเปลี่ยนเป็น 6 หรือ 11
bool hasPSRAM = false;
framesize_t currentFrameSize = FRAMESIZE_QVGA;
SemaphoreHandle_t cameraMutex = NULL;  // กันถ่ายภาพ/เปลี่ยนขนาด/สตรีมใช้กล้องชนกัน

// กำหนดขา GPIO ของกล้อง AI Thinker
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

#define FLASH_LED_PIN 4
bool flashState = false;

httpd_handle_t camera_httpd = NULL;
httpd_handle_t stream_httpd = NULL;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART = "Content-Type: image/jpeg\r\n"
                                 "Content-Length: %u\r\n\r\n";

// HTML Web Page
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="th">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0">
<title>ESP32-CAM CCTV</title>
<style>
*{box-sizing:border-box;}
body{margin:0;padding:0;background:#090c10;color:#ffffff;font-family:Arial,Helvetica,sans-serif;}
.header{background:#111820;padding:18px 15px;text-align:center;border-bottom:1px solid #26303b;}
.title{font-size:24px;font-weight:bold;}
.subtitle{margin-top:6px;color:#8e9aa7;font-size:13px;}
.live{color:#36e887;font-weight:bold;margin-top:8px;}
.container{width:100%;max-width:900px;margin:auto;padding:15px;}
.camera-box{width:100%;background:#000;border:1px solid #29313c;border-radius:14px;overflow:hidden;position:relative;}
.camera{width:100%;display:block;min-height:220px;object-fit:contain;background:#000;}
.live-badge{position:absolute;top:12px;left:12px;padding:6px 10px;border-radius:6px;background:rgba(220,30,30,0.85);font-size:12px;font-weight:bold;}
.panel{background:#111820;border:1px solid #26303b;border-radius:14px;margin-top:15px;padding:15px;}
.panel-title{font-size:14px;color:#9eabb8;margin-bottom:10px;}
.button-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px;}
button{width:100%;padding:14px;border:none;border-radius:9px;font-size:15px;font-weight:bold;cursor:pointer;background:#263241;color:white;}
button:active{transform:scale(0.98);}
.btn-flash{background:#d99619;}
.btn-capture{background:#1679d3;}
.btn-restart{background:#b43a3a;}
select{width:100%;padding:13px;border-radius:9px;background:#202a35;color:white;border:1px solid #364554;font-size:15px;}
.info{font-size:13px;color:#8e9aa7;line-height:1.8;}
.status{color:#36e887;}
.footer{text-align:center;color:#586575;font-size:12px;padding:20px;}
@media(max-width:500px){.button-grid{grid-template-columns:1fr;}.title{font-size:21px;}}
</style>
</head>
<body>
<div class="header">
    <div class="title">ESP32-CAM CCTV</div>
    <div class="subtitle">Standalone WiFi Camera • Low Latency</div>
    <div class="live">● เริ่มต้นด้วยโหมดภาพไว QVGA</div>
</div>
<div class="container">
    <div class="camera-box">
        <div class="live-badge">● LIVE</div>
        <img id="stream" class="camera">
    </div>
    <div class="panel">
        <div class="panel-title">CAMERA CONTROL</div>
        <div class="button-grid">
            <button id="flashButton" class="btn-flash" onclick="toggleFlash()">💡 เปิดไฟ Flash</button>
            <button class="btn-capture" onclick="capturePhoto()">📷 ถ่ายภาพ</button>
            <button onclick="reconnectStream()">↻ เชื่อมต่อภาพใหม่</button>
        </div>
    </div>
    <div class="panel">
        <div class="panel-title">RESOLUTION</div>
        <select id="resolution" onchange="changeResolution()">
            <option value="QVGA" selected>QVGA - 320 × 240 • เน้นภาพไว</option>
            <option value="VGA">VGA - 640 × 480 • ชัดขึ้น</option>
            <option value="SVGA">SVGA - 800 × 600 • อาจหน่วงขึ้น</option>
            <option value="XGA">XGA - 1024 × 768 • อาจหน่วงขึ้น</option>
            <option value="SXGA">SXGA - 1280 × 1024 • อาจหน่วงขึ้น</option>
        </select>
        <div class="info">เริ่มทดสอบด้วย QVGA และเปิดดูเพียงเครื่องเดียว/แท็บเดียว หากภาพตามไม่ทันให้กดเชื่อมต่อภาพใหม่</div>
    </div>
    <div class="panel">
        <div class="panel-title">SYSTEM INFORMATION</div>
        <div class="info">
            WiFi : <b>ESP32-CAM-CCTV20</b><br>
            Camera IP : <b>192.168.4.1</b><br>
            Stream : <b>192.168.4.1:81/stream</b><br>
            Status : <span id="status" class="status">กำลังเชื่อมต่อ...</span>
        </div>
    </div>
    <div class="panel">
        <button class="btn-restart" onclick="restartESP()">↻ Restart ESP32-CAM</button>
    </div>
</div>
<div class="footer">ESP32-CAM Standalone CCTV</div>

<script>
let flashState = false;
let changingResolution = false;
let streamWanted = false;
let streamTimer = null;
let lastResolution = 'QVGA';
const blankImage = 'data:image/gif;base64,R0lGODlhAQABAAD/ACwAAAAAAQABAAACADs=';
const streamImage = document.getElementById('stream');
const statusText = document.getElementById('status');

async function request(url){
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 12000);
    try {
        const response = await fetch(url, {cache:'no-store', signal:controller.signal});
        const body = await response.text();
        if(!response.ok) throw new Error(body || ('HTTP ' + response.status));
        return body;
    } finally { clearTimeout(timer); }
}
function stopStream(){
    clearTimeout(streamTimer);
    streamWanted = false;
    streamImage.src = blankImage;
}
function startStream(){
    clearTimeout(streamTimer);
    if(document.hidden || changingResolution) return;
    streamWanted = true;
    statusText.textContent = 'กำลังรับภาพ...';
    streamImage.src = 'http://' + window.location.hostname + ':81/stream?t=' + Date.now();
}
function reconnectStream(){
    stopStream();
    if(!document.hidden && !changingResolution){
        streamTimer = setTimeout(startStream, 250);
    }
}
streamImage.onerror = function(){
    if(!streamWanted || document.hidden || changingResolution) return;
    statusText.textContent = 'ภาพขาด กำลังเชื่อมต่อใหม่...';
    clearTimeout(streamTimer);
    streamTimer = setTimeout(startStream, 1500);
};
function showFlash(){
    document.getElementById('flashButton').textContent = flashState ? '💡 ปิดไฟ Flash' : '💡 เปิดไฟ Flash';
}
async function toggleFlash(){
    const button = document.getElementById('flashButton');
    button.disabled = true;
    try {
        flashState = (await request('/flash?state=' + (flashState ? '0' : '1'))) === 'ON';
        showFlash();
    } catch(error){ alert('ควบคุมไฟไม่สำเร็จ: ' + error.message); }
    finally { button.disabled = false; }
}
function capturePhoto(){ window.open("/capture?t=" + Date.now(), "_blank"); }
async function loadStatus(){
    const data = JSON.parse(await request('/status'));
    lastResolution = data.resolution;
    const select = document.getElementById('resolution');
    select.value = lastResolution;
    for(const option of select.options){
        option.disabled = !data.psram && option.value !== 'QVGA';
    }
    flashState = data.flash;
    showFlash();
}
async function changeResolution(){
    if(changingResolution) return;
    const select = document.getElementById('resolution');
    changingResolution = true;
    select.disabled = true;
    stopStream();
    statusText.textContent = 'กำลังเปลี่ยนขนาดภาพ...';
    try {
        await request('/resolution?size=' + encodeURIComponent(select.value));
        lastResolution = select.value;
    } catch(error){
        select.value = lastResolution;
        alert('เปลี่ยนขนาดไม่สำเร็จ: ' + error.message);
        try { await loadStatus(); } catch(ignored) {}
    } finally {
        changingResolution = false;
        select.disabled = false;
        reconnectStream();
    }
}
async function restartESP(){
    if(!confirm('ต้องการ Restart ESP32-CAM หรือไม่?')) return;
    stopStream();
    statusText.textContent = 'กำลังรีสตาร์ต เมื่อ WiFi กลับมาให้รีเฟรชหน้านี้';
    try { await request('/restart'); }
    catch(error){ statusText.textContent = 'การเชื่อมต่อขาด ตรวจ WiFi แล้วรีเฟรชหน้านี้'; }
}
// หยุดรับภาพเมื่อซ่อนแท็บ และเริ่มการเชื่อมต่อใหม่เมื่อกลับมาดู
document.addEventListener('visibilitychange', function(){
    if(document.hidden) stopStream();
    else reconnectStream();
});
window.addEventListener('pagehide', stopStream);
window.addEventListener('pageshow', function(event){ if(event.persisted) reconnectStream(); });
window.onload = async function(){
    changingResolution = true;
    try { await loadStatus(); }
    catch(error){ statusText.textContent = 'อ่านสถานะไม่ได้ กำลังลองเปิดภาพ...'; }
    finally { changingResolution = false; startStream(); }
};
</script>
</body>
</html>
)rawliteral";

static esp_err_t indexHandler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
  return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

// ============================================================
// ฟังก์ชัน Video Streaming: ส่ง JPEG โดยตรงและจำกัดอัตราส่ง
// CAMERA_GRAB_LATEST ช่วยจัดการคิวฝั่งกล้อง แต่ล้างข้อมูลที่ส่งเข้า TCP ไปแล้วไม่ได้
// หากเบราว์เซอร์แสดงภาพเก่าค้าง ให้ใช้ปุ่มเชื่อมต่อภาพใหม่เพื่อปิดสตรีมเดิม
// ============================================================
static esp_err_t streamHandler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  char partBuffer[64];

  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) { return res; }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
  httpd_resp_set_hdr(req, "Pragma", "no-cache");

  uint32_t reportStart = millis();
  uint32_t reportFrames = 0;
  uint32_t reportBytes = 0;
  uint32_t reportSendMs = 0;
  Serial.println("[STREAM] Viewer connected. Use one viewer/tab for testing.");

  while (true) {
    const uint32_t frameStart = millis();
    if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
      res = ESP_FAIL;
      break;
    }
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    } else if (fb->format != PIXFORMAT_JPEG) {
      esp_camera_fb_return(fb);
      fb = NULL;
      res = ESP_FAIL;
    } else {
      const size_t jpegBytes = fb->len;
      const uint32_t sendStart = millis();
      res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
      if (res == ESP_OK) {
        const int hlen = snprintf(partBuffer, sizeof(partBuffer), STREAM_PART, (unsigned)fb->len);
        if (hlen <= 0 || (size_t)hlen >= sizeof(partBuffer)) res = ESP_FAIL;
        else res = httpd_resp_send_chunk(req, partBuffer, hlen);
      }
      if (res == ESP_OK) {
        res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
      }
      esp_camera_fb_return(fb);
      fb = NULL;
      if (res == ESP_OK) {
        reportFrames++;
        reportBytes += jpegBytes;
        reportSendMs += millis() - sendStart;
      }
    }
    xSemaphoreGive(cameraMutex);

    if (res != ESP_OK) { break; }

    // พิมพ์สถิติทุก 5 วินาที: send_ms คือเวลาที่บอร์ดใช้ส่ง ไม่ใช่ดีเลย์ถึงจอมือถือ
    const uint32_t reportElapsed = millis() - reportStart;
    if (reportElapsed >= 5000 && reportFrames > 0) {
      Serial.printf("[STREAM] fps=%.1f jpeg=%.1f KB send=%.1f ms/frame\n",
                    reportFrames * 1000.0 / reportElapsed,
                    reportBytes / (1024.0 * reportFrames),
                    reportSendMs / (double)reportFrames);
      reportStart = millis();
      reportFrames = reportBytes = reportSendMs = 0;
    }

    // จำกัดอัตราส่งไว้ประมาณ 15 FPS เพื่อลดภาระ WiFi/เบราว์เซอร์
    // ไม่ไล่ส่งชดเชยเฟรมที่ช้า และพักอย่างน้อย 1 tick ให้งานระบบทำงาน
    const uint32_t elapsed = millis() - frameStart;
    const uint32_t waitMs = elapsed < STREAM_FRAME_INTERVAL_MS ? STREAM_FRAME_INTERVAL_MS - elapsed : 1;
    // ปัดขึ้นเป็น tick เพื่อไม่ให้การปัดเศษทำให้อัตราส่งสูงกว่าที่ตั้งไว้
    const TickType_t ticks = pdMS_TO_TICKS(waitMs + portTICK_PERIOD_MS - 1);
    vTaskDelay(ticks > 0 ? ticks : 1);
  }
  Serial.println("[STREAM] Connection closed.");
  return res;
}

static esp_err_t captureHandler(httpd_req_t *req) {
  if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "Camera busy. Please try again.");
  }
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    xSemaphoreGive(cameraMutex);
    Serial.println("Camera capture failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=ESP32-CAM.jpg");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
  esp_err_t result = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  xSemaphoreGive(cameraMutex);
  return result;
}

static esp_err_t flashHandler(httpd_req_t *req) {
  char query[32];
  char value[8];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    if (httpd_query_key_value(query, "state", value, sizeof(value)) == ESP_OK) {
      if (atoi(value) == 1) {
        flashState = true;
        digitalWrite(FLASH_LED_PIN, HIGH);
      } else {
        flashState = false;
        digitalWrite(FLASH_LED_PIN, LOW);
      }
    }
  }
  httpd_resp_set_type(req, "text/plain");
  return httpd_resp_sendstr(req, flashState ? "ON" : "OFF");
}

static esp_err_t resolutionHandler(httpd_req_t *req) {
  char query[64];
  char sizeValue[20];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) { return httpd_resp_send_404(req); }
  if (httpd_query_key_value(query, "size", sizeValue, sizeof(sizeValue)) != ESP_OK) { return httpd_resp_send_404(req); }

  framesize_t frameSize;

  if (strcmp(sizeValue, "QVGA") == 0) frameSize = FRAMESIZE_QVGA;
  else if (strcmp(sizeValue, "VGA") == 0) frameSize = FRAMESIZE_VGA;
  else if (strcmp(sizeValue, "SVGA") == 0) frameSize = FRAMESIZE_SVGA;
  else if (strcmp(sizeValue, "XGA") == 0) frameSize = FRAMESIZE_XGA;
  else if (strcmp(sizeValue, "SXGA") == 0) frameSize = FRAMESIZE_SXGA;
  else return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown resolution");

  if (!hasPSRAM && frameSize != FRAMESIZE_QVGA) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No PSRAM: use QVGA");
  }
  if (xSemaphoreTake(cameraMutex, pdMS_TO_TICKS(3000)) != pdTRUE) {
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "Camera busy. Please reconnect and try again.");
  }
  sensor_t *sensor = esp_camera_sensor_get();
  const int result = sensor ? sensor->set_framesize(sensor, frameSize) : -1;
  if (result == 0) {
    currentFrameSize = frameSize;
    // ทิ้งเฟรมที่อาจค้างจากขนาดเดิมก่อนเริ่มส่งภาพรอบใหม่
    for (int i = 0; i < 2; i++) {
      camera_fb_t *oldFrame = esp_camera_fb_get();
      if (!oldFrame) break;
      esp_camera_fb_return(oldFrame);
    }
  }
  xSemaphoreGive(cameraMutex);
  if (result != 0) {
    return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot change resolution");
  }
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, "OK");
}

static esp_err_t statusHandler(httpd_req_t *req) {
  const char *sizeName = "QVGA";
  if (currentFrameSize == FRAMESIZE_VGA) sizeName = "VGA";
  else if (currentFrameSize == FRAMESIZE_SVGA) sizeName = "SVGA";
  else if (currentFrameSize == FRAMESIZE_XGA) sizeName = "XGA";
  else if (currentFrameSize == FRAMESIZE_SXGA) sizeName = "SXGA";
  char json[128];
  snprintf(json, sizeof(json), "{\"resolution\":\"%s\",\"psram\":%s,\"flash\":%s}",
           sizeName, hasPSRAM ? "true" : "false", flashState ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, json);
}

static esp_err_t restartHandler(httpd_req_t *req) {
  httpd_resp_sendstr(req, "Restarting...");
  delay(500);
  ESP.restart();
  return ESP_OK;
}
// ============================================================
// ฟังก์ชันเริ่ม Web Server (ปรับแต่ง Config เพิ่มความเสถียร)
// ============================================================
void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  // เลิกค้างรอการส่งเมื่อเครือข่ายไม่รับข้อมูลนานเกินกำหนด
  // timeout ไม่ได้เป็นการรับประกันดีเลย์สูงสุดของภาพบนหน้าจอ
  config.lru_purge_enable = true;
  config.send_wait_timeout = 2;
  config.recv_wait_timeout = 5;
  config.server_port = 80;
  httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = indexHandler, .user_ctx = NULL };
  httpd_uri_t capture_uri = { .uri = "/capture", .method = HTTP_GET, .handler = captureHandler, .user_ctx = NULL };
  httpd_uri_t flash_uri = { .uri = "/flash", .method = HTTP_GET, .handler = flashHandler, .user_ctx = NULL };
  httpd_uri_t resolution_uri = { .uri = "/resolution", .method = HTTP_GET, .handler = resolutionHandler, .user_ctx = NULL };
  httpd_uri_t restart_uri = { .uri = "/restart", .method = HTTP_GET, .handler = restartHandler, .user_ctx = NULL };
  httpd_uri_t status_uri = { .uri = "/status", .method = HTTP_GET, .handler = statusHandler, .user_ctx = NULL };
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &flash_uri);
    httpd_register_uri_handler(camera_httpd, &resolution_uri);
    httpd_register_uri_handler(camera_httpd, &restart_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
  } else {
    Serial.println("ERROR: Cannot start web server on port 80.");
  }
  config.server_port = 81;
  config.ctrl_port += 1;
  httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = streamHandler, .user_ctx = NULL };
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  } else {
    Serial.println("ERROR: Cannot start stream server on port 81.");
  }
}
bool setupCamera() {
  // เคลียร์ทุกช่องก่อนตั้งค่า ป้องกันสมาชิกที่ไม่ได้กำหนดมีค่าขยะ
  camera_config_t config = {};
  hasPSRAM = psramFound();
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
  config.pixel_format = PIXFORMAT_JPEG;
  config.jpeg_quality = JPEG_QUALITY;
  if (hasPSRAM) {
    // จองบัฟเฟอร์รองรับขนาดสูงสุดในเมนู แล้วสั่งกล้องเริ่มที่ QVGA ด้านล่าง
    // ไม่เริ่มส่ง SXGA จริง และไม่เพิ่มความละเอียดเกินขนาดที่จองไว้
    config.frame_size = FRAMESIZE_SXGA;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_LATEST;
  } else {
    config.frame_size = FRAMESIZE_QVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count = 1;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  }
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", (unsigned)err);
    return false;
  }
  sensor_t *sensor = esp_camera_sensor_get();
  if (!sensor || sensor->set_framesize(sensor, FRAMESIZE_QVGA) != 0) {
    Serial.println("Cannot set QVGA camera mode.");
    esp_camera_deinit();
    return false;
  }
  currentFrameSize = FRAMESIZE_QVGA;
  sensor->set_brightness(sensor, 0);
  sensor->set_contrast(sensor, 0);
  sensor->set_saturation(sensor, 0);
  sensor->set_sharpness(sensor, 0);
  sensor->set_whitebal(sensor, 1);
  sensor->set_exposure_ctrl(sensor, 1);
  sensor->set_gain_ctrl(sensor, 1);
  Serial.printf("PSRAM: %s | default: QVGA | JPEG: %d | max stream FPS: %u\n",
                hasPSRAM ? "OK" : "NOT FOUND (QVGA only)", JPEG_QUALITY, (unsigned)STREAM_MAX_FPS);
  return true;
}
void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(FLASH_LED_PIN, OUTPUT);
  digitalWrite(FLASH_LED_PIN, LOW);
  cameraMutex = xSemaphoreCreateMutex();
  if (!cameraMutex) {
    Serial.println("Cannot allocate camera mutex.");
    while (true) { delay(1000); }
  }
  if (!setupCamera()) {
    while (true) { delay(1000); }
  }
  WiFi.mode(WIFI_AP);
  WiFi.setSleep(false);
  if (!WiFi.softAPConfig(local_IP, gateway, subnet)) {
    Serial.println("Cannot configure AP address.");
    while (true) { delay(1000); }
  }
  // 4 คือจำนวนเครื่องที่ต่อ WiFi ได้ ไม่ใช่จำนวนผู้ชมวิดีโอพร้อมกัน
  // HTTP stream แบบนี้ให้บริการสตรีมยาวทีละ 1 รายการ ควรเปิดเพียง 1 แท็บ
  bool apResult = WiFi.softAP(AP_SSID, AP_PASSWORD, AP_CHANNEL, 0, 4);
  if (!apResult) {
    Serial.println("Cannot start WiFi AP.");
    while (true) { delay(1000); }
  }
  startCameraServer();
  Serial.printf("WiFi: %s | channel: %u\n", AP_SSID, (unsigned)AP_CHANNEL);
  Serial.print("Open: http://");
  Serial.println(WiFi.softAPIP());
  Serial.println("Serial Monitor: 115200 baud. Stream statistics appear every 5 seconds.");
}
void loop() {
  delay(1000);
}
