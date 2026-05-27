/**
 * wifi_ota — WiFi AP + OTA upload + device settings for TankSync Transmitter
 */

#include "wifi_ota.h"
#include "battery_monitor.h"      /* power_mode_t + power_get_override / set */
#include "driver/gpio.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_buffer.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "wifi_ota";

#define AP_SSID         "TankSync-Update"
#define AP_MAX_CONN     2
#define TIMEOUT_US      (5LL * 60 * 1000000)  // 5 minutes

static int64_t s_last_activity_us = 0;
static int     s_led_gpio = 8;
static char    s_ap_ssid[32] = AP_SSID;

static void touch_activity(void) {
    s_last_activity_us = esp_timer_get_time();
}

// ── HTML page ────────────────────────────────────────────────────────────────
static const char PAGE_HTML[] =
"<!DOCTYPE html><html><head>"
"<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1,user-scalable=no'>"
"<title>TankSync TX setup</title>"
"<style>"
":root{--mist:#eef1f4;--paper:#fafbfc;--ink:#0f1620;--ink2:#3a4654;--ink3:#6b7886;"
"--line:#d8dde3;--line2:#c2c9d2;--leaf:#4a7a5c;--leaf-soft:#e3ede6;--rust:#a8423a;"
"--rust-soft:#f1dad6;--warm:#b87a3c;--warm-soft:#f4e9d8;"
"--serif:'Iowan Old Style','Charter','Georgia','Times New Roman',serif;"
"--sans:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;"
"--mono:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace}"
"*{box-sizing:border-box;margin:0;padding:0}"
"html,body{background:var(--mist);min-height:100vh}"
"body{font-family:var(--sans);font-size:15px;line-height:1.5;color:var(--ink);"
"padding:24px 16px 48px;-webkit-font-smoothing:antialiased;-webkit-tap-highlight-color:transparent}"
".wrap{max-width:560px;margin:0 auto}"
".banner{border-bottom:1px solid var(--ink);padding-bottom:14px;margin-bottom:24px;text-align:center}"
".banner h1{font-family:var(--serif);font-weight:600;font-size:34px;letter-spacing:-.025em;margin:0;line-height:1}"
".banner .sub{font-family:var(--serif);font-style:italic;font-size:13px;color:var(--ink3);margin-top:6px}"
".card{background:var(--paper);border:1px solid var(--line);padding:20px;margin-bottom:18px}"
".card h2{font-family:var(--serif);font-weight:500;font-size:18px;letter-spacing:-.01em;"
"margin:0 0 14px;padding-bottom:10px;border-bottom:1px dotted var(--line)}"
".row{display:flex;justify-content:space-between;align-items:baseline;padding:6px 0;"
"border-bottom:1px dotted var(--line);gap:10px}"
".row:last-child{border-bottom:0}"
".row .lbl{font-size:11px;letter-spacing:.16em;text-transform:uppercase;color:var(--ink3);font-weight:500}"
".row .v{font-family:var(--mono);font-size:13px;color:var(--ink);text-align:right;word-break:break-all}"
".pill{display:inline-block;padding:2px 10px;font-size:11px;font-weight:600;letter-spacing:.06em;"
"text-transform:uppercase;border-radius:999px}"
".pill.ok{background:var(--leaf-soft);color:var(--leaf)}"
".pill.bad{background:var(--rust-soft);color:var(--rust)}"
".pill.warn{background:var(--warm-soft);color:var(--warm)}"
"label{display:block;font-size:11px;letter-spacing:.16em;text-transform:uppercase;"
"color:var(--ink3);margin:14px 0 6px;font-weight:500}"
"label:first-of-type{margin-top:0}"
"input[type=number],select{width:100%;padding:9px 10px;background:#fff;border:1px solid var(--line2);"
"color:var(--ink);font-family:var(--sans);font-size:14px;border-radius:0;-webkit-appearance:none;appearance:none}"
"select{background:#fff url('data:image/svg+xml;utf8,<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 12 8\"><path d=\"M1 1l5 5 5-5\" fill=\"none\" stroke=\"%236b7886\" stroke-width=\"1.5\"/></svg>') no-repeat right 12px center/12px 8px;padding-right:36px}"
"input[type=number]:focus,select:focus{outline:none;border-color:var(--ink2)}"
"input[type=file]{font-family:var(--sans);font-size:13px;color:var(--ink2);margin:6px 0}"
".btn{display:block;width:100%;padding:11px 14px;border:1px solid var(--ink);background:var(--ink);"
"color:#fff;font-family:var(--sans);font-size:14px;font-weight:500;cursor:pointer;margin-top:14px;"
"letter-spacing:.02em;border-radius:0;transition:opacity .15s}"
".btn:hover{opacity:.88}"
".btn:active{opacity:.7}"
".btn:disabled{opacity:.35;cursor:not-allowed}"
".btn.danger{border-color:var(--rust);background:#fff;color:var(--rust)}"
".btn.danger:hover{background:var(--rust-soft);opacity:1}"
".note{font-size:12px;color:var(--ink3);margin-top:6px;line-height:1.45}"
".note.warn{color:var(--warm)}"
".bar-wrap{height:6px;background:var(--line);margin-top:10px;display:none}"
".bar-fill{height:100%;background:var(--leaf);width:0%;transition:width .3s}"
".bar-txt{text-align:center;font-size:12px;color:var(--ink3);margin-top:6px;font-variant-numeric:tabular-nums}"
".msg{padding:9px 12px;margin-top:10px;font-size:13px;display:none;border:1px solid var(--line)}"
".msg-ok{border-color:var(--leaf);background:var(--leaf-soft);color:var(--leaf);display:block}"
".msg-err{border-color:var(--rust);background:var(--rust-soft);color:var(--rust);display:block}"
"</style></head><body>"
"<div class='wrap'>"
"<div class='banner'>"
"<h1>TankSync</h1>"
"<div class='sub'>Transmitter setup</div>"
"</div>"

