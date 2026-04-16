/**
 * web_server implementation - TankSync Terminal UI v3.5
 */

#include "web_server.h"
#include "transmitter_registry.h"
#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "ota_manager.h"
#include "lora_rylr998.h"
#include "led_ws2812.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_spiffs.h"
#include "esp_random.h"
#include <inttypes.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "web";

// ── Staged firmware version extraction ───────────────────────────────────────
// ESP-IDF binaries embed esp_app_desc_t with a magic word (0xABCD5432).
// The struct lives in .rodata_desc which is typically in the 2nd or 3rd
// segment — can be hundreds of bytes into the binary.  Scan up to 4KB.
static bool extract_staged_version(char *ver_out, size_t ver_len) {
    FILE *f = fopen("/spiffs/tx_fw.bin", "rb");
    if (!f) return false;
    uint8_t buf[256];
    size_t file_off = 0;
    // Read in 256-byte chunks, scan up to 4KB
    while (file_off < 4096) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n < 4) break;
        for (size_t i = 0; i + 48 <= n; i += 4) {
            uint32_t w = (uint32_t)buf[i] | ((uint32_t)buf[i+1] << 8)
                       | ((uint32_t)buf[i+2] << 16) | ((uint32_t)buf[i+3] << 24);
            if (w == 0xABCD5432u) {
                // Version string is 32 bytes at magic + 16
                size_t copy = (ver_len - 1 < 31) ? ver_len - 1 : 31;
                memcpy(ver_out, &buf[i + 16], copy);
                ver_out[copy] = '\0';
                fclose(f);
                return ver_out[0] != '\0';
            }
        }
        // Overlap by 48 bytes so we don't miss a magic spanning chunk boundary
        if (n == sizeof(buf)) {
            fseek(f, -(long)48, SEEK_CUR);
            file_off += sizeof(buf) - 48;
        } else {
            break;
        }
    }
    fclose(f);
    return false;
}
static httpd_handle_t s_server = NULL;

