#include "web.h"

#include <WiFi.h>
#include <WebServer.h>
#include <esp_mac.h>
#include <esp_heap_caps.h>

static WebServer server(80);
static WebConfig web_config;
static bool server_started = false;
static uint8_t *latest_jpeg = nullptr;
static size_t latest_jpeg_len = 0;
static size_t latest_jpeg_cap = 0;
static WebFrameInfo latest_info;

static const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>VST-BASE</title>
<style>
html,body{margin:0;width:100%;height:100%;background:#070707;color:#f6f6f6;font-family:Arial,Helvetica,sans-serif;overflow:hidden}
.stage{position:fixed;inset:0;display:flex;align-items:center;justify-content:center;background:#070707}
#frame{display:block;max-width:100vw;max-height:100dvh;width:auto;height:auto;object-fit:contain;background:#000;image-rendering:auto}
#overlay{position:fixed;left:0;top:0;pointer-events:none}
</style>
</head>
<body>
<div class="stage">
<img id="frame" alt="">
<canvas id="overlay"></canvas>
</div>
<script>
const img=document.getElementById('frame');
const canvas=document.getElementById('overlay');
const ctx=canvas.getContext('2d');
const names={0:'Apis mellifera',1:'Vespa crabro',2:'Vespula sp.',3:'Vespa velutina'};
let lastId=0,lastUrl='',lastInfo=null,inflight=false;
function clamp(v,min,max){return Math.max(min,Math.min(max,v));}
function fitCanvas(){
  const r=img.getBoundingClientRect();
  canvas.style.left=r.left+'px';canvas.style.top=r.top+'px';
  canvas.style.width=r.width+'px';canvas.style.height=r.height+'px';
  canvas.width=Math.max(1,Math.round(r.width));
  canvas.height=Math.max(1,Math.round(r.height));
}
function draw(){
  fitCanvas();
  ctx.clearRect(0,0,canvas.width,canvas.height);
  if(!lastInfo||!img.naturalWidth||!img.naturalHeight){
    ctx.font='16px Arial';ctx.textAlign='center';ctx.textBaseline='middle';
    ctx.fillStyle='rgba(255,255,255,.72)';
    ctx.fillText('Waiting for frame',canvas.width/2,canvas.height/2);
    return;
  }
  const b=lastInfo.box||{};
  if(!b.w||!b.h)return;
  const sx=canvas.width/img.naturalWidth,sy=canvas.height/img.naturalHeight;
  let x=b.x*sx,y=b.y*sy,w=b.w*sx,h=b.h*sy;
  x=clamp(x,0,canvas.width-1);y=clamp(y,0,canvas.height-1);
  w=clamp(w,1,canvas.width-x);h=clamp(h,1,canvas.height-y);
  ctx.strokeStyle=lastInfo.inference.detection_match?'#18d071':'#19b7ff';
  ctx.lineWidth=3;ctx.strokeRect(Math.round(x)+.5,Math.round(y)+.5,Math.round(w),Math.round(h));
  const label=(names[lastInfo.inference.class_idx]||('Class '+lastInfo.inference.class_idx))+' '+Math.round(lastInfo.inference.confidence*100)+'%';
  ctx.font='bold 16px Arial';ctx.textBaseline='top';
  const tw=Math.min(ctx.measureText(label).width+10,canvas.width);
  const lx=clamp(x,0,canvas.width-tw),ly=y>27?y-27:y+4;
  ctx.fillStyle='rgba(0,0,0,.82)';ctx.fillRect(lx,ly,tw,24);
  ctx.fillStyle='#fff';ctx.fillText(label,lx+5,ly+4);
}
async function tick(){
  if(inflight)return;
  inflight=true;
  try{
    const r=await fetch('/state.json?ts='+Date.now(),{cache:'no-store'});
    if(!r.ok){draw();return;}
    const info=await r.json();
    if(info.frame_id&&info.frame_id!==lastId){
      lastId=info.frame_id;lastInfo=info;
      const fr=await fetch('/frame.jpg?id='+lastId,{cache:'no-store'});
      if(fr.ok){
        const blob=await fr.blob();
        const url=URL.createObjectURL(blob);
        img.onload=()=>{draw();if(lastUrl)URL.revokeObjectURL(lastUrl);lastUrl=url;};
        img.src=url;
      }
    }else{
      lastInfo=info;draw();
    }
  }catch(e){draw();}
  finally{inflight=false;}
}
window.addEventListener('resize',draw);
setInterval(tick,120);
tick();
</script>
</body>
</html>
)HTML";

static void handle_root()
{
    server.send_P(200, "text/html", INDEX_HTML);
}

static void handle_frame()
{
    if (!latest_jpeg || latest_jpeg_len == 0) {
        server.send(503, "text/plain", "No frame");
        return;
    }

    server.sendHeader("Cache-Control", "no-store, max-age=0");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.setContentLength(latest_jpeg_len);
    server.send(200, "image/jpeg", "");

    WiFiClient client = server.client();
    size_t sent = 0;
    while (sent < latest_jpeg_len && client.connected()) {
        size_t chunk = latest_jpeg_len - sent;
        if (chunk > 1024)
            chunk = 1024;

        size_t written = client.write(latest_jpeg + sent, chunk);
        if (written == 0) {
            delay(1);
            continue;
        }

        sent += written;
    }
}

static void append_hex(String &s, uint32_t value)
{
    char buf[11];
    snprintf(buf, sizeof(buf), "\"%08lX\"", (unsigned long)value);
    s += buf;
}

static void handle_state()
{
    if (latest_info.frame_id == 0) {
        server.send(503, "text/plain", "No inference");
        return;
    }

    float confidence = (float)latest_info.confidence_u8 / 255.0f;
    String s;
    s.reserve(640);
    s += "{\"frame_id\":";
    s += (unsigned long)latest_info.frame_id;
    s += ",\"uptime_ms\":";
    s += (unsigned long)millis();
    s += ",\"inference\":{\"state\":";
    s += (unsigned)latest_info.state;
    s += ",\"class_idx\":";
    s += (unsigned)latest_info.class_idx;
    s += ",\"confidence_u8\":";
    s += (unsigned)latest_info.confidence_u8;
    s += ",\"confidence\":";
    s += String(confidence, 3);
    s += ",\"filter_match\":";
    s += latest_info.filter_match ? "true" : "false";
    s += ",\"detection_match\":";
    s += latest_info.detection_match ? "true" : "false";
    s += ",\"occurrence_count\":";
    s += (unsigned)latest_info.occurrence_count;
    s += ",\"occurrence_required\":";
    s += (unsigned)latest_info.occurrence_required;
    s += "},\"box\":{\"format\":\"xywh\",\"x\":";
    s += (unsigned)latest_info.bbox_x;
    s += ",\"y\":";
    s += (unsigned)latest_info.bbox_y;
    s += ",\"w\":";
    s += (unsigned)latest_info.bbox_w;
    s += ",\"h\":";
    s += (unsigned)latest_info.bbox_h;
    s += "},\"jpeg\":{\"len\":";
    s += (unsigned long)latest_info.jpeg_len;
    s += ",\"crc_rx\":";
    append_hex(s, latest_info.crc_rx);
    s += ",\"crc_calc\":";
    append_hex(s, latest_info.crc_calc);
    s += ",\"crc_ok\":";
    s += latest_info.crc_ok ? "true" : "false";
    s += ",\"valid\":";
    s += latest_info.valid ? "true" : "false";
    s += ",\"saved\":";
    s += latest_info.saved ? "true" : "false";
    s += "},\"actuation\":{\"activated\":";
    s += latest_info.actuated ? "true" : "false";
    s += "}}";

    server.sendHeader("Cache-Control", "no-store, max-age=0");
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.send(200, "application/json", s);
}

static bool start_station()
{
    if (web_config.ssid[0] == '\0') {
        Serial.println("WEB: station SSID empty");
        return false;
    }

    WiFi.mode(WIFI_STA);
    WiFi.begin(web_config.ssid, web_config.password);

    uint32_t start_ms = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start_ms < 15000)
        delay(250);

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("WEB: station connect timeout");
        return false;
    }

    Serial.print("WEB: station connected ip=");
    Serial.println(WiFi.localIP());
    return true;
}