"<div class='card'>"
"<h2>System</h2>"
"<div class='row'><span class='lbl'>Firmware</span><span class='v' id='ver'>—</span></div>"
"<div class='row'><span class='lbl'>Device MAC</span><span class='v' id='mac'>—</span></div>"
"<div class='row'><span class='lbl'>Wi-Fi SSID</span><span class='v' id='apssid'>—</span></div>"
"<div class='row'><span class='lbl'>Pairing</span><span class='v' id='pair-pill'>—</span></div>"
"<div class='row'><span class='lbl'>Sleep</span><span class='v'><span id='slp'>—</span> s</span></div>"
"<div class='row'><span class='lbl'>Samples</span><span class='v' id='smp'>—</span></div>"
"<div class='row'><span class='lbl'>Power monitor</span><span class='v' id='pmd'>—</span></div>"
"<div class='row'><span class='lbl'>Distance sensor</span><span class='v' id='snm'>—</span></div>"
"</div>"

"<div class='card'>"
"<h2>Distance sensor</h2>"
"<label>Sensor type</label>"
"<select id='sensor_kind'>"
"<option value='sr04'>Ultrasonic — AJ-SR04M (0.05–4 m)</option>"
"<option value='ld2413'>mmWave — HLK-LD2413 (0.15–10.5 m) · Experimental</option>"
"</select>"
"<div class='note warn'>Changing the sensor reboots the transmitter so the new driver loads. The mmWave driver is built from the datasheet and has not been bench-verified — use for early access only.</div>"
"</div>"

"<div class='card'>"
"<h2>Settings</h2>"
"<label>Sleep interval (seconds)</label>"
"<input type='number' id='sleep_s' min='60' max='86400' value='300'>"
"<label>Sensor samples per wake</label>"
"<input type='number' id='samples' min='3' max='20' value='5'>"
"<label>Power monitor</label>"
"<select id='power_override'>"
"<option value='auto'>Auto-detect (recommended)</option>"
"<option value='ina219'>Force INA219 (I²C 0x40)</option>"
"<option value='voltage'>Force voltage divider (ADC)</option>"
"<option value='disabled'>Disabled (no power monitoring)</option>"
"</select>"
"<div class='note'>All changes are written on Save. If the sensor was changed, the transmitter reboots automatically.</div>"
"<button class='btn' onclick='saveAll()'>Save &amp; apply changes</button>"
"<div class='msg' id='cfg-status'></div>"
"</div>"

"<div class='card'>"
"<h2>Firmware update</h2>"
"<input type='file' id='fw-file' accept='.bin'>"
"<button class='btn' id='upload-btn' onclick='uploadFw()'>Upload &amp; flash</button>"
"<div class='bar-wrap' id='progress'><div class='bar-fill' id='pbar'></div></div>"
"<div class='bar-txt' id='ptxt'></div>"
"<div class='msg' id='ota-status'></div>"
"</div>"