static const char DASHBOARD_HTML[] =
"<!DOCTYPE html><html lang='en'><head>"
"<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1,user-scalable=no'>"
"<title>TankSync [TERMINAL]</title>"
"<style>"
"*{box-sizing:border-box;margin:0;padding:0;border-radius:0 !important}"
"body{font-family:'Courier New',Courier,monospace;background:#000;color:#ffb000;min-height:100vh;text-transform:uppercase;font-size:14px;line-height:1.4;text-shadow:0 0 2px #ffb000;overflow-x:hidden}"
"header{background:#000;padding:1rem;display:flex;flex-direction:column;align-items:center;gap:0.5rem;border-bottom:2px solid #ffb000;box-shadow:0 2px 10px rgba(255,176,0,0.2)}"
"@media(min-width:600px){header{flex-direction:row;justify-content:space-between;padding:1rem 1.5rem}}"
"header h1{font-size:1.4rem;font-weight:bold;color:#ffb000;letter-spacing:2px;text-align:center}"
".badge{padding:.3rem .6rem;border:1px solid #ffb000;font-size:.7rem;font-weight:bold;white-space:nowrap}"
".online{background:#ffb000;color:#000}.offline{background:#000;color:#ffb000;border-style:dashed}"
".stale{background:#000;color:#ffb000;border-style:double}.waiting{background:#000;color:#ffb000;opacity:0.6}"
"main{padding:1rem;max-width:1000px;margin:0 auto}"
".tanks{display:grid;grid-template-columns:1fr;gap:1rem;margin-bottom:2rem}"
"@media(min-width:700px){.tanks{grid-template-columns:repeat(auto-fill,minmax(320px,1fr));gap:1.5rem}}"
".card{background:#000;border:1px solid #ffb000;padding:1.5rem;position:relative;margin-bottom:1.5rem}"
".card-title{font-size:.9rem;color:#ffb000;margin-bottom:1.5rem;border-bottom:1px solid #ffb000;display:inline-block;padding-bottom:4px;width:100%;font-weight:bold}"
".tank-name{font-size:1.3rem;font-weight:bold;margin-bottom:1.2rem;color:#ffb000}"
".level-bar{background:#000;border:1px solid #ffb000;height:32px;margin:1.2rem 0;position:relative;overflow:hidden}"
".level-fill{height:100%;background:#ffb000;transition:width 0.5s;box-shadow:0 0 10px #ffb000}"
".stats{display:grid;grid-template-columns:1fr 1fr;gap:.6rem;margin-top:1.2rem;font-size:.9rem;border-top:1px dashed #ffb000;padding-top:1rem}"
".stat-label{color:#ffb000;opacity:0.8}.stat-val{font-weight:bold;text-align:right}"
".tabs{display:flex;gap:0;margin-bottom:1.5rem;flex-wrap:wrap;border:1px solid #ffb000;width:100%}"
".tab{flex:1 1 33.33%;padding:1.2rem .5rem;background:#000;border:none;border-right:1px solid #ffb000;border-bottom:1px solid #ffb000;color:#ffb000;cursor:pointer;font-family:inherit;font-size:.8rem;text-transform:uppercase;text-align:center;font-weight:bold}"
"@media(min-width:600px){.tab{flex:none;padding:.75rem 1.25rem;font-size:.875rem;border-bottom:none}}"
".tab.active{background:#ffb000;color:#000}"
".panel{display:none;border:1px solid #ffb000;padding:1.5rem;background:rgba(255,176,0,0.02)}"
".panel.active{display:block}"
".form-group{margin-bottom:1.8rem}"
"label{display:block;font-size:1rem;color:#ffb000;margin-bottom:.8rem;font-weight:bold;opacity:0.9}"
"input,select{width:100%;padding:1.2rem 1rem;background:#000;border:1px solid #ffb000;color:#ffb000;font-family:inherit;font-size:1.2rem;outline:none;min-height:4rem}"
"input[type='checkbox']{width:auto;min-height:auto;transform:scale(2);margin-right:20px;vertical-align:middle}"
".btn{display:inline-block;padding:1.2rem 1.5rem;border:1px solid #ffb000;cursor:pointer;font-family:inherit;font-size:1.1rem;font-weight:bold;background:#000;color:#ffb000;text-transform:uppercase;text-align:center;width:100%;transition:all 0.1s;margin-bottom:0.5rem}"
"@media(min-width:600px){.btn{width:auto;margin-right:0.5rem}}"
".btn:active{background:#ffb000;color:#000;transform:translateY(2px)}"
".btn-danger{border-color:#ff0000;color:#ff0000}.btn-danger:active{background:#ff0000;color:#000}"
".btn-secondary{border-style:dashed}.btn-sm{padding:.6rem 1.2rem;font-size:.9rem;width:auto}"
".toast{position:fixed;bottom:2rem;left:50%;transform:translateX(-50%);z-index:10000;padding:1.2rem 2.5rem;background:#111;color:#ffb000;border:3px solid #ffb000;font-weight:bold;opacity:0;transition:opacity 0.3s;pointer-events:none;min-width:300px;text-align:center;box-shadow:0 0 30px rgba(255,176,0,0.4);font-size:1.1rem}"
".toast.show{opacity:1;pointer-events:auto}"
".overlay{position:fixed;top:0;left:0;right:0;bottom:0;background:rgba(0,0,0,0.98);z-index:9000;display:none;flex-direction:column;align-items:center;justify-content:center;padding:2rem;text-align:center}"
".loader{border:8px solid #222;border-top:8px solid #ffb000;width:90px;height:90px;animation:spin 1s linear infinite;margin-bottom:2.5rem;border-radius:50% !important}"
"@keyframes spin{0%{transform:rotate(0deg)}100%{transform:rotate(360deg)}}"
".tx-table-container{overflow-x:auto;margin-top:1.5rem;border:1px solid #ffb000}"
".tx-table{width:100%;border-collapse:collapse;font-size:1rem;min-width:750px}"
".tx-table th{text-align:left;padding:1.2rem;color:#000;background:#ffb000;font-weight:bold}"
".tx-table td{padding:1.2rem;border-bottom:1px solid #ffb000}"
"hr{border:none;border-top:1px solid #ffb000;margin:2rem 0}"
"</style></head><body>"
"<div id='pairing-overlay' class='overlay'>"
"<div id='pair-icon' class='loader'></div>"
"<h2 id='pair-title' style='font-size:2rem;margin-bottom:0.5rem'>PAIRING IN PROGRESS</h2>"
"<p id='pair-msg' style='margin:1rem 0;font-size:1.1rem;opacity:0.85'>HOLD BUTTON ON TRANSMITTER FOR 2 SECONDS</p>"
"<div id='pair-timer' style='font-size:5rem;margin:1.5rem 0;font-weight:bold;letter-spacing:4px'>60S</div>"
"<p id='pair-sub' style='font-size:0.85rem;opacity:0.6;margin-bottom:1.5rem'>TIMER IS SERVER-SIDE — PHONE SLEEP SAFE</p>"
"<button id='pair-cancel' class='btn' style='border-color:#f00;color:#f00' onclick='closePairing()'>CANCEL</button>"
"</div>"
"<div id='global-toast' class='toast'></div>"
"<header><h1>[ TANK_SYNC ]</h1><div id='conn-status' class='badge waiting'>...</div></header>"
"<main><div class='tabs'>"
"<button class='tab active' onclick=\"switchTab('tanks',this)\">TANKS</button>"
"<button class='tab' onclick=\"switchTab('transmitters',this)\">DEVICES</button>"
"<button class='tab' onclick=\"switchTab('wifi',this)\">WIFI</button>"
"<button class='tab' onclick=\"switchTab('mqtt',this)\">MQTT</button>"
"<button class='tab' onclick=\"switchTab('lora',this)\">LORA</button>"
"<button class='tab' onclick=\"switchTab('display',this)\">DISPLAY</button>"
"<button class='tab' onclick=\"switchTab('ota',this)\">OTA</button>"
"<button class='tab' onclick=\"switchTab('link',this)\">LINK</button>"
"</div>"
"<div id='panel-tanks' class='panel active'><div id='tanks-grid' class='tanks'></div></div>"
"<div id='panel-transmitters' class='panel'>"
"<div class='card'><div class='card-title' style='display:flex;justify-content:space-between;align-items:center'><span>EDIT / PAIR</span><button id='pair-btn' class='btn btn-sm' onclick='togglePairing()'>PAIR NEW</button></div>"
"<div style='display:grid;grid-template-columns:repeat(auto-fit,minmax(250px,1fr));gap:1.5rem;margin-bottom:1.5rem'>"
"<div><label>ADDR</label><input type='number' id='tx-addr'></div>"
"<div><label>NAME</label><input type='text' id='tx-name' maxlength='15'></div>"
"<div><label>MIN CM</label><input type='number' id='tx-min'></div>"
"<div><label>MAX CM</label><input type='number' id='tx-max'></div>"
"<div><label>CAP L</label><input type='number' id='tx-cap'></div>"
"<div><label>SLEEP S</label><input type='number' id='tx-sleep'></div>"
"<div><label>SAMPLES</label><input type='number' id='tx-samp'></div>"
"</div><button class='btn' onclick='saveTx()'>SAVE DEVICE</button></div>"
"<div class='card'><div class='card-title' style='display:flex;justify-content:space-between;align-items:center'><span>REGISTERED</span><button class='btn btn-sm' style='border-color:#f00;color:#f00' onclick='clearAllTx()'>CLEAR ALL</button></div>"
"<div class='tx-table-container'><table class='tx-table'><thead><tr><th>ADDR</th><th>NAME</th><th>MIN</th><th>MAX</th><th>CAP</th><th>SLP</th><th>SMP</th><th>ST</th><th></th></tr></thead><tbody id='tx-tbody'></tbody></table></div></div></div>"
"<div id='panel-wifi' class='panel'><div class='card'><div class='card-title'>WIFI</div><div id='wifi-status' style='margin-bottom:1rem'></div>"
"<div class='form-group'><label>SSID</label><input type='text' id='wifi-ssid'></div>"
"<div class='form-group'><label>PASSWORD</label><input type='password' id='wifi-pass'></div>"
"<button class='btn' onclick='wifiConnect()'>CONNECT</button><button class='btn btn-secondary' onclick='wifiScan()' style='margin-top:1rem'>SCAN</button><ul id='scan-list' style='list-style:none;border:1px solid #ffb000;margin-top:1.5rem'></ul></div></div>"
"<div id='panel-mqtt' class='panel'><div class='card'><div class='card-title'>MQTT BROKER</div>"
"<div style='display:flex;align-items:center;gap:1rem;margin-bottom:1.5rem;padding:.8rem;border:1px dashed #ffb000'>"
"<span style='opacity:0.7;font-size:.8rem'>STATUS</span>"
"<span id='mqtt-live-badge' class='badge waiting'>...</span>"
"</div>"
"<div><label>HOST</label><input type='text' id='mqtt-host' style='margin-bottom:1rem'></div>"
"<div><label>PORT</label><input type='number' id='mqtt-port' value='1883' style='margin-bottom:1rem'></div>"
"<div><label>USER</label><input type='text' id='mqtt-user' style='margin-bottom:1rem'></div>"
"<div><label>PASS</label><input type='password' id='mqtt-pass' placeholder='(UNCHANGED)' style='margin-bottom:1rem'></div>"
"<label><input type='checkbox' id='mqtt-en'> ENABLE MQTT</label><br><label><input type='checkbox' id='mqtt-ha'> HA DISCOVERY</label><br><button class='btn' onclick='saveMqtt()' style='margin-top:1.5rem'>SAVE</button></div></div>"
"<div id='panel-lora' class='panel'><div class='card'><div class='card-title'>LORA</div>"
"<div><label>FREQ</label><input type='number' id='lora-freq' style='margin-bottom:1rem'></div>"
"<div><label>ADDR</label><input type='number' id='lora-addr' style='margin-bottom:1rem'></div>"
"<button class='btn' onclick='saveLora()'>SAVE</button></div></div>"
"<div id='panel-display' class='panel'><div class='card'><div class='card-title'>OLED</div>"
"<label><input type='checkbox' id='scr-water'> WATER</label><br><label><input type='checkbox' id='scr-battery'> BATTERY</label><br>"
"<label><input type='checkbox' id='scr-signal'> SIGNAL</label><br><label><input type='checkbox' id='scr-diag'> DIAG</label><br>"
"<label><input type='checkbox' id='scr-system'> SYSTEM</label><br><button class='btn' onclick='saveDisplay()' style='margin-top:1.5rem'>SAVE</button></div></div>"
"<div id='panel-ota' class='panel'>"
/* ── Firmware Versions card ── */
"<div class='card'><div class='card-title'>FIRMWARE VERSIONS</div>"
"<div id='fw-ver-grid' style='display:grid;grid-template-columns:1fr 1fr;gap:1rem;margin-bottom:1rem'>"
"<div><div style='font-size:.75rem;opacity:.7;margin-bottom:.4rem'>RECEIVER</div><div id='rx-ver' style='font-size:1.1rem;font-weight:bold'>...</div></div>"
"<div><div style='font-size:.75rem;opacity:.7;margin-bottom:.4rem'>TX STAGED</div><div id='tx-staged-ver' style='font-size:1.1rem;font-weight:bold'>NONE</div></div>"
"</div>"
"<div id='spiffs-info' style='font-size:.7rem;opacity:.5;margin-top:.5rem'></div>"
"<div id='tx-dev-list' style='margin-top:.5rem'></div>"
"</div>"
/* ── Transmitter OTA card ── */
"<div class='card' style='border-color:#0af'><div class='card-title' style='color:#0af'>TRANSMITTER OTA</div>"
"<p style='font-size:.8rem;opacity:.8;margin-bottom:1.2rem'>STEP 1: STAGE FIRMWARE &nbsp;→&nbsp; STEP 2: DEPLOY TO DEVICES ON NEXT WAKE</p>"
"<div style='margin-bottom:1rem'>"
"<label style='font-size:.8rem;opacity:.7;margin-bottom:.5rem'>STEP 1 — STAGE TX BINARY ON RECEIVER</label>"
"<div style='display:flex;gap:.5rem;flex-wrap:wrap;margin-top:.5rem'>"
"<input type='file' id='tx-bin-file' accept='.bin' style='flex:1;min-width:200px;margin-bottom:0'>"
"<button class='btn btn-secondary' onclick='uploadTx()' id='tx-upload-btn'>STAGE BINARY</button>"
"<button class='btn btn-danger btn-sm' onclick='clearStaged()' id='tx-clear-btn' style='display:none'>CLEAR</button>"
"</div>"
"<div id='tx-upload-progress' style='display:none;margin-top:.8rem'>"
"<div class='level-bar'><div class='level-fill' id='tx-upload-bar' style='width:0%'></div></div>"
"<div id='tx-upload-label' style='font-size:.75rem;margin-top:.3rem'>0%</div>"
"</div>"
"<div id='tx-staged-info' style='font-size:.8rem;margin-top:.6rem;opacity:.7'></div>"
"</div>"
"<hr>"
"<div style='margin-top:1rem'>"
"<label style='font-size:.8rem;opacity:.7;margin-bottom:.5rem'>STEP 2 — DEPLOY TO ALL PAIRED DEVICES</label>"
"<p style='font-size:.75rem;margin-bottom:1rem;opacity:.6'>DEVICES RECEIVE UPDATE ON THEIR NEXT WAKE CYCLE (up to 5 min)</p>"
"<button class='btn' id='sync-btn' onclick='syncFirmware()' disabled>DEPLOY TO ALL DEVICES</button>"
"<div id='sync-status' style='font-size:.8rem;margin-top:.8rem'></div>"
"<div id='ota-devices-progress' style='margin-top:1rem'></div>"
"</div>"
"</div>"
/* ── Receiver OTA card ── */
"<div class='card'><div class='card-title'>RECEIVER OTA</div><div id='ota-status' style='margin-bottom:1rem'>IDLE</div>"
"<button class='btn' onclick='otaCheck()' style='margin-bottom:1rem'>CHECK UPDATES</button>"
"<div id='ota-progress' style='display:none'><div class='level-bar'><div class='level-fill' id='ota-bar'></div></div></div>"
"<hr style='margin:1.5rem 0'>"
"<label style='font-size:.8rem;opacity:.7;margin-bottom:.5rem'>MANUAL UPLOAD (RECEIVER BIN)</label>"
"<div style='margin-top:.5rem'>"
"<input type='file' id='bin-file' accept='.bin' style='margin-bottom:.5rem'>"
"<button class='btn btn-secondary' onclick='uploadBin()'>UPLOAD RECEIVER BIN</button>"
"</div></div>"
"</div>"
/* ── LINK / QR Code panel ── */
"<div id='panel-link' class='panel'>"
"<div class='card' style='text-align:center'>"
"<div class='card-title'>LINK TO TANKSYNC APP</div>"
"<p style='font-size:.85rem;opacity:.8;margin-bottom:1.5rem'>SCAN THIS QR CODE WITH YOUR PHONE CAMERA TO LINK THIS DEVICE TO YOUR TANKSYNC ACCOUNT</p>"
"<div id='qr-container' style='display:inline-block;padding:16px;background:#fff;margin-bottom:1.5rem'></div>"
"<div style='margin-bottom:1rem'>"
"<div style='font-size:.75rem;opacity:.6;margin-bottom:.3rem'>OR COPY THIS LINK:</div>"
"<input type='text' id='link-url' readonly style='text-align:center;font-size:.75rem;cursor:pointer' onclick='this.select();document.execCommand(\"copy\");showToast(\"COPIED!\",true)'>"
"</div>"
"<div style='display:grid;grid-template-columns:1fr 1fr;gap:1rem;margin-top:1.5rem;font-size:.8rem;opacity:.7'>"
"<div>DEVICE ID: <span id='link-devid' style='font-weight:bold'>...</span></div>"
"<div>IP: <span id='link-ip' style='font-weight:bold'>...</span></div>"
"</div>"
"</div></div>"
"</main>"
"<script>"
"let pairPoll=null;"
"function switchTab(t,el){document.querySelectorAll('.panel').forEach(p=>p.classList.remove('active'));document.querySelectorAll('.tab').forEach(b=>b.classList.remove('active'));"
"const p=document.getElementById('panel-'+t);if(p)p.classList.add('active');if(el)el.classList.add('active');"
"if(t==='wifi')loadWifi();if(t==='mqtt')loadMqtt();if(t==='lora')loadLora();if(t==='transmitters')loadTransmitters();if(t==='display')loadDisplay();if(t==='system')loadSystem();if(t==='ota')loadOta();if(t==='link')loadLink();}"
"function showToast(msg,ok=true){const t=document.getElementById('global-toast');t.textContent='> '+msg.toUpperCase();t.className='toast show'+(ok?'':' toast-error');setTimeout(()=>t.classList.remove('show'),3000);}"
"async function api(p,o){const r=await fetch(p,o);return r.json();}"
"function stateClass(s){return s==='online'?'online':s==='stale'?'stale':s==='offline'?'offline':'waiting'}"
"async function loadTanks(){const d=await api('/api/data');const c=document.getElementById('conn-status');if(c){let label=d.wifi_status.toUpperCase();if(d.wifi_connected&&d.mqtt_connected)label='WIFI+MQTT';else if(d.wifi_connected)label='WIFI ONLY';c.textContent=label;c.className='badge '+stateClass(d.wifi_connected?'online':'offline');}"
"const g=document.getElementById('tanks-grid');if(!g)return;if(!d.tanks||!d.tanks.length){g.innerHTML='<p>[!] EMPTY</p>';return;}g.innerHTML=d.tanks.map(t=>`<div class='card'><div style='display:flex;justify-content:space-between;align-items:start'><div class='tank-name'>${t.name}</div>"
"<span class='badge ${stateClass(t.state)}'>[${t.state.toUpperCase()}]</span></div><div style='font-size:2.5rem;font-weight:bold'>${t.water_pct}%</div>"
"<div class='level-bar'><div class='level-fill' style='width:${t.water_pct}%'></div></div><div class='stats'><span class='stat-label'>VOL</span><span class='stat-val'>${t.water_liters.toFixed(0)}L</span>"
"<span class='stat-label'>DIST</span><span class='stat-val'>${t.raw_dist}CM</span><span class='stat-label'>RSSI</span><span class='stat-val'>${t.rssi}</span><span class='stat-label'>SEEN</span><span class='stat-val'>${t.last_seen_s>=0?(t.last_seen_s<60?t.last_seen_s+'S':Math.round(t.last_seen_s/60)+'M'):'NEVER'}</span></div></div>`).join('');}"
"async function loadTransmitters(){const d=await api('/api/transmitters');const tb=document.getElementById('tx-tbody');if(!tb)return;if(!d.transmitters||!d.transmitters.length){tb.innerHTML='<tr><td colspan=\"9\">EMPTY</td></tr>';return;}"
"tb.innerHTML=d.transmitters.map(t=>`<tr><td>${t.address}</td><td>${t.name}</td><td>${t.min_dist}</td><td>${t.max_dist}</td><td>${t.capacity}</td><td>${t.sleep}S</td><td>${t.samples}</td><td>${t.state}</td><td>"
"<button class='btn btn-sm' onclick='editTx(${t.address},\"${t.name}\",${t.min_dist},${t.max_dist},${t.capacity},${t.sleep},${t.samples})'>EDIT</button></td></tr>`).join('');}"
"function editTx(a,n,mn,mx,c,s,sa){document.getElementById('tx-addr').value=a;document.getElementById('tx-name').value=n;document.getElementById('tx-min').value=mn;document.getElementById('tx-max').value=mx;document.getElementById('tx-cap').value=c;document.getElementById('tx-sleep').value=s;document.getElementById('tx-samp').value=sa;window.scrollTo(0,0);}"
"async function saveTx(){const b={addr:+document.getElementById('tx-addr').value,name:document.getElementById('tx-name').value,min_dist:+document.getElementById('tx-min').value,max_dist:+document.getElementById('tx-max').value,capacity:+document.getElementById('tx-cap').value,sleep:+document.getElementById('tx-sleep').value,samples:+document.getElementById('tx-samp').value};"
"const r=await api('/api/transmitters',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});showToast(r.message||'SAVED');loadTransmitters();}"
"async function clearAllTx(){if(confirm('PURGE ALL?')){await api('/api/transmitters/clear',{method:'POST'});showToast('CLEARED');loadTransmitters();}}"
"function pairShow(state,msg,timer){"
"const ov=document.getElementById('pairing-overlay');"
"const icon=document.getElementById('pair-icon');"
"const title=document.getElementById('pair-title');"
"const pmsg=document.getElementById('pair-msg');"
"const psub=document.getElementById('pair-sub');"
"const ptimer=document.getElementById('pair-timer');"
"const btn=document.getElementById('pair-cancel');"
"ov.style.display='flex';"
"if(state==='waiting'){"
"icon.style.display='block';icon.style.color='';"
"title.textContent='PAIRING IN PROGRESS';title.style.color='';"
"pmsg.textContent='HOLD BUTTON ON TRANSMITTER FOR 2 SECONDS';"
"psub.textContent='TIMER IS SERVER-SIDE \u2014 PHONE SLEEP SAFE';"
"ptimer.style.display='block';ptimer.textContent=(timer||60)+'S';"
"btn.textContent='CANCEL';btn.style.color='#f00';btn.style.borderColor='#f00';"
"}else if(state==='success'){"
"icon.style.display='none';"
"title.textContent='\u2714 PAIRED!';title.style.color='#0f0';"
"pmsg.textContent=msg||'NEW DEVICE REGISTERED';"
"psub.textContent='DEVICE WILL APPEAR IN TANKS LIST';"
"ptimer.style.display='none';"
"btn.textContent='CLOSE';btn.style.color='#0f0';btn.style.borderColor='#0f0';"
"}else if(state==='failed'){"
"icon.style.display='none';"
"title.textContent='\u2716 PAIRING FAILED';title.style.color='#f44';"
"pmsg.textContent=msg||'NO RESPONSE FROM TRANSMITTER';"
"psub.textContent='ENSURE RECEIVER IS IN PAIRING MODE AND RETRY';"
"ptimer.style.display='none';"
"btn.textContent='CLOSE';btn.style.color='#f44';btn.style.borderColor='#f44';"
"}}"
"async function togglePairing(){"
"if(pairPoll){clearInterval(pairPoll);pairPoll=null;}"
"await api('/api/lora/pairing?start=1',{method:'POST'});"
"pairShow('waiting',null,60);"
"pairPoll=setInterval(async()=>{"
"try{const r=await api('/api/lora/pairing');"
"if(r.paired){"
"clearInterval(pairPoll);pairPoll=null;"
"pairShow('success','PAIRED: '+(r.name||'ADDR '+r.addr));"
"showToast('PAIRED',true);loadTransmitters();"
"}else if(r.active){"
"document.getElementById('pair-timer').textContent=r.time_left+'S';"
"}else{"
"clearInterval(pairPoll);pairPoll=null;"
"pairShow('failed','TIMEOUT — NO TRANSMITTER RESPONDED');"
"showToast('TIMEOUT',false);"
"}}catch(e){/* ignore fetch errors while phone was asleep */}"
"},1000);}"
"async function closePairing(){"
"if(pairPoll){clearInterval(pairPoll);pairPoll=null;}"
"document.getElementById('pairing-overlay').style.display='none';"
"await api('/api/lora/pairing?start=0',{method:'POST'});}"
"async function loadWifi(){const d=await api('/api/system');document.getElementById('wifi-status').textContent=d.ssid||'DISCONNECTED';}"
"async function wifiScan(){const ul=document.getElementById('scan-list');ul.innerHTML='<li style=\"padding:1rem;opacity:.5\">SCANNING...</li>';try{const r=await api('/api/wifi/scan');const nets=r.networks||[];if(!nets.length){ul.innerHTML='<li style=\"padding:1rem;opacity:.5\">NO NETWORKS FOUND</li>';return;}"
"ul.innerHTML=nets.map(n=>`<li style='padding:1.2rem;cursor:pointer;border-bottom:1px solid #333' onclick='document.getElementById(\"wifi-ssid\").value=\"${n.ssid}\"'>${n.ssid} (${n.rssi}dBm)</li>`).join('');}catch(e){ul.innerHTML='<li style=\"padding:1rem;color:#f44\">SCAN FAILED</li>';showToast('SCAN FAILED',false);}}"
"async function wifiConnect(){const s=document.getElementById('wifi-ssid').value;if(!s){showToast('ENTER SSID',false);return;}"
"const b={ssid:s,password:document.getElementById('wifi-pass').value};await api('/api/wifi/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});showToast('CONNECTING TO '+s+'...');"
"let tries=0;const poll=setInterval(async()=>{tries++;if(tries>10){clearInterval(poll);showToast('TIMEOUT — CHECK CREDENTIALS',false);document.getElementById('wifi-status').textContent='FAILED';return;}"
"try{const d=await api('/api/system');if(d.wifi_status==='connected'){clearInterval(poll);showToast('CONNECTED! IP: '+d.ip,true);document.getElementById('wifi-status').textContent=d.ssid+' ('+d.ip+')';}"
"else{document.getElementById('wifi-status').textContent='CONNECTING ('+tries+'/10)...';}}catch(e){}},3000);}"
"async function saveMqtt(){const b={host:document.getElementById('mqtt-host').value,port:+document.getElementById('mqtt-port').value,user:document.getElementById('mqtt-user').value,pass:document.getElementById('mqtt-pass').value,enabled:document.getElementById('mqtt-en').checked,ha_discovery:document.getElementById('mqtt-ha').checked};"
"const r=await api('/api/mqtt',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});showToast(r.message||'SAVED');}"
"async function saveLora(){const b={freq:+document.getElementById('lora-freq').value,addr:+document.getElementById('lora-addr').value};const r=await api('/api/lora',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});showToast(r.message||'SAVED');}"
"async function saveDisplay(){let m=0;if(document.getElementById('scr-water').checked)m|=1;if(document.getElementById('scr-battery').checked)m|=2;if(document.getElementById('scr-signal').checked)m|=4;if(document.getElementById('scr-diag').checked)m|=8;if(document.getElementById('scr-system').checked)m|=16;"
"const r=await api('/api/display',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({mask:m})});showToast(r.message||'SAVED');}"
"async function loadMqtt(){const d=await api('/api/mqtt');document.getElementById('mqtt-host').value=d.host;document.getElementById('mqtt-port').value=d.port;document.getElementById('mqtt-user').value=d.user;document.getElementById('mqtt-en').checked=!!d.enabled;document.getElementById('mqtt-ha').checked=!!d.ha_discovery;"
"const b=document.getElementById('mqtt-live-badge');if(b){const st=d.live_status||'disabled';const cls=st==='connected'?'online':st==='connecting'||st==='disconnected'?'stale':'offline';b.textContent=st.toUpperCase();b.className='badge '+cls;}}"
"async function loadLora(){const d=await api('/api/lora');document.getElementById('lora-freq').value=d.freq;document.getElementById('lora-addr').value=d.addr;}"
"async function loadDisplay(){const d=await api('/api/display');const m=d.mask;document.getElementById('scr-water').checked=!!(m&1);document.getElementById('scr-battery').checked=!!(m&2);document.getElementById('scr-signal').checked=!!(m&4);document.getElementById('scr-diag').checked=!!(m&8);document.getElementById('scr-system').checked=!!(m&16);}"
"function uploadTx(){"
"const f=document.getElementById('tx-bin-file').files[0];"
"if(!f){alert('SELECT A .BIN FILE');return;}"
"const prog=document.getElementById('tx-upload-progress');"
"const bar=document.getElementById('tx-upload-bar');"
"const lbl=document.getElementById('tx-upload-label');"
"const info=document.getElementById('tx-staged-info');"
"const btn=document.getElementById('tx-upload-btn');"
"prog.style.display='block';btn.disabled=true;info.textContent='';"
"const xhr=new XMLHttpRequest();"
"xhr.upload.onprogress=e=>{if(e.lengthComputable){const pct=Math.round(e.loaded*100/e.total);bar.style.width=pct+'%';lbl.textContent=pct+'% ('+Math.round(e.loaded/1024)+'KB / '+Math.round(e.total/1024)+'KB)';}};"
"xhr.onload=()=>{"
"btn.disabled=false;"
"try{"
"const r=JSON.parse(xhr.responseText);"
"if(r.ok){showToast('STAGED '+Math.round(r.bytes/1024)+'KB',true);loadOta();}"
"else{showToast(r.message||'UPLOAD FAILED',false);info.textContent='ERROR: '+(r.message||'upload failed');prog.style.display='none';}"
"}catch(e){showToast('SERVER ERROR',false);prog.style.display='none';}"
"};"
"xhr.onerror=()=>{btn.disabled=false;showToast('NETWORK ERROR',false);prog.style.display='none';};"
"xhr.open('POST','/api/ota/upload_tx');"
"xhr.send(f);"
"}"
"let otaPoll=null;"
"async function syncFirmware(){"
"const r=await api('/api/lora/ota_sync',{method:'POST'});"
"if(!r.ok){showToast(r.message||'ERROR',false);return;}"
"showToast('DEPLOYING '+Math.round(r.bytes/1024)+'KB TO ALL DEVICES',true);"
"document.getElementById('sync-status').textContent='QUEUED — WAITING FOR DEVICE WAKE CYCLES...';"
"if(otaPoll)clearInterval(otaPoll);"
"otaPoll=setInterval(loadOtaDevices,4000);"
"}"
"async function loadOtaDevices(){"
"const d=await api('/api/ota/tx_status');"
"const el=document.getElementById('ota-devices-progress');"
"if(!d.devices||!d.devices.length){el.innerHTML='<p style=\"opacity:.5;font-size:.8rem\">NO PAIRED DEVICES</p>';return;}"
"let anyPending=false;"
"el.innerHTML=d.devices.map(dev=>{"
"const pct=dev.ota_pending&&dev.ota_offset>0?'...':'';"
"const badge=dev.ota_pending?'<span class=\"badge stale\">OTA PENDING</span>':'<span class=\"badge online\">UP TO DATE</span>';"
"if(dev.ota_pending)anyPending=true;"
"return `<div style='border:1px solid #ffb000;padding:.8rem;margin-bottom:.5rem;display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:.5rem'>`"
"+`<span>${dev.name} <span style='opacity:.5;font-size:.75rem'>(${dev.addr||dev.address})</span></span>`"
"+`<span style='font-size:.75rem;opacity:.7'>TX VER: ${dev.fw_version}</span>`"
"+badge+'</div>';"
"}).join('');"
"if(!anyPending&&otaPoll){clearInterval(otaPoll);otaPoll=null;document.getElementById('sync-status').textContent='ALL DEVICES UPDATED OR PENDING NEXT WAKE';}"
"}"
"async function clearStaged(){"
"if(!confirm('CLEAR STAGED FIRMWARE?'))return;"
"await api('/api/ota/clear_tx',{method:'POST'});showToast('STAGED FIRMWARE CLEARED');loadOta();}"
"async function loadOta(){"
"const sys=await api('/api/system');"
"const rxv=document.getElementById('rx-ver');if(rxv)rxv.textContent=sys.version||'?';"
"const staged=await api('/api/ota/tx_staged');"
"const stv=document.getElementById('tx-staged-ver');"
"const info=document.getElementById('tx-staged-info');"
"const syncBtn=document.getElementById('sync-btn');"
"const clrBtn=document.getElementById('tx-clear-btn');"
"if(staged.staged){"
"const ver=staged.version||'?';"
"const kb=Math.round(staged.bytes/1024);"
"stv.textContent='V'+ver+' ('+kb+'KB)';"
"info.textContent='STAGED: V'+ver+' — '+kb+'KB';"
"if(syncBtn)syncBtn.disabled=false;"
"if(clrBtn)clrBtn.style.display='';"
"}else{"
"stv.textContent='NONE';"
"info.textContent='';"
"if(syncBtn)syncBtn.disabled=true;"
"if(clrBtn)clrBtn.style.display='none';"
"}"
"try{const sp=await api('/api/spiffs/info');"
"const el=document.getElementById('spiffs-info');"
"if(el)el.textContent='STORAGE: '+Math.round(sp.used/1024)+'KB / '+Math.round(sp.total/1024)+'KB ('+Math.round(sp.free/1024)+'KB FREE)';"
"}catch(e){}"
"await loadOtaDevices();"
"}"
"async function otaCheck(){document.getElementById('ota-status').textContent='CHECKING...';const r=await api('/api/ota/check',{method:'POST'});showToast('CHECKING GITHUB',true);setTimeout(async()=>{const s=await api('/api/ota/state');document.getElementById('ota-status').textContent=s.status.toUpperCase()+(s.latest_version?' ('+s.latest_version+')':'');},3000);}"
"function uploadBin(){const f=document.getElementById('bin-file').files[0];if(!f){alert('SELECT FILE');return;}"
"const xhr=new XMLHttpRequest();"
"xhr.open('POST','/api/ota/upload');"
"function waitReboot(){showToast('FLASHING — REBOOTING...',true);"
"document.getElementById('ota-status').textContent='REBOOTING — WAITING FOR DEVICE...';"
"let tries=0;const poll=setInterval(async()=>{tries++;if(tries>30){clearInterval(poll);showToast('DEVICE NOT RESPONDING',false);return;}"
"try{const r=await fetch('/api/system',{signal:AbortSignal.timeout(3000)});if(r.ok){clearInterval(poll);showToast('UPDATE COMPLETE — RELOADED',true);setTimeout(()=>location.reload(),1500);}}catch(e){}},3000);}"
"xhr.onload=()=>{try{const r=JSON.parse(xhr.responseText);if(r.ok)waitReboot();else showToast(r.message||'ERROR',false);}catch(e){waitReboot();}};"
"xhr.onerror=()=>waitReboot();"
"showToast('UPLOADING...',true);xhr.send(f);}"
"async function loadSystem(){const d=await api('/api/system');const g=document.getElementById('sys-grid');if(g)g.innerHTML=[['VER',d.version],['IP',d.ip],['HEAP',d.free_heap],['UPTIME',d.uptime_s+'S']].map(([l,v])=>`<div class='sys-card'><div class='sys-label'>${l}</div><div class='sys-val'>${v}</div></div>`).join('');}"
/* ── QR Code generator (minimal, alphanumeric mode, version 6, ECC L) ── */
/* Generates a simple table-based QR code using the Google Charts API as a    */
/* fallback-free approach: we render the QR as an <img> from an inline SVG.  */
/* Uses a lightweight approach: encode the URL into a QR via API endpoint.   */
"async function loadLink(){"
"const d=await api('/api/link');"
"document.getElementById('link-url').value=d.url;"
"document.getElementById('link-devid').textContent=d.device_id;"
"document.getElementById('link-ip').textContent=d.ip;"
/* Generate QR code as a table of cells using a minimal encoder */
/* We use the receiver itself to encode — simpler than shipping a JS QR lib */
/* Fallback: just show the URL text for manual entry */
"const c=document.getElementById('qr-container');"
"try{"
  "const size=200;"
  "c.innerHTML=`<img src='https://api.qrserver.com/v1/create-qr-code/?size=${size}x${size}&data=${encodeURIComponent(d.url)}&bgcolor=FFFFFF&color=000000' "
  "width='${size}' height='${size}' alt='QR Code' style='image-rendering:pixelated' "
  "onerror=\"this.parentNode.innerHTML='<div style=color:#000;padding:2rem;font-size:.8rem>QR GENERATION FAILED<br>USE THE LINK BELOW</div>'\">`;"