static bool start_ap()
{
    WiFi.mode(WIFI_AP);

    String ssid = web_config.ssid[0] ? web_config.ssid : "VST-BASE";
    if (web_config.append_mac) {
        uint8_t mac[6] = {0};
        if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) == ESP_OK) {
            char suffix[10];
            snprintf(suffix, sizeof(suffix), "-%02X%02X%02X", mac[3], mac[4], mac[5]);
            ssid += suffix;
        }
    }

    bool ok = false;
    if (strlen(web_config.password) >= 8)
        ok = WiFi.softAP(ssid.c_str(), web_config.password);
    else
        ok = WiFi.softAP(ssid.c_str());

    if (!ok) {
        Serial.println("WEB: AP start failed");
        return false;
    }

    Serial.print("WEB: AP started ssid=");
    Serial.print(ssid);
    Serial.print(" ip=");
    Serial.println(WiFi.softAPIP());
    return true;
}

bool web_init(const WebConfig &config)
{
    web_config = config;
    server_started = false;

    if (web_config.mode == 0) {
        WiFi.mode(WIFI_OFF);
        Serial.println("WEB: disabled");
        return false;
    }

    bool wifi_ok = web_config.mode == 1 ? start_station() : start_ap();
    if (!wifi_ok)
        return false;

    server.on("/", handle_root);
    server.on("/frame.jpg", handle_frame);
    server.on("/state.json", handle_state);
    server.onNotFound([]() {
        server.send(404, "text/plain", "Not found");
    });
    server.begin();
    server_started = true;
    Serial.println("WEB: HTTP server started port=80 endpoints=/ /frame.jpg /state.json");
    return true;
}

void web_loop()
{
    if (server_started)
        server.handleClient();
}

void web_publish_frame(const uint8_t *jpeg, size_t jpeg_len, const WebFrameInfo &info)
{
    if (!jpeg || jpeg_len == 0)
        return;

    if (latest_jpeg_cap < jpeg_len) {
        uint8_t *next = (uint8_t *)heap_caps_malloc(jpeg_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!next)
            next = (uint8_t *)heap_caps_malloc(jpeg_len, MALLOC_CAP_8BIT);
        if (!next) {
            Serial.printf("WEB: latest frame alloc failed len=%u\n", (unsigned)jpeg_len);
            return;
        }
        if (latest_jpeg)
            free(latest_jpeg);
        latest_jpeg = next;
        latest_jpeg_cap = jpeg_len;
    }

    memcpy(latest_jpeg, jpeg, jpeg_len);
    latest_jpeg_len = jpeg_len;
    latest_info = info;
    latest_info.jpeg_len = (uint32_t)jpeg_len;
}