"<div class='card'>"
"<h2>Diagnostics</h2>"
"<div class='row'><span class='lbl'>Diag mode</span><span class='v' id='diag-state'>—</span></div>"
"<div class='note'>When enabled, the TX skips deep sleep and keeps the Wi-Fi AP up so you can watch the live console below. Auto-disables after 30 minutes. <b>Battery drains ~30× faster while enabled — bench use only.</b></div>"
"<button class='btn' id='diag-btn' onclick='toggleDiag()'>Enable diagnostic mode</button>"
"<div class='msg' id='diag-status'></div>"
"<div style='margin-top:14px;display:flex;align-items:center;gap:8px'>"
"<label style='margin:0;flex:1'>Live console</label>"
"<button class='btn' style='width:auto;padding:6px 12px;margin:0;font-size:12px' onclick='togglePoll()'><span id='poll-lbl'>Pause</span></button>"
"<button class='btn' style='width:auto;padding:6px 12px;margin:0;font-size:12px' onclick='clearLogs()'>Clear</button>"
"</div>"
"<pre id='console' style='background:#fafbfc;border:1px solid var(--line2);padding:10px;max-height:340px;overflow:auto;font-family:var(--mono);font-size:11px;line-height:1.45;margin-top:8px;white-space:pre-wrap;word-break:break-all'></pre>"
"</div>"

"<div class='card'>"
"<h2>Factory reset</h2>"
"<div class='note'>Erases all stored settings on this transmitter: pairing, LoRa config, sleep interval, and sensor choice. The TX reboots into an unpaired state.</div>"
"<button class='btn danger' onclick='factoryReset()'>Factory reset</button>"
"<div class='msg' id='rst-status'></div>"
"</div>"

"</div>"  /* /.wrap */

"<script>"
"function $(id){return document.getElementById(id)}"
// Load device info on page load
"fetch('/api/info').then(r=>r.json()).then(d=>{"
"$('ver').textContent=d.version;"
// MAC: format raw 12-hex as colon-separated for readability
"var raw=(d.mac||'').toLowerCase();"
"var mac=raw.length===12?(raw.match(/.{2}/g)||[]).join(':'):(raw||'—');"
"$('mac').textContent=mac;"
"$('apssid').textContent=d.ap_ssid||'—';"
// Pairing status pill — green if address > 0, red otherwise
"var addr=parseInt(d.address||0,10);"
"var pill=$('pair-pill');"
"if(addr>0){pill.innerHTML='<span class=\"pill ok\">Paired · addr '+addr+'</span>';}"
"else{pill.innerHTML='<span class=\"pill bad\">Not paired — hold BOOT 2s on the device</span>';}"
"$('slp').textContent=d.sleep_s;"
"$('smp').textContent=d.samples;"
"$('sleep_s').value=d.sleep_s;"
"$('samples').value=d.samples;"
// Power monitor: shows the saved override (active value applies after reboot)
"var po=d.power_override||'auto';"
"var pmd=(po==='auto')?'Auto-detect':('Forced '+po);"
"$('pmd').textContent=pmd;"
"$('power_override').value=po;"
// Distance sensor: current kind (sr04 default if NVS absent). Stash the
// loaded value so saveAll() can detect a real change vs no-op save.
"var sk=d.sensor_kind||'sr04';"
"window._loadedSensorKind=sk;"
"var snm=(sk==='ld2413')?'mmWave (LD2413) · experimental':'Ultrasonic (SR04M)';"
"$('snm').textContent=snm;"
"$('sensor_kind').value=sk;"
// Diagnostics: reflect the current diag mode state in the button + pill
"reflectDiagState(!!d.diag_mode);"
"});"