"}catch(e){c.innerHTML='<div style=color:#000;padding:2rem>USE LINK BELOW</div>';}"
"}"
"loadTanks();setInterval(loadTanks,5000);</script></body></html>";

static void send_json(httpd_req_t *req, const char *json) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json);
}
static void send_ok(httpd_req_t *req, const char *msg) {
    char buf[128]; snprintf(buf, sizeof(buf), "{\"ok\":true,\"message\":\"%s\"}", msg ? msg : "OK"); send_json(req, buf);
}
static void send_err(httpd_req_t *req, const char *msg) {
    char buf[128]; snprintf(buf, sizeof(buf), "{\"ok\":false,\"message\":\"%s\"}", msg ? msg : "Error"); httpd_resp_set_status(req, "400 Bad Request"); send_json(req, buf);
}
static char *read_body(httpd_req_t *req) {
    int len = req->content_len; if (len <= 0 || len > 4096) return NULL;
    char *buf = malloc(len + 1); if (!buf) return NULL;
    int received = 0; while (received < len) { int r = httpd_req_recv(req, buf + received, len - received); if (r <= 0) { free(buf); return NULL; } received += r; }
    buf[len] = '\0'; return buf;
}

static esp_err_t handle_root(httpd_req_t *req) { httpd_resp_set_type(req, "text/html; charset=utf-8"); httpd_resp_sendstr(req, DASHBOARD_HTML); return ESP_OK; }