// Save EVERYTHING in one round-trip. /api/config has partial-update semantics
// so we just include all fields; backend reboots only if sensor_kind changed.
"function saveAll(){"
"var sk=$('sensor_kind').value;"
"var sleep=parseInt($('sleep_s').value);"
"var samp=parseInt($('samples').value);"
"var po=$('power_override').value;"
"var st=$('cfg-status');"
// Track the currently-loaded sensor so we know whether this save will reboot.
// d.sensor_kind was stashed on load; compare to detect a real change.
"var prev=window._loadedSensorKind||'';"
"var willReboot=(sk && sk!==prev);"
"var label=(sk==='ld2413')?'mmWave (LD2413)':'Ultrasonic (SR04M)';"
"if(willReboot && sk==='ld2413' && !confirm('Switch sensor to '+label+'?\\n\\nThe LD2413 driver has not been bench-verified. Proceed?'))return;"
"var body={sleep_s:sleep,samples:samp,power_override:po,sensor_kind:sk};"
"fetch('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify(body)}).then(r=>r.json()).then(d=>{"
"st.className='msg '+(d.ok?'msg-ok':'msg-err');"
"if(!d.ok){st.textContent='Error: '+(d.error||'unknown');return;}"
"st.textContent=willReboot?('Saved. Rebooting to load '+label+'…'):'Saved. Sleep, samples and power monitor apply on next boot.';"
"$('slp').textContent=sleep;"
"$('smp').textContent=samp;"
"if(willReboot)setTimeout(()=>{location.reload();},2500);"
"}).catch(e=>{st.className='msg msg-err';st.textContent='Failed: '+e;});"
"}"

// Upload firmware
"function uploadFw(){"
"var f=$('fw-file').files[0];"
"if(!f){alert('Select a .bin file first');return;}"
"var btn=$('upload-btn'),prog=$('progress'),bar=$('pbar'),txt=$('ptxt'),st=$('ota-status');"
"btn.disabled=true;prog.style.display='block';st.style.display='none';"
"var xhr=new XMLHttpRequest();"
"xhr.upload.onprogress=function(e){"
"if(e.lengthComputable){var p=Math.round(e.loaded*100/e.total);"
"bar.style.width=p+'%';txt.textContent=p+'% ('+Math.round(e.loaded/1024)+' KB)';}"
"};"
"xhr.onload=function(){"
"if(xhr.status===200){st.className='msg msg-ok';st.textContent='Update OK — rebooting…';}"
"else{st.className='msg msg-err';st.textContent='Failed: '+xhr.responseText;btn.disabled=false;}"
"};"
"xhr.onerror=function(){st.className='msg msg-err';"
"st.textContent='Upload failed (connection lost)';btn.disabled=false;};"
"xhr.open('POST','/api/ota');xhr.send(f);"
"}"

"function factoryReset(){"
"if(!confirm('Erase all settings on this transmitter? It will unpair and reboot.'))return;"
"var st=$('rst-status');"
"fetch('/api/reset',{method:'POST'}).then(r=>r.json()).then(d=>{"
"st.className='msg msg-ok';st.textContent='Reset complete — rebooting…';"
"}).catch(e=>{st.className='msg msg-err';st.textContent='Reset failed';});"
"}"