// Captive portal detection — redirect to root so popup opens the web UI
// iOS:     GET /hotspot-detect.html  — expects non-"Success" body to show popup
// Android: GET /generate_204         — expects non-204 to show popup
// Windows: GET /connecttest.txt      — expects non-"Microsoft Connect Test" to show popup
static esp_err_t handle_captive_redirect(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_sendstr(req, "Redirecting to TankSync setup...");
    return ESP_OK;
}

static esp_err_t handle_api_data(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    wifi_status_t ws = wifi_manager_status();
    cJSON_AddStringToObject(root, "wifi_status", ws == WIFI_ST_CONNECTED ? "connected" : "ap_mode");
    cJSON_AddBoolToObject(root, "wifi_connected", ws == WIFI_ST_CONNECTED);

    mqtt_mgr_status_t ms = mqtt_manager_status();
    const char *mqtt_str = (ms == MQTT_ST_CONNECTED)    ? "connected"    :
                           (ms == MQTT_ST_CONNECTING)   ? "connecting"   :
                           (ms == MQTT_ST_DISCONNECTED) ? "disconnected" :
                           (ms == MQTT_ST_ERROR)        ? "error"        : "disabled";
    cJSON_AddStringToObject(root, "mqtt_status", mqtt_str);
    cJSON_AddBoolToObject(root, "mqtt_connected", ms == MQTT_ST_CONNECTED);
    cJSON *tanks = cJSON_AddArrayToObject(root, "tanks");
    for (int i = 0; i < registry_count(); i++) {
        tx_info_t info; tx_data_t data;
        if (!registry_get_info(i, &info) || !registry_get_data(i, &data)) continue;
        if (!info.enabled) continue;
        cJSON *t = cJSON_CreateObject();
        cJSON_AddNumberToObject(t, "address", info.address);
        cJSON_AddStringToObject(t, "name", info.name);
        cJSON_AddNumberToObject(t, "water_pct", data.water_pct);
        cJSON_AddNumberToObject(t, "water_liters", data.water_liters);
        cJSON_AddNumberToObject(t, "raw_dist", data.raw_dist_cm);
        cJSON_AddNumberToObject(t, "battery_pct", data.battery_pct);
        cJSON_AddNumberToObject(t, "battery_v", data.battery_voltage);
        cJSON_AddNumberToObject(t, "rssi", data.rssi);
        cJSON_AddStringToObject(t, "state", registry_state_str(data.state));
        int64_t age_s = data.last_update_us > 0
            ? (esp_timer_get_time() - data.last_update_us) / 1000000LL : -1;
        cJSON_AddNumberToObject(t, "last_seen_s", (double)age_s);
        cJSON_AddItemToArray(tanks, t);
    }
    char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root); send_json(req, json); free(json);
    return ESP_OK;
}