// ── Diagnostic mode toggle + live console ─────────────────────────────────
"window._diag={enabled:false,cursor:0,paused:false,timer:null};"
"function reflectDiagState(en){"
"window._diag.enabled=!!en;"
"$('diag-state').textContent=en?'ENABLED':'disabled';"
"$('diag-state').style.color=en?'var(--leaf)':'var(--ink3)';"
"$('diag-btn').textContent=en?'Disable diagnostic mode':'Enable diagnostic mode';"
"$('diag-btn').classList.toggle('danger',en);"
"}"
"function toggleDiag(){"
"var en=!window._diag.enabled;"
"var msg=en?'Enable diagnostic mode?\\n\\nThe TX will stop deep-sleeping and keep its Wi-Fi AP up so you can watch logs. Battery drains ~30× faster. Auto-off after 30 minutes.':"
"'Disable diagnostic mode? TX will resume normal deep-sleep cycle.';"
"if(!confirm(msg))return;"
"var st=$('diag-status');"
"fetch('/api/diag',{method:'POST',headers:{'Content-Type':'application/json'},"
"body:JSON.stringify({enabled:en})}).then(r=>{"
// Same defensive handling as the log poller — surface 404/etc clearly
// instead of throwing SyntaxError on JSON.parse of an error page.
"if(!r.ok)return{ok:false,error:'HTTP '+r.status};"
"return r.json().catch(()=>({ok:false,error:'bad JSON response'}));"
"}).then(d=>{"
"st.className='msg '+(d.ok?'msg-ok':'msg-err');"
"st.textContent=d.ok?('Diag mode '+(en?'enabled':'disabled')+' — rebooting…'):'Failed: '+(d.error||'unknown');"
"if(d.ok)setTimeout(()=>{location.reload();},2500);"
"}).catch(e=>{st.className='msg msg-err';st.textContent='Failed: '+e;});"
"}"
"function pollLogs(){"
"if(window._diag.paused)return;"
"fetch('/api/logs?since='+window._diag.cursor).then(r=>{"
// Defensive: if /api/logs is missing (older firmware) or any non-2xx,
// silently no-op rather than appending the server's error body into the
// console (which would just spam 'Nothing matches the given URI' forever).
"if(!r.ok)return null;"
"var c=r.headers.get('X-Log-Cursor');if(c)window._diag.cursor=parseInt(c)||0;"
"return r.text();"
"}).then(txt=>{"
"if(!txt)return;"
"var el=$('console');var atBottom=(el.scrollHeight-el.scrollTop-el.clientHeight)<30;"
"el.textContent+=txt;"
// Trim displayed buffer to last 20 KB so the DOM doesn't bloat over hours
"if(el.textContent.length>20000)el.textContent=el.textContent.slice(-15000);"
"if(atBottom)el.scrollTop=el.scrollHeight;"
"}).catch(e=>{});"
"}"
"function togglePoll(){window._diag.paused=!window._diag.paused;$('poll-lbl').textContent=window._diag.paused?'Resume':'Pause';}"
"function clearLogs(){fetch('/api/log_clear',{method:'POST'}).then(()=>{$('console').textContent='';window._diag.cursor=0;});}"
// Start the console poller — runs even when diag mode is off so the user can
// see boot logs from the most recent reboot. Cheap on the TX (one short
// fetch every 1.5s, only when this page is open).
"window._diag.timer=setInterval(pollLogs,1500);"
"pollLogs();"
"</script></body></html>";

// ── API: GET /api/info ───────────────────────────────────────────────────────
static esp_err_t handle_info(httpd_req_t *req) {
    touch_activity();
    uint32_t sleep_s = 300;
    uint8_t  samples = 5;
    uint16_t address = 0;
    char     version[32]      = "unknown";
    char     sensor_kind[16]  = "sr04";   // default if NVS absent

    uint8_t diag_mode = 0;

    // Read NVS settings
    nvs_handle_t h;
    if (nvs_open("system", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "sleep_s", &sleep_s);
        nvs_get_u8(h, "samples", &samples);
        size_t klen = sizeof(sensor_kind);
        nvs_get_str(h, "sensor_kind", sensor_kind, &klen);
        nvs_get_u8(h, "diag_mode", &diag_mode);
        nvs_close(h);
    }
    if (nvs_open("lora", NVS_READONLY, &h) == ESP_OK) {
        // Key is "addr" — the LoRa driver writes it under that name in
        // lora_tx.c (NVS_NS="lora", key="addr"). The previous "my_addr"
        // typo always read 0, which the old UI displayed as a harmless
        // "ADDRESS 0" line. The new pill-based UI interprets 0 as
        // "not paired" — making the latent bug loudly visible.
        nvs_get_u16(h, "addr", &address);
        nvs_close(h);
    }

    // Get version from app desc
    const esp_app_desc_t *app = esp_app_get_description();
    if (app) strncpy(version, app->version, sizeof(version) - 1);

    // Read persisted power-monitor override (default "auto")
    char power_override[16] = "auto";
    power_get_override(power_override, sizeof(power_override));

    // WiFi STA MAC — the canonical device identifier per
    // project_pair_identity_redesign_2026_05_20. Shown in the SYSTEM card so
    // users can confirm they're configuring the right TX (matches the last
    // 4 hex chars of the AP SSID).
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[13];
    snprintf(mac_str, sizeof(mac_str), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    char json[480];
    snprintf(json, sizeof(json),
        "{\"version\":\"%s\",\"address\":%d,\"sleep_s\":%lu,\"samples\":%d,"
        "\"power_override\":\"%s\",\"sensor_kind\":\"%s\",\"mac\":\"%s\","
        "\"ap_ssid\":\"%s\",\"diag_mode\":%s}",
        version, (int)address, (unsigned long)sleep_s, (int)samples,
        power_override, sensor_kind, mac_str, s_ap_ssid,
        diag_mode ? "true" : "false");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, -1);
}