static esp_err_t handle_api_system(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", FIRMWARE_VERSION);
    cJSON_AddStringToObject(root, "ip", wifi_manager_ip());
    cJSON_AddStringToObject(root, "ssid", wifi_manager_ssid());
    cJSON_AddStringToObject(root, "wifi_status", wifi_manager_status() == WIFI_ST_CONNECTED ? "connected" : "ap_mode");
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root); send_json(req, json); free(json);
    return ESP_OK;
}

// ── Cloud/PWA Linking ────────────────────────────────────────────────────────
// Generates a per-boot random claim token. Returns a URL that, when opened in a
// browser, auto-links this receiver to the user's TankSync PWA account.
// Token regenerates on each reboot — no NVS persistence needed.
static char s_link_token[9] = {0}; // 8 hex chars + null

static void ensure_link_token(void) {
    if (s_link_token[0] != '\0') return;
    uint32_t r1 = esp_random();
    snprintf(s_link_token, sizeof(s_link_token), "%08" PRIx32, r1);
}

static esp_err_t handle_api_link(httpd_req_t *req) {
    ensure_link_token();
    const char *dev_id = mqtt_manager_device_id();
    const char *ip = wifi_manager_ip();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", dev_id);
    cJSON_AddStringToObject(root, "token", s_link_token);
    cJSON_AddStringToObject(root, "ip", ip);
    cJSON_AddStringToObject(root, "version", FIRMWARE_VERSION);

    // Build the claim URL
    char url[256];
    snprintf(url, sizeof(url), TANKSYNC_CLOUD_URL "/link?id=%s&token=%s&ip=%s", dev_id, s_link_token, ip);
    cJSON_AddStringToObject(root, "url", url);

    char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root); send_json(req, json); free(json);
    return ESP_OK;
}

static esp_err_t handle_get_transmitters(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject(); cJSON *arr = cJSON_AddArrayToObject(root, "transmitters");
    for (int i = 0; i < registry_count(); i++) {
        tx_info_t info; tx_data_t data;
        if (!registry_get_info(i, &info)) continue;
        registry_get_data(i, &data);
        bool ota_p = false; uint32_t ota_o = 0;
        registry_get_ota_status(info.address, &ota_p, &ota_o);
        cJSON *t = cJSON_CreateObject();
        cJSON_AddNumberToObject(t, "address",    info.address);
        cJSON_AddStringToObject(t, "name",       info.name);
        cJSON_AddNumberToObject(t, "min_dist",   info.min_dist_cm);
        cJSON_AddNumberToObject(t, "max_dist",   info.max_dist_cm);
        cJSON_AddNumberToObject(t, "capacity",   info.capacity_liters);
        cJSON_AddNumberToObject(t, "sleep",      info.sleep_s);
        cJSON_AddNumberToObject(t, "samples",    info.samples);
        cJSON_AddStringToObject(t, "state",      registry_state_str(data.state));
        cJSON_AddStringToObject(t, "fw_version", data.fw_version[0] ? data.fw_version : "unknown");
        cJSON_AddBoolToObject  (t, "ota_pending", ota_p);
        cJSON_AddNumberToObject(t, "ota_offset", ota_o);
        cJSON_AddItemToArray(arr, t);
    }
    char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root); send_json(req, json); free(json);
    return ESP_OK;
}

static esp_err_t handle_post_transmitters(httpd_req_t *req) {
    char *body = read_body(req); if (!body) return ESP_OK;
    cJSON *j = cJSON_Parse(body); free(body); if (!j) return ESP_OK;
    uint16_t addr = (uint16_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "addr"));
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(j, "name"));
    int min_dist = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "min_dist"));
    int max_dist = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "max_dist"));
    float cap = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "capacity"));
    uint32_t sleep = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "sleep"));
    uint8_t samples = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "samples"));
    cJSON_Delete(j);
    if (addr == 0) { send_err(req, "Invalid addr"); return ESP_OK; }
    bool updated = registry_update(addr, name, min_dist, max_dist, cap);
    if (!updated) { if (registry_add(addr, name, min_dist, max_dist, cap) < 0) { send_err(req, "Full"); return ESP_OK; } }
    if (sleep >= 60) { registry_set_remote_config(addr, sleep, samples > 0 ? samples : 5); }
    send_ok(req, "Saved"); return ESP_OK;
}

static esp_err_t handle_delete_transmitter(httpd_req_t *req) {
    const char *last_slash = strrchr(req->uri, '/'); if (!last_slash) return ESP_OK;
    uint16_t addr = (uint16_t)atoi(last_slash + 1);
    registry_remove(addr) ? send_ok(req, "OK") : send_err(req, "No"); return ESP_OK;
}

static esp_err_t handle_clear_transmitters(httpd_req_t *req) { registry_clear_all(); send_ok(req, "OK"); return ESP_OK; }

static esp_err_t handle_lora_pairing(httpd_req_t *req) {
    if (req->method == HTTP_POST) {
        char qry[32] = {0};
        if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) == ESP_OK) {
            char buf[8] = {0};
            if (httpd_query_key_value(qry, "start", buf, sizeof(buf)) == ESP_OK) {
                lora_set_pairing_mode(atoi(buf) > 0);
            }
        }
        send_ok(req, "OK");
    } else {
        bool active = false; uint16_t addr = 0; char name[16] = {0}; int time_left = 0;
        lora_get_pairing_state(&active, &addr, name, &time_left);
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "active", active);
        cJSON_AddBoolToObject(root, "paired", addr > 0);
        cJSON_AddNumberToObject(root, "time_left", time_left);
        if (addr > 0) {
            cJSON_AddNumberToObject(root, "addr", addr);
            cJSON_AddStringToObject(root, "name", name);
        }
        char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root); send_json(req, json); free(json);
    }
    return ESP_OK;
}