// Parse a JSON string field "name":"value" out of buf into out[out_sz].
// Returns true if the key was present and a value extracted.
static bool json_extract_string(const char *buf, const char *key,
                                char *out, size_t out_sz) {
    const char *p = strstr(buf, key);
    if (!p) return false;
    const char *q = strchr(p, ':');
    if (!q) return false;
    const char *start = strchr(q, '"');
    if (!start) return false;
    start++;
    const char *end = strchr(start, '"');
    if (!end) return false;
    size_t n = (size_t)(end - start);
    if (n >= out_sz) n = out_sz - 1;
    memcpy(out, start, n);
    out[n] = '\0';
    return true;
}

// ── API: POST /api/config ────────────────────────────────────────────────────
// Partial-update semantics: each field is written to NVS only if present in
// the JSON body. Missing fields keep their stored value.
static esp_err_t handle_config(httpd_req_t *req) {
    touch_activity();
    char buf[160];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    // Detect which fields are present in the payload.
    bool has_sleep   = (strstr(buf, "\"sleep_s\"")        != NULL);
    bool has_samples = (strstr(buf, "\"samples\"")        != NULL);
    bool has_power   = (strstr(buf, "\"power_override\"") != NULL);
    bool has_sensor  = (strstr(buf, "\"sensor_kind\"")    != NULL);

    uint32_t sleep_s = 300;
    uint8_t  samples = 5;
    char *p;
    if (has_sleep) {
        p = strchr(strstr(buf, "\"sleep_s\""), ':');
        if (p) sleep_s = (uint32_t)atoi(p + 1);
    }
    if (has_samples) {
        p = strchr(strstr(buf, "\"samples\""), ':');
        if (p) samples = (uint8_t)atoi(p + 1);
    }

    char power_override[16] = {0};
    if (has_power) {
        json_extract_string(buf, "\"power_override\"", power_override, sizeof(power_override));
    }

    char sensor_kind[16] = {0};
    if (has_sensor) {
        json_extract_string(buf, "\"sensor_kind\"", sensor_kind, sizeof(sensor_kind));
        // Validate — accept only known kinds. Unknown values are rejected so a
        // typo doesn't brick the next boot (driver lookup would fall back to
        // default, but the user thinks they configured something else).
        if (strcmp(sensor_kind, "sr04") != 0 && strcmp(sensor_kind, "ld2413") != 0) {
            ESP_LOGW(TAG, "Rejecting unknown sensor_kind='%s'", sensor_kind);
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req,
                "{\"ok\":false,\"error\":\"unknown sensor_kind (expected sr04 or ld2413)\"}", -1);
        }
    }

    // Clamp numeric values (only matters if they were provided)
    if (has_sleep) {
        if (sleep_s < 60)    sleep_s = 60;
        if (sleep_s > 86400) sleep_s = 86400;
    }
    if (has_samples) {
        if (samples < 3)  samples = 3;
        if (samples > 20) samples = 20;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open("system", NVS_READWRITE, &h);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS open failed");
        return ESP_FAIL;
    }
    if (has_sleep)   nvs_set_u32(h, "sleep_s",     sleep_s);
    if (has_samples) nvs_set_u8 (h, "samples",     samples);
    if (has_sensor)  nvs_set_str(h, "sensor_kind", sensor_kind);
    nvs_commit(h);
    nvs_close(h);

    // Apply power-mode override (separate NVS namespace, validated by power_set_override)
    if (has_power && power_override[0] != '\0') {
        esp_err_t perr = power_set_override(power_override);
        if (perr != ESP_OK) {
            ESP_LOGW(TAG, "Ignoring invalid power_override='%s'", power_override);
        }
    }

    ESP_LOGI(TAG, "Config saved: sleep=%s%lus samples=%s%d power=%s sensor=%s",
             has_sleep   ? "" : "(unchanged) ", (unsigned long)sleep_s,
             has_samples ? "" : "(unchanged) ", (int)samples,
             has_power   ? power_override : "(unchanged)",
             has_sensor  ? sensor_kind    : "(unchanged)");

    // sensor_kind takes effect at next boot — the iface vtable is resolved
    // once at app_main start. Reboot now so the new driver actually loads.
    // Response is sent first; the reboot happens after a short delay so the
    // browser sees the ACK before the TCP connection drops.
    if (has_sensor) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"ok\":true,\"reboot\":true}", -1);
        ESP_LOGW(TAG, "Sensor kind changed to %s — rebooting in 500ms", sensor_kind);
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
        return ESP_OK;  // unreachable
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", -1);
}