static esp_err_t handle_wifi_scan(httpd_req_t *req) {
    char *json_arr = wifi_manager_scan_json(); if (!json_arr) { send_err(req, "Fail"); return ESP_OK; }
    char *resp = malloc(strlen(json_arr) + 32); sprintf(resp, "{\"networks\":%s}", json_arr); free(json_arr);
    send_json(req, resp); free(resp); return ESP_OK;
}

static esp_err_t handle_wifi_connect(httpd_req_t *req) {
    char *body = read_body(req); if (!body) return ESP_OK;
    cJSON *j = cJSON_Parse(body); free(body);
    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(j, "ssid")), *pass = cJSON_GetStringValue(cJSON_GetObjectItem(j, "password"));
    if (ssid) { wifi_manager_save_credentials(ssid, pass ? pass : ""); }
    cJSON_Delete(j); send_ok(req, "Connecting"); return ESP_OK;
}

static esp_err_t handle_wifi_forget(httpd_req_t *req) { wifi_manager_forget(); send_ok(req, "OK"); return ESP_OK; }

static esp_err_t handle_get_mqtt(httpd_req_t *req) {
    mqtt_mgr_config_t cfg; mqtt_manager_get_config(&cfg);
    mqtt_mgr_status_t ms = mqtt_manager_status();
    const char *mqtt_live = (ms == MQTT_ST_CONNECTED)    ? "connected"    :
                            (ms == MQTT_ST_CONNECTING)   ? "connecting"   :
                            (ms == MQTT_ST_DISCONNECTED) ? "disconnected" :
                            (ms == MQTT_ST_ERROR)        ? "error"        : "disabled";
    cJSON *root = cJSON_CreateObject(); cJSON_AddStringToObject(root, "host", cfg.host); cJSON_AddNumberToObject(root, "port", cfg.port);
    cJSON_AddStringToObject(root, "user", cfg.user); cJSON_AddBoolToObject(root, "enabled", cfg.enabled); cJSON_AddBoolToObject(root, "ha_discovery", cfg.ha_discovery);
    cJSON_AddBoolToObject(root, "use_tls", cfg.use_tls);
    cJSON_AddStringToObject(root, "live_status", mqtt_live);
    char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root); send_json(req, json); free(json); return ESP_OK;
}

static esp_err_t handle_post_mqtt(httpd_req_t *req) {
    char *body = read_body(req); if (!body) return ESP_OK;
    cJSON *j = cJSON_Parse(body); free(body); mqtt_mgr_config_t cfg = {0};
    const char *h = cJSON_GetStringValue(cJSON_GetObjectItem(j, "host")), *u = cJSON_GetStringValue(cJSON_GetObjectItem(j, "user")), *p = cJSON_GetStringValue(cJSON_GetObjectItem(j, "pass"));
    if (h) { strncpy(cfg.host, h, sizeof(cfg.host)-1); }
    if (u) { strncpy(cfg.user, u, sizeof(cfg.user)-1); }
    if (p) { strncpy(cfg.pass, p, sizeof(cfg.pass)-1); }
    cfg.port = (uint16_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "port")); cfg.enabled = cJSON_IsTrue(cJSON_GetObjectItem(j, "enabled")); cfg.ha_discovery = cJSON_IsTrue(cJSON_GetObjectItem(j, "ha_discovery"));
    cfg.use_tls = cJSON_IsTrue(cJSON_GetObjectItem(j, "use_tls"));
    cJSON_Delete(j); if (mqtt_manager_set_config(&cfg) == ESP_OK) { send_ok(req, "OK"); } else { send_err(req, "NO"); }
    return ESP_OK;
}

static esp_err_t handle_get_lora(httpd_req_t *req) {
    lora_config_t cfg; lora_get_config(&cfg);
    cJSON *root = cJSON_CreateObject(); cJSON_AddNumberToObject(root, "freq", (double)cfg.freq_hz); cJSON_AddNumberToObject(root, "addr", cfg.address);
    char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root); send_json(req, json); free(json); return ESP_OK;
}

static esp_err_t handle_post_lora(httpd_req_t *req) {
    char *body = read_body(req);
    if (!body) { send_err(req, "No body"); return ESP_OK; }
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) { send_err(req, "Bad JSON"); return ESP_OK; }
    // Read current config first — only overlay provided fields (audit RX#1)
    lora_config_t cfg;
    lora_get_config(&cfg);
    cJSON *v;
    if ((v = cJSON_GetObjectItem(j, "freq"))) cfg.freq_hz = (uint32_t)v->valuedouble;
    if ((v = cJSON_GetObjectItem(j, "addr"))) cfg.address  = (uint16_t)v->valueint;
    cJSON_Delete(j);
    if (lora_set_config(&cfg) == ESP_OK) { send_ok(req, "OK"); } else { send_err(req, "NO"); }
    return ESP_OK;
}