// ── API: POST /api/ota ───────────────────────────────────────────────────────
static esp_err_t handle_ota(httpd_req_t *req) {
    touch_activity();
    ESP_LOGI(TAG, "OTA upload starting (%d bytes)", req->content_len);

    const esp_partition_t *update = esp_ota_get_next_update_partition(NULL);
    if (!update) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition");
        return ESP_FAIL;
    }

    esp_ota_handle_t ota_h;
    esp_err_t err = esp_ota_begin(update, OTA_WITH_SEQUENTIAL_WRITES, &ota_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    char buf[1024];
    int total = 0;
    int remaining = req->content_len;

    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, buf, (remaining < sizeof(buf)) ? remaining : sizeof(buf));
        if (recv_len <= 0) {
            ESP_LOGE(TAG, "OTA recv failed at %d bytes", total);
            esp_ota_abort(ota_h);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload interrupted");
            return ESP_FAIL;
        }

        err = esp_ota_write(ota_h, buf, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_h);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_FAIL;
        }

        total += recv_len;
        remaining -= recv_len;
        touch_activity();
    }

    err = esp_ota_end(ota_h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
        return ESP_FAIL;
    }

    err = esp_ota_set_boot_partition(update);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Boot partition set failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA SUCCESS: %d bytes written. Rebooting in 2s...", total);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "OK", 2);

    // Delay to let response reach the client, then reboot
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK; // never reached
}

// ── API: POST /api/reset ─────────────────────────────────────────────────────
static esp_err_t handle_reset(httpd_req_t *req) {
    touch_activity();
    ESP_LOGW(TAG, "FACTORY RESET requested via web UI");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"ok\":true}", -1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    nvs_flash_erase();
    esp_restart();
    return ESP_OK;
}

// ── API: GET /api/logs?since=N ───────────────────────────────────────────────
// Returns plain-text log lines since the byte cursor N. Client passes the
// total byte count it has already seen; server returns just what's new.
// Headers:
//   X-Log-Cursor: <new total byte count>  (echo back on next request)
static esp_err_t handle_logs(httpd_req_t *req) {
    touch_activity();

    // Parse ?since=N from query string. Default 0 = give me everything.
    size_t cursor = 0;
    char qbuf[32];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char val[20] = {0};
        if (httpd_query_key_value(qbuf, "since", val, sizeof(val)) == ESP_OK) {
            cursor = (size_t)strtoul(val, NULL, 10);
        }
    }

    static char out[2400];   // ~half of the 4KB ring; one HTTP response chunk
    size_t n = log_buffer_read(out, sizeof(out), &cursor);

    char hdr[24];
    snprintf(hdr, sizeof(hdr), "%u", (unsigned)cursor);
    httpd_resp_set_hdr(req, "X-Log-Cursor", hdr);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, out, n);
}

static esp_err_t handle_log_clear(httpd_req_t *req) {
    touch_activity();
    log_buffer_clear();
    ESP_LOGI(TAG, "Log buffer cleared via web UI");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", -1);
}

// ── API: POST /api/diag ──────────────────────────────────────────────────────
// Toggle diagnostic mode. Body: {"enabled":true|false}
// When enabled, TX writes diag_mode=1 to NVS and reboots. On next boot,
// app_main reads the flag and skips deep sleep, runs faster cycles, keeps
// WiFi AP up indefinitely. Auto-disables itself after 30 minutes.
static esp_err_t handle_diag(httpd_req_t *req) {
    touch_activity();
    char buf[64];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    bool enabled = (strstr(buf, "\"enabled\":true") != NULL ||
                    strstr(buf, "\"enabled\": true") != NULL);

    nvs_handle_t h;
    if (nvs_open("system", NVS_READWRITE, &h) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS open failed");
        return ESP_FAIL;
    }
    nvs_set_u8(h, "diag_mode", enabled ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);

    ESP_LOGW(TAG, "Diagnostic mode %s — rebooting", enabled ? "ENABLED" : "DISABLED");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, enabled
        ? "{\"ok\":true,\"diag\":true,\"reboot\":true}"
        : "{\"ok\":true,\"diag\":false,\"reboot\":true}", -1);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// ── API: GET / (serve HTML page) ─────────────────────────────────────────────
static esp_err_t handle_root(httpd_req_t *req) {
    touch_activity();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PAGE_HTML, -1);
}

// ── WiFi AP setup ────────────────────────────────────────────────────────────
static void wifi_ap_init(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .ap = {
            .channel = 6,
            .max_connection = AP_MAX_CONN,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    strncpy((char *)wifi_config.ap.ssid, s_ap_ssid, sizeof(wifi_config.ap.ssid));
    wifi_config.ap.ssid_len = strlen(s_ap_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Max TX power for better visibility on small C3 antenna
    esp_wifi_set_max_tx_power(80);  // 80 = 20dBm (max)

    ESP_LOGI(TAG, "WiFi AP started: SSID='%s' ch=6 IP=192.168.4.1", s_ap_ssid);
}

// ── HTTP server setup ────────────────────────────────────────────────────────
static httpd_handle_t start_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    // 8 routes today: /, /api/info, /api/config, /api/ota, /api/reset,
    // /api/logs, /api/log_clear, /api/diag. Bump headroom to 12 so adding a
    // route doesn't silently 404 the last one again.
    config.max_uri_handlers = 12;
    config.recv_wait_timeout = 30;  // 30s for large uploads

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }

    static const httpd_uri_t routes[] = {
        { .uri = "/",              .method = HTTP_GET,  .handler = handle_root },
        { .uri = "/api/info",      .method = HTTP_GET,  .handler = handle_info },
        { .uri = "/api/config",    .method = HTTP_POST, .handler = handle_config },
        { .uri = "/api/ota",       .method = HTTP_POST, .handler = handle_ota },
        { .uri = "/api/reset",     .method = HTTP_POST, .handler = handle_reset },
        { .uri = "/api/logs",      .method = HTTP_GET,  .handler = handle_logs },
        { .uri = "/api/log_clear", .method = HTTP_POST, .handler = handle_log_clear },
        { .uri = "/api/diag",      .method = HTTP_POST, .handler = handle_diag },
    };

    for (int i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }

    ESP_LOGI(TAG, "HTTP server started on port 80");
    return server;
}

// ── Public entry point ───────────────────────────────────────────────────────
void wifi_ota_start(int led_gpio, uint16_t device_addr) {
    s_led_gpio = led_gpio;

    // SSID is derived from the WiFi STA MAC so every TX broadcasts a unique
    // AP name even before LoRa pairing assigns an address. The last 4 hex
    // chars of the MAC mirror the convention used for the RX mDNS hostname
    // (tanksync-XXXX) so the same physical identifier ties the two surfaces
    // together — see project_device_identity_strategy.
    // (uint16_t)device_addr is kept in the signature for API stability but
    // no longer feeds the SSID; if needed for logging in the future, append
    // to the AP name with a separator other than the MAC suffix.
    uint8_t mac[6] = {0};
    esp_err_t mac_err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (mac_err == ESP_OK) {
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "TankSync-TX-%02X%02X", mac[4], mac[5]);
    } else {
        // Fall back to a generic-but-warning SSID so the user notices.
        snprintf(s_ap_ssid, sizeof(s_ap_ssid), "TankSync-TX-NOMAC");
        ESP_LOGW(TAG, "esp_read_mac failed (%s) — using fallback SSID",
                 esp_err_to_name(mac_err));
    }

    ESP_LOGI(TAG, "=== ENTERING WIFI OTA MODE ===");
    ESP_LOGI(TAG, "Connect to WiFi '%s' → open http://192.168.4.1 (addr=%u)",
             s_ap_ssid, (unsigned)device_addr);

    wifi_ap_init();
    httpd_handle_t server = start_server();

    if (!server) {
        ESP_LOGE(TAG, "Server failed — rebooting");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    touch_activity();

    // Wait for activity or timeout
    while (1) {
        int64_t idle = esp_timer_get_time() - s_last_activity_us;
        if (idle > TIMEOUT_US) {
            ESP_LOGW(TAG, "5-minute timeout — rebooting to normal mode");
            break;
        }

        // Blink LED to show we're in update mode (fast double-blink)
        gpio_set_level(s_led_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(s_led_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(s_led_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(s_led_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(1700));
    }

    httpd_stop(server);
    esp_wifi_stop();
    esp_restart();
}