static esp_err_t handle_ota_state(httpd_req_t *req) {
    ota_state_t st; ota_manager_get_state(&st); static const char *sns[] = { "idle","checking","available","downloading","done","up_to_date","error" };
    cJSON *root = cJSON_CreateObject(); cJSON_AddStringToObject(root, "status", sns[st.status]); cJSON_AddNumberToObject(root, "progress", st.progress_pct);
    char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root); send_json(req, json); free(json); return ESP_OK;
}
static esp_err_t handle_ota_check(httpd_req_t *req) { ota_manager_check_github(); send_ok(req, "OK"); return ESP_OK; }
// Wrapper task for GitHub OTA — proper FreeRTOS task function (audit RX#15)
static void ota_flash_task(void *arg) {
    char *url = (char *)arg;
    ota_manager_flash_url(url);
    free(url);
    vTaskDelete(NULL);
}
static esp_err_t handle_ota_update(httpd_req_t *req) {
    ota_state_t st; ota_manager_get_state(&st);
    if (st.status != OTA_ST_AVAILABLE) { send_err(req, "No update available"); return ESP_OK; }
    char *u = strdup(st.download_url);
    xTaskCreate(ota_flash_task, "ota", 8192, u, 5, NULL);
    send_ok(req, "Started"); return ESP_OK;
}
static esp_err_t handle_ota_upload(httpd_req_t *req) {
    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t h;
    if (esp_ota_begin(p, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) {
        send_err(req, "OTA begin failed");  // audit RX#2: was missing response
        return ESP_OK;
    }
    char *b = malloc(1024);
    if (!b) { esp_ota_abort(h); send_err(req, "OOM"); return ESP_OK; }
    int t = 0, l = req->content_len;
    while (t < l) {
        int n = httpd_req_recv(req, b, (l - t) > 1024 ? 1024 : (l - t));
        if (n <= 0) break;
        esp_ota_write(h, b, n);
        t += n;
    }
    free(b);
    if (t == l && esp_ota_end(h) == ESP_OK && esp_ota_set_boot_partition(p) == ESP_OK) {
        send_ok(req, "Rebooting...");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else {
        // audit RX#3: partial upload or OTA end failure — clean up and respond
        if (t != l) esp_ota_abort(h);
        send_err(req, "Upload incomplete or verification failed");
    }
    return ESP_OK;
}

static esp_err_t handle_ota_upload_tx(httpd_req_t *req) {
    int l = req->content_len;
    if (l <= 0 || l > 2 * 1024 * 1024) { send_err(req, "Bad size"); return ESP_OK; }
    // Remove existing file first so SPIFFS frees the blocks immediately
    remove("/spiffs/tx_fw.bin");
    FILE *f = fopen("/spiffs/tx_fw.bin", "w");
    if (!f) { send_err(req, "SPIFFS full or error"); return ESP_OK; }
    char *b = malloc(2048); if (!b) { fclose(f); send_err(req, "OOM"); return ESP_OK; }
    int t = 0;
    while (t < l) {
        int n = httpd_req_recv(req, b, (l - t) > 2048 ? 2048 : (l - t));
        if (n <= 0) break;
        fwrite(b, 1, n, f);
        t += n;
    }
    fclose(f); free(b);
    if (t == l) {
        ESP_LOGI(TAG, "TX firmware staged: %d bytes at /spiffs/tx_fw.bin", t);
        // Return size so the UI can confirm
        char buf[64]; snprintf(buf, sizeof(buf), "{\"ok\":true,\"bytes\":%d}", t);
        send_json(req, buf);
    } else {
        remove("/spiffs/tx_fw.bin");  // partial write — clean up
        send_err(req, "Upload incomplete");
    }
    return ESP_OK;
}

static esp_err_t handle_sync_ota(httpd_req_t *req) {
    // Check staged firmware exists first
    FILE *chk = fopen("/spiffs/tx_fw.bin", "rb");
    if (!chk) { send_err(req, "No TX firmware staged. Upload a .bin first."); return ESP_OK; }
    fseek(chk, 0, SEEK_END); long sz = ftell(chk); fclose(chk);
    if (sz <= 0) { send_err(req, "Staged file is empty"); return ESP_OK; }
    registry_set_all_ota_pending(true);
    char buf[64]; snprintf(buf, sizeof(buf), "{\"ok\":true,\"bytes\":%ld}", sz);
    send_json(req, buf);
    return ESP_OK;
}

static esp_err_t handle_ota_tx_staged(httpd_req_t *req) {
    FILE *f = fopen("/spiffs/tx_fw.bin", "rb");
    if (!f) { send_json(req, "{\"staged\":false,\"bytes\":0,\"version\":\"\"}"); return ESP_OK; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fclose(f);
    char ver[32] = "";
    extract_staged_version(ver, sizeof(ver));
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"staged\":true,\"bytes\":%ld,\"version\":\"%s\"}", sz, ver);
    send_json(req, buf);
    return ESP_OK;
}

static esp_err_t handle_ota_clear_tx(httpd_req_t *req) {
    remove("/spiffs/tx_fw.bin");
    // Cancel any pending OTA across all transmitters
    registry_set_all_ota_pending(false);
    ESP_LOGI(TAG, "Staged TX firmware cleared");
    send_ok(req, "Staged firmware cleared");
    return ESP_OK;
}

static esp_err_t handle_spiffs_info(httpd_req_t *req) {
    size_t total = 0, used = 0;
    registry_get_spiffs_info(&total, &used);
    char buf[128];
    snprintf(buf, sizeof(buf), "{\"total\":%d,\"used\":%d,\"free\":%d}",
             (int)total, (int)used, (int)(total - used));
    send_json(req, buf);
    return ESP_OK;
}

static esp_err_t handle_ota_tx_status(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "devices");
    int n = registry_count();
    for (int i = 0; i < n; i++) {
        tx_info_t info; tx_data_t data;
        if (!registry_get_info(i, &info) || !registry_get_data(i, &data)) continue;
        bool ota_p = false; uint32_t ota_o = 0;
        registry_get_ota_status(info.address, &ota_p, &ota_o);
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "addr",       info.address);
        cJSON_AddStringToObject(d, "name",       info.name);
        cJSON_AddStringToObject(d, "fw_version", data.fw_version[0] ? data.fw_version : "unknown");
        cJSON_AddBoolToObject  (d, "ota_pending", ota_p);
        cJSON_AddNumberToObject(d, "ota_offset", ota_o);
        cJSON_AddStringToObject(d, "state",      registry_state_str(data.state));
        cJSON_AddItemToArray(arr, d);
    }
    char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    send_json(req, json); free(json);
    return ESP_OK;
}

static esp_err_t handle_get_display(httpd_req_t *req) {
    uint8_t m = 0x1F; nvs_handle_t h; if (nvs_open("display", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "mask", &m); nvs_close(h); }
    char buf[32]; sprintf(buf, "{\"mask\":%d}", m); send_json(req, buf); return ESP_OK;
}
static esp_err_t handle_post_display(httpd_req_t *req) {
    char *body = read_body(req); cJSON *j = cJSON_Parse(body); free(body); if (!j) return ESP_OK;
    uint8_t m = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "mask"));
    cJSON_Delete(j); nvs_handle_t h; if (nvs_open("display", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "mask", m); nvs_commit(h); nvs_close(h); }
    send_ok(req, "OK"); return ESP_OK;
}

#define URI(m, p, h) { .uri = (p), .method = (m), .handler = (h), .user_ctx = NULL }
static const httpd_uri_t s_routes[] = {
    URI(HTTP_GET, "/", handle_root), URI(HTTP_GET, "/api/data", handle_api_data), URI(HTTP_GET, "/api/system", handle_api_system),
    URI(HTTP_GET, "/api/transmitters", handle_get_transmitters), URI(HTTP_POST, "/api/transmitters", handle_post_transmitters),
    URI(HTTP_POST, "/api/transmitters/clear", handle_clear_transmitters), URI(HTTP_DELETE, "/api/transmitters/*", handle_delete_transmitter),
    URI(HTTP_POST, "/api/lora/ota_sync", handle_sync_ota),
    URI(HTTP_GET,  "/api/ota/tx_staged", handle_ota_tx_staged),
    URI(HTTP_POST, "/api/ota/clear_tx",  handle_ota_clear_tx),
    URI(HTTP_GET,  "/api/ota/tx_status", handle_ota_tx_status),
    URI(HTTP_GET,  "/api/spiffs/info",   handle_spiffs_info),
    URI(HTTP_GET, "/api/wifi/scan", handle_wifi_scan), URI(HTTP_POST, "/api/wifi/connect", handle_wifi_connect), URI(HTTP_POST, "/api/wifi/forget", handle_wifi_forget),
    URI(HTTP_GET, "/api/mqtt", handle_get_mqtt), URI(HTTP_POST, "/api/mqtt", handle_post_mqtt), URI(HTTP_GET, "/api/lora", handle_get_lora), URI(HTTP_POST, "/api/lora", handle_post_lora),
    URI(HTTP_GET, "/api/lora/pairing", handle_lora_pairing), URI(HTTP_POST, "/api/lora/pairing", handle_lora_pairing),
    URI(HTTP_GET, "/api/ota/state", handle_ota_state), URI(HTTP_POST, "/api/ota/check", handle_ota_check), URI(HTTP_POST, "/api/ota/update", handle_ota_update),
    URI(HTTP_POST, "/api/ota/upload", handle_ota_upload), URI(HTTP_POST, "/api/ota/upload_tx", handle_ota_upload_tx),
    URI(HTTP_GET, "/api/display", handle_get_display), URI(HTTP_POST, "/api/display", handle_post_display),
    URI(HTTP_GET, "/api/link", handle_api_link),
    // Captive portal detection endpoints
    URI(HTTP_GET, "/hotspot-detect.html", handle_captive_redirect),  // iOS
    URI(HTTP_GET, "/library/test/success.html", handle_captive_redirect), // iOS alternate
    URI(HTTP_GET, "/generate_204", handle_captive_redirect),          // Android
    URI(HTTP_GET, "/gen_204", handle_captive_redirect),               // Android alternate
    URI(HTTP_GET, "/connecttest.txt", handle_captive_redirect),       // Windows
    URI(HTTP_GET, "/ncsi.txt", handle_captive_redirect),              // Windows alternate
    URI(HTTP_GET, "/redirect", handle_captive_redirect),              // Firefox
};

esp_err_t web_server_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port        = WEB_PORT;
    cfg.max_uri_handlers   = 38;
    cfg.uri_match_fn       = httpd_uri_match_wildcard;
    cfg.max_open_sockets   = 6;          // leave 4+ lwIP sockets for MQTT/DNS/NTP
    cfg.recv_wait_timeout  = 15;         // balance: short enough to free sockets, long enough for TX firmware upload
    cfg.send_wait_timeout  = 5;
    cfg.lru_purge_enable   = true;       // auto-close oldest connection when sockets full
    esp_err_t err = httpd_start(&s_server, &cfg); if (err != ESP_OK) return err;
    for (int i = 0; i < (int)(sizeof(s_routes)/sizeof(s_routes[0])); i++) { httpd_register_uri_handler(s_server, &s_routes[i]); }
    ESP_LOGI(TAG, "Server started");
    return ESP_OK;
}
void web_server_stop(void) { if (s_server) { httpd_stop(s_server); s_server = NULL; } }
