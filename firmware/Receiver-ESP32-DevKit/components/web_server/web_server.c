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
#include "geo_time.h"
#include "buzzer.h"
#include "log_buffer.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_spiffs.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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
"<!doctype html><html lang='en'><head>\n"
"<meta charset='utf-8'>\n"
"<meta name='viewport' content='width=device-width,initial-scale=1,viewport-fit=cover'>\n"
"<meta name='theme-color' content='#eef1f4'>\n"
"<title>TankSync</title>\n"
"<style>\n"
":root{--mist:#eef1f4;--mist2:#e2e7ec;--paper:#fafbfc;--paper2:#f4f6f8;--ink:#0f1620;--ink2:#3a4654;--ink3:#6b7886;--line:#d8dde3;--line2:#c2c9d2;--rain:#4a6b9c;--rain2:#6f8cb8;--rain-soft:#e6ecf3;--warm:#b87a3c;--warm-soft:#f4e9d8;--leaf:#4a7a5c;--leaf-soft:#e3ede6;--rust:#a8423a;--rust-soft:#f1dad6;--serif:\"Iowan Old Style\",\"Charter\",\"Georgia\",\"Times New Roman\",serif;--sans:-apple-system,BlinkMacSystemFont,\"Segoe UI\",Roboto,sans-serif;--mono:ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;--pad:24px;--shadow-sm:0 1px 2px rgba(15,22,32,.04);--shadow:0 1px 3px rgba(15,22,32,.06),0 4px 14px rgba(15,22,32,.04)}\n"
"*{box-sizing:border-box}html,body{margin:0;padding:0}\n"
"body{font-family:var(--sans);font-size:15px;line-height:1.5;color:var(--ink);background:var(--mist);background-image:radial-gradient(ellipse at top,#f6f8fa 0%,var(--mist) 70%);-webkit-font-smoothing:antialiased;min-height:100vh;-webkit-tap-highlight-color:transparent}\n"
".serif{font-family:var(--serif)}.italic{font-style:italic}.mono{font-family:var(--mono)}\n"
".wrap{max-width:1240px;margin:0 auto;padding:24px var(--pad) 80px}\n"
".banner{display:grid;grid-template-columns:1fr auto 1fr;align-items:end;gap:16px;border-bottom:1px solid var(--ink);padding-bottom:14px;margin-bottom:18px}\n"
".banner .l,.banner .r{font-size:11px}.banner .r{text-align:right}\n"
".banner-meta{font-family:var(--serif);font-style:italic;font-size:13px;color:var(--ink3)}\n"
".banner-sub{font-size:10px;letter-spacing:.18em;text-transform:uppercase;color:var(--ink3);margin-top:2px}\n"
".masthead{font-family:var(--serif);font-weight:600;font-size:clamp(34px,7vw,52px);line-height:.92;letter-spacing:-.035em;margin:0;text-align:center;white-space:nowrap}\n"
".masthead em{font-style:italic;font-weight:400;color:var(--rain)}\n"
".tabs-wrap{border-bottom:1px solid var(--line);margin-bottom:28px;position:sticky;top:0;z-index:20;background:rgba(238,241,244,.94);backdrop-filter:blur(8px);-webkit-backdrop-filter:blur(8px)}\n"
".tabs{display:flex;gap:clamp(14px,3.5vw,30px);justify-content:center;flex-wrap:nowrap;overflow-x:auto;scrollbar-width:none;-webkit-mask-image:linear-gradient(90deg,transparent 0,#000 16px,#000 calc(100% - 16px),transparent 100%)}\n"
".tabs::-webkit-scrollbar{display:none}\n"
".tab{padding:12px 4px 14px;font-family:var(--serif);font-size:17px;font-weight:500;color:var(--ink3);background:transparent;border:0;cursor:pointer;position:relative;white-space:nowrap;transition:color .15s}\n"
".tab:hover{color:var(--ink2)}.tab.active{color:var(--ink)}\n"
".tab.active::after{content:\"\";position:absolute;left:0;right:0;bottom:-1px;height:2px;background:var(--rain)}\n"
".panel{display:none;animation:fade .25s ease}.panel.active{display:block}\n"
"@keyframes fade{from{opacity:0;transform:translateY(4px)}to{opacity:1;transform:none}}\n"
".hero{display:grid;grid-template-columns:1.4fr 1fr;gap:40px;align-items:start;margin-bottom:40px}\n"
".hero .label{font-size:11px;letter-spacing:.22em;text-transform:uppercase;color:var(--rain);margin-bottom:12px;font-weight:500}\n"
".hero h2{font-family:var(--serif);font-weight:400;font-size:clamp(26px,4.2vw,40px);line-height:1.05;letter-spacing:-.025em;margin:0 0 14px;text-wrap:pretty}\n"
".hero h2 em{font-style:italic;color:var(--rain)}\n"
".hero p.lede{font-family:var(--serif);font-size:clamp(15px,1.8vw,18px);line-height:1.5;color:var(--ink2);margin:0 0 16px;max-width:48ch;text-wrap:pretty}\n"
".hero-summary{background:var(--paper);border:1px solid var(--line);padding:22px;display:grid;gap:12px}\n"
".hs-row{display:flex;align-items:baseline;justify-content:space-between;gap:12px;padding-bottom:10px;border-bottom:1px dotted var(--line)}\n"
".hs-row:last-child{border-bottom:0;padding-bottom:0}\n"
".hs-name{font-family:var(--serif);font-size:16px;font-weight:500}\n"
".hs-name small{display:block;font-family:var(--sans);font-size:10px;letter-spacing:.18em;text-transform:uppercase;color:var(--ink3);font-weight:400;margin-top:2px}\n"
".hs-pct{font-family:var(--serif);font-weight:500;font-size:26px;letter-spacing:-.03em;font-variant-numeric:tabular-nums}\n"
".hs-pct sub{font-size:11px;color:var(--ink3);margin-left:2px}\n"
".section-head{display:flex;align-items:baseline;gap:14px;margin-bottom:24px}\n"
".section-head h2{font-family:var(--serif);font-weight:500;font-size:clamp(20px,2.6vw,28px);letter-spacing:-.02em;margin:0}\n"
".section-head .rule{flex:1;height:1px;background:var(--ink);margin:0 4px;align-self:center}\n"
".section-head .meta{font-family:var(--serif);font-style:italic;color:var(--ink3);font-size:13px;white-space:nowrap}\n"
".tanks{display:grid;grid-template-columns:repeat(3,1fr);gap:1px;background:var(--line);border:1px solid var(--line);margin-bottom:40px}\n"
".tcard{background:var(--paper);padding:28px 22px 22px;display:grid;grid-template-rows:auto auto 1fr auto auto;gap:11px;position:relative;cursor:pointer;transition:background .15s}\n"
".tcard:hover{background:#fff}\n"
".tcard.offline{background:repeating-linear-gradient(135deg,var(--paper) 0 8px,var(--paper2) 8px 9px)}\n"
".tcard.waiting{border:1px dashed var(--line2);margin:-1px}\n"
".state-eye{font-size:10px;letter-spacing:.22em;text-transform:uppercase;color:var(--ink3);display:flex;align-items:center;gap:8px;font-weight:500}\n"
".state-eye::before{content:\"\";display:inline-block;width:7px;height:7px;border-radius:50%;background:var(--leaf)}\n"
".tcard.online .state-eye{color:var(--leaf)}\n"
".tcard.online .state-eye::before{background:var(--leaf);box-shadow:0 0 0 3px var(--leaf-soft)}\n"
".tcard.stale .state-eye{color:var(--warm)}\n"
".tcard.stale .state-eye::before{background:transparent;box-shadow:inset 0 0 0 1.5px var(--warm)}\n"
".tcard.offline .state-eye{color:var(--rust)}\n"
".tcard.offline .state-eye::before{background:var(--rust);height:2px;width:12px;border-radius:0}\n"
".tcard.waiting .state-eye::before{background:transparent;border:1.5px dotted var(--ink3)}\n"
".tcard h3{font-family:var(--serif);font-weight:500;font-size:26px;letter-spacing:-.025em;margin:0;line-height:1}\n"
".tcard-del{position:absolute;top:10px;right:10px;width:30px;height:30px;border:none;background:transparent;color:var(--ink3);cursor:pointer;border-radius:4px;display:flex;align-items:center;justify-content:center;opacity:.4;transition:opacity .15s,color .15s,background .15s}\n"
".tcard-del:hover{opacity:1;color:var(--rust);background:var(--rust-soft)}\n"
".tcard-del svg{width:16px;height:16px;stroke:currentColor;fill:none;stroke-width:1.5;stroke-linecap:round;stroke-linejoin:round}\n"
".tcard h3 em{font-style:italic;color:var(--rain);font-weight:400}\n"
".tcard .addr{font-family:var(--mono);font-size:11px;letter-spacing:.05em;color:var(--ink3)}\n"
".body-block{display:grid;grid-template-columns:100px 1fr;gap:16px;align-items:center;margin:6px 0}\n"
".visual{display:flex;align-items:center;justify-content:center;min-height:160px}\n"
".visual svg{display:block;width:100%;max-width:100px;height:auto}\n"
".pct-block{min-width:0}\n"
".pct{font-family:var(--serif);font-weight:300;font-size:clamp(58px,9vw,76px);line-height:.9;letter-spacing:-.05em;font-variant-numeric:tabular-nums}\n"
".tcard.stale .pct{color:var(--warm)}.tcard.offline .pct{color:var(--ink3)}\n"
".pct sup{font-size:20px;vertical-align:32px;margin-left:4px;color:var(--ink3);font-weight:400}\n"
".vol{font-family:var(--serif);font-style:italic;font-size:14px;color:var(--ink2);margin-top:2px;line-height:1.3}\n"
".predict{font-family:var(--serif);font-size:14px;color:var(--rain);padding:9px 11px;background:var(--rain-soft);border-left:2px solid var(--rain);display:flex;align-items:center;gap:8px;text-wrap:pretty}\n"
".tcard.stale .predict{color:var(--warm);background:var(--warm-soft);border-left-color:var(--warm)}\n"
".tcard.offline .predict{color:var(--rust);background:var(--rust-soft);border-left-color:var(--rust)}\n"
".predict svg{flex:0 0 14px}\n"
/* Phase 2 (v2.5.0) — subtle 24h water-level sparkline + power-pill line.
   Both sit between .predict and .meta-grid in the tcard. Editorial-tuned:
   stroke-only sparkline (no fills, no axes), serif power pill, dotted gap
   indicator for empty history slots. */
".spark{height:26px;display:block;width:100%;color:var(--rain);opacity:.78}\n"
".tcard.stale .spark{color:var(--warm)}.tcard.offline .spark{color:var(--ink3)}\n"
".pwr{font-family:var(--serif);font-size:13px;color:var(--ink2);display:flex;align-items:center;gap:8px;flex-wrap:wrap;line-height:1.35;letter-spacing:0}\n"
".pwr .pwr-pct{font-variant-numeric:tabular-nums;color:var(--ink)}\n"
".pwr .pwr-sep{color:var(--ink3)}\n"
".pwr .pwr-state{display:inline-flex;align-items:center;gap:4px;font-style:italic}\n"
".pwr .pwr-state.chg{color:var(--leaf)}\n"
".pwr .pwr-state.dis{color:var(--ink2)}\n"
".pwr .pwr-state.hold{color:var(--ink-soft)}\n"
".pwr .pwr-state.warn{color:var(--warm)}\n"
".pwr .pwr-state.alert{color:var(--rust)}\n"
".pwr .pwr-state .dt{width:6px;height:6px;border-radius:50%;background:currentColor;flex:0 0 6px}\n"
".pwr .pwr-num{font-variant-numeric:tabular-nums;font-family:var(--mono);font-size:11.5px;color:var(--ink3)}\n"
".pwr .pwr-trend{font-style:italic;font-size:11.5px;color:var(--ink3)}\n"
".pwr .pwr-trend.up{color:var(--leaf)}\n"
".pwr .pwr-trend.down{color:var(--warm)}\n"
".pending-chip{font-style:italic;font-size:.85em;color:var(--warm);background:rgba(0,0,0,.03);padding:1px 6px;border-radius:8px;margin-left:6px}\n"
".pwr .pwr-mode{font-style:italic;font-size:.8em;color:var(--ink3);opacity:.75}\n"
".meta-grid{margin:0;padding-top:14px;border-top:1px solid var(--line);display:grid;grid-template-columns:1fr 1fr 1fr;gap:4px 12px}\n"
".meta-grid dt{font-size:10px;letter-spacing:.18em;text-transform:uppercase;color:var(--ink3);margin:0}\n"
".meta-grid dd{font-family:var(--serif);font-size:14px;margin:2px 0 0;font-variant-numeric:tabular-nums}\n"
".insights{display:grid;grid-template-columns:1fr 1.6fr;gap:32px;margin-bottom:40px;padding-top:24px;border-top:1px solid var(--ink)}\n"
".insights-head h2{font-family:var(--serif);font-weight:500;font-size:clamp(20px,2.8vw,26px);letter-spacing:-.02em;margin:0 0 8px;line-height:1.1}\n"
".insights-head .by{font-family:var(--serif);font-style:italic;font-size:13px;color:var(--ink3)}\n"
".insights-head .upd{font-size:10px;letter-spacing:.2em;text-transform:uppercase;color:var(--ink3);margin-top:14px}\n"
".insight-list{display:grid;gap:16px}\n"
".insight{display:grid;grid-template-columns:26px 1fr;gap:14px;align-items:start;padding-bottom:14px;border-bottom:1px dotted var(--line)}\n"
".insight:last-child{border-bottom:0;padding-bottom:0}\n"
".insight .ico{width:26px;height:26px;display:flex;align-items:center;justify-content:center;border-radius:50%;background:var(--rain-soft);color:var(--rain);margin-top:2px;flex:0 0 26px}\n"
".insight.warn .ico{background:var(--warm-soft);color:var(--warm)}\n"
".insight.alert .ico{background:var(--rust-soft);color:var(--rust)}\n"
".insight.good .ico{background:var(--leaf-soft);color:var(--leaf)}\n"
".insight h4{font-family:var(--serif);font-weight:500;font-size:18px;letter-spacing:-.015em;margin:0 0 4px;line-height:1.25;text-wrap:pretty}\n"
".insight h4 em{font-style:italic;color:var(--rain)}\n"
".insight.warn h4 em{color:var(--warm)}.insight.alert h4 em{color:var(--rust)}.insight.good h4 em{color:var(--leaf)}\n"
".insight p{font-family:var(--serif);font-size:14px;color:var(--ink2);margin:0;line-height:1.5;text-wrap:pretty}\n"
".num-strong{font-family:var(--serif);font-variant-numeric:tabular-nums;font-weight:500;color:var(--ink)}\n"
".status-strip{display:grid;grid-template-columns:repeat(4,1fr);gap:22px;padding:22px 0;border-top:1px solid var(--ink);border-bottom:1px solid var(--ink);margin-bottom:30px}\n"
".status-strip .lbl{font-size:10px;letter-spacing:.22em;text-transform:uppercase;color:var(--ink3);margin-bottom:6px}\n"
".status-strip .val{font-family:var(--serif);font-weight:500;font-size:clamp(18px,2.4vw,24px);letter-spacing:-.02em;font-variant-numeric:tabular-nums}\n"
".status-strip .sub{font-family:var(--serif);font-style:italic;font-size:12px;color:var(--ink3);margin-top:2px}\n"
".dot{display:inline-block;width:9px;height:9px;border-radius:50%;background:var(--leaf);vertical-align:middle;margin-right:8px}\n"
".dot.warn{background:transparent;box-shadow:inset 0 0 0 2px var(--warm)}\n"
".dot.err{background:var(--rust)}\n"
".ec{background:var(--paper);border:1px solid var(--line);margin-bottom:26px}\n"
".ec-head{padding:20px var(--pad);border-bottom:1px solid var(--line);display:flex;align-items:baseline;gap:14px;flex-wrap:wrap}\n"
".ec-head h3{font-family:var(--serif);font-weight:500;font-size:22px;letter-spacing:-.02em;margin:0}\n"
".ec-head h3 em{font-style:italic;color:var(--rain);font-weight:400}\n"
".ec-head .lede-sm{font-family:var(--serif);font-style:italic;font-size:13px;color:var(--ink3);margin:0}\n"
".ec-body{padding:20px var(--pad)}\n"
".ec-foot{padding:14px var(--pad);border-top:1px solid var(--line);background:var(--paper2);display:flex;align-items:center;gap:10px;flex-wrap:wrap}\n"
".field-row{display:grid;grid-template-columns:1fr;gap:16px}\n"
"@media(min-width:600px){.field-row.c2{grid-template-columns:1fr 1fr}.field-row.c3{grid-template-columns:1fr 1fr 1fr}}\n"
".field-row+.field-row{margin-top:16px}\n"
".field{display:grid;gap:6px;min-width:0}\n"
".lbl{font-size:10px;letter-spacing:.22em;text-transform:uppercase;color:var(--ink3);font-weight:500}\n"
".help{font-family:var(--serif);font-style:italic;font-size:13px;color:var(--ink3);margin-top:2px}\n"
"input[type=text],input[type=number],input[type=password],select,textarea{appearance:none;width:100%;font-family:var(--sans);font-size:15px;color:var(--ink);padding:11px 13px;background:#fff;border:1px solid var(--line);border-radius:0;transition:border-color .15s,box-shadow .15s}\n"
"input.serifish{font-family:var(--serif);font-size:17px}\n"
"input.mi{font-family:var(--mono);font-size:14px;letter-spacing:-.01em}\n"
"input:focus,select:focus,textarea:focus{outline:none;border-color:var(--rain);box-shadow:0 0 0 3px var(--rain-soft)}\n"
"input[readonly]{background:var(--mist);color:var(--ink2)}\n"
".is{display:flex;align-items:stretch}.is input{border-right:0}\n"
".is .sf{padding:0 12px;display:inline-flex;align-items:center;background:var(--mist);border:1px solid var(--line);font-family:var(--serif);font-style:italic;font-size:13px;color:var(--ink2)}\n"
"select{background-image:url(\"data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' width='12' height='8' viewBox='0 0 12 8' fill='none'><path d='M1 1.5L6 6.5L11 1.5' stroke='%236b7886' stroke-width='1.4' stroke-linecap='round' stroke-linejoin='round'/></svg>\");background-repeat:no-repeat;background-position:right 14px center;padding-right:36px}\n"
".chk{display:flex;align-items:flex-start;gap:10px;padding:11px 13px;border:1px solid var(--line);background:#fff;cursor:pointer;user-select:none;transition:background .15s,border-color .15s}\n"
".chk input{margin-top:3px;accent-color:var(--rain);flex:0 0 auto}\n"
".chk .ct{font-family:var(--serif);font-size:15px;font-weight:500}\n"
".chk .ck{font-family:var(--serif);font-style:italic;font-size:12px;color:var(--ink3);margin-top:2px}\n"
"input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:24px;background:transparent}\n"
"input[type=range]::-webkit-slider-runnable-track{height:2px;background:var(--line2)}\n"
"input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;appearance:none;width:16px;height:16px;border-radius:50%;background:var(--rain);margin-top:-7px;border:2px solid var(--paper);box-shadow:0 1px 3px rgba(0,0,0,.2)}\n"
".btn{appearance:none;display:inline-flex;align-items:center;justify-content:center;gap:8px;padding:11px 18px;font-family:var(--sans);font-size:13px;font-weight:500;letter-spacing:.06em;text-transform:uppercase;color:var(--ink);background:#fff;border:1px solid var(--ink2);border-radius:0;cursor:pointer;min-height:42px;transition:background .12s,color .12s,border-color .12s}\n"
".btn:hover{background:var(--ink);color:#fff;border-color:var(--ink)}\n"
".btn:active{transform:translateY(1px)}\n"
".btn.pri{background:var(--ink);color:#fff;border-color:var(--ink)}\n"
".btn.pri:hover{background:var(--rain);border-color:var(--rain)}\n"
".btn.dng{color:var(--rust);border-color:var(--rust)}\n"
".btn.dng:hover{background:var(--rust);color:#fff;border-color:var(--rust)}\n"
".btn.gh{border-color:transparent;background:transparent}\n"
".btn.gh:hover{background:var(--mist2);color:var(--ink);border-color:transparent}\n"
".btn.sm{min-height:34px;padding:7px 13px;font-size:11px}\n"
".btn:disabled{opacity:.5;cursor:not-allowed}\n"
".tw{overflow-x:auto;border:1px solid var(--line)}\n"
"table.t{width:100%;border-collapse:collapse;min-width:640px}\n"
"table.t th,table.t td{text-align:left;padding:11px 14px;border-bottom:1px solid var(--line);font-size:14px}\n"
"table.t th{font-family:var(--sans);font-weight:500;font-size:10px;letter-spacing:.22em;text-transform:uppercase;color:var(--ink3);background:var(--paper2);border-bottom:1px solid var(--ink2)}\n"
"table.t td{font-family:var(--serif)}\n"
"table.t td.n{font-variant-numeric:tabular-nums;font-family:var(--mono);font-size:13px}\n"
"table.t tr:hover td{background:var(--paper2)}\n"
".wifi-list{border:1px solid var(--line);background:#fff;display:none}\n"
".wifi-row{display:flex;align-items:center;gap:14px;padding:13px 16px;border-bottom:1px solid var(--line);cursor:pointer;transition:background .12s}\n"
".wifi-row:last-child{border-bottom:0}.wifi-row:hover{background:var(--paper2)}\n"
".wifi-name{font-family:var(--serif);font-size:16px;flex:1;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}\n"
".wifi-meta{font-size:11px;color:var(--ink3);font-family:var(--mono)}\n"
".pbar{height:6px;background:var(--mist2);position:relative;overflow:hidden}\n"
".pbar>i{display:block;height:100%;width:0%;background:var(--rain);transition:width .4s ease}\n"
".pbar-row{display:flex;align-items:center;gap:12px;font-size:12px;font-family:var(--mono)}\n"
".pbar-row .pbar{flex:1}\n"
".cloud-grid{display:grid;grid-template-columns:1.4fr 1fr;gap:36px;align-items:start}\n"
".qrf{background:#fff;border:1px solid var(--ink);padding:18px;width:fit-content;margin:0 auto}\n"
"#claim-qr{width:170px;height:170px;display:block}\n"
".di-card{background:var(--rain-soft);border-left:3px solid var(--rain);padding:16px 18px;display:flex;align-items:center;gap:14px;flex-wrap:wrap;margin-bottom:18px}\n"
".di-card .lbl-id{font-size:10px;letter-spacing:.22em;text-transform:uppercase;color:var(--rain);margin-bottom:4px}\n"
".di{font-family:var(--mono);font-size:20px;letter-spacing:.04em;color:var(--ink);font-weight:600}\n"
"ol.serif-list{margin:0;padding-left:22px;font-family:var(--serif);font-size:15px;line-height:1.55;color:var(--ink2)}\n"
"ol.serif-list li{margin-bottom:6px;padding-left:4px}\n"
"ol.serif-list li::marker{color:var(--rain);font-style:italic}\n"
"ol.serif-list strong{font-weight:500;color:var(--ink)}\n"
"#toasts{position:fixed;left:50%;bottom:24px;transform:translateX(-50%);z-index:80;pointer-events:none;display:flex;flex-direction:column;gap:8px;align-items:center}\n"
".toast{pointer-events:auto;background:var(--ink);color:var(--paper);padding:13px 20px;font-family:var(--serif);font-style:italic;font-size:14px;border-left:2px solid var(--rain);box-shadow:var(--shadow);animation:tin .25s ease}\n"
".toast.error{border-left-color:var(--rust)}\n"
"@keyframes tin{from{opacity:0;transform:translate(-50%,8px)}to{opacity:1;transform:translate(-50%,0)}}\n"
"@keyframes pulse{0%{opacity:1;transform:scale(1)}50%{opacity:.4;transform:scale(.7)}100%{opacity:1;transform:scale(1)}}\n"
".takeover{position:fixed;inset:0;z-index:60;background:var(--mist);background-image:radial-gradient(ellipse at top,#f6f8fa 0%,var(--mist) 70%);display:none;flex-direction:column;animation:fade .25s ease}\n"
".takeover.is-open{display:flex}\n"
".tk-head{padding:18px var(--pad);border-bottom:1px solid var(--ink);display:flex;align-items:center;gap:14px}\n"
".tk-mast{font-family:var(--serif);font-size:20px;font-weight:500;letter-spacing:-.02em;margin:0}\n"
".tk-mast em{font-style:italic;color:var(--rain)}\n"
".tk-body{flex:1;display:flex;align-items:center;justify-content:center;padding:24px}\n"
".tk-card{width:100%;max-width:440px;text-align:center}\n"
".tk-ring{width:200px;height:200px;margin:0 auto 24px;position:relative}\n"
".tk-ring svg{width:100%;height:100%;transform:rotate(-90deg)}\n"
".tk-ring .trk{stroke:var(--mist2)}\n"
".tk-ring .fl{stroke:var(--rain);transition:stroke-dashoffset 1s linear}\n"
".tk-ring .ce{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center}\n"
".tcc{font-family:var(--serif);font-weight:300;font-size:72px;letter-spacing:-.04em;line-height:1;font-variant-numeric:tabular-nums}\n"
".tcl{font-size:10px;letter-spacing:.22em;text-transform:uppercase;color:var(--ink3);margin-top:4px}\n"
".tkH{font-family:var(--serif);font-weight:500;font-size:24px;letter-spacing:-.02em;margin:0 0 6px}\n"
".tkH em{font-style:italic;color:var(--rain)}\n"
".tks{font-family:var(--serif);font-size:15px;color:var(--ink2);margin:0 0 22px;text-wrap:pretty}\n"
".tkt{font-family:var(--serif);font-style:italic;color:var(--ink3);font-size:13px;margin-top:14px}\n"
".compliance-hint{padding:10px 14px;background:var(--warm-soft);border-left:2px solid var(--warm);font-family:var(--serif);font-style:italic;font-size:13px;color:var(--ink2);margin-top:10px;display:none}\n"
".compliance-hint.on{display:block}\n"
".hide{display:none !important}\n"
"@media(max-width:900px){:root{--pad:18px}.wrap{padding-top:18px;padding-bottom:60px}.banner{grid-template-columns:1fr;padding-bottom:12px;margin-bottom:14px}.banner .l,.banner .r{display:none}.masthead{white-space:normal}.tabs{justify-content:flex-start}.hero{grid-template-columns:1fr;gap:24px;margin-bottom:30px}.insights{grid-template-columns:1fr;gap:18px;padding-top:18px}.tanks{grid-template-columns:1fr;margin-bottom:30px}.tcard{padding:22px 18px}.body-block{grid-template-columns:90px 1fr;gap:14px}.visual{min-height:140px}.visual svg{max-width:90px}.pct{font-size:60px}.status-strip{grid-template-columns:1fr 1fr;gap:16px;padding:18px 0}.cloud-grid{grid-template-columns:1fr;gap:24px}}\n"
"@media(max-width:480px){.meta-grid{grid-template-columns:1fr 1fr}.meta-grid dt:nth-child(3),.meta-grid dd:nth-child(6){display:none}.tcard h3{font-size:22px}.tk-ring{width:170px;height:170px}.tcc{font-size:60px}}\n"
".bn{display:none;position:fixed;left:0;right:0;bottom:0;z-index:30;background:rgba(238,241,244,.97);backdrop-filter:blur(10px);-webkit-backdrop-filter:blur(10px);border-top:1px solid var(--ink);padding:6px 4px calc(6px + env(safe-area-inset-bottom))}\n"
".bn-tabs{display:flex;justify-content:space-around}\n"
".bn button{background:transparent;border:0;flex:1;padding:8px 4px;display:flex;flex-direction:column;align-items:center;gap:3px;font-family:var(--serif);font-size:11px;color:var(--ink3);cursor:pointer;border-top:2px solid transparent;margin-top:-7px;padding-top:9px}\n"
".bn button.active{color:var(--ink);border-top-color:var(--rain)}\n"
"@media(max-width:600px){.tabs-wrap{display:none}.bn{display:block}.wrap{padding-bottom:90px}}\n"
"</style></head><body><div class='wrap'>\n"
"\n"
"<header class='banner'>\n"
"<div class='l'><div class='banner-meta' id='banner-time'>\u2014</div><div class='banner-sub' id='banner-date'></div></div>\n"
"<h1 class='masthead'>Tank<em>Sync</em></h1>\n"
"<div class='r'><div class='banner-meta' id='banner-host'>\u2014</div><div class='banner-sub' id='banner-fw'>\u2014</div><div class='banner-sub' id='banner-cc' style='margin-top:1px'>\u2014</div></div>\n"
"</header>\n"
"\n"
"<nav class='tabs-wrap'><div class='tabs' role='tablist'>\n"
"<button class='tab active' data-tab='tanks' onclick=\"switchTab('tanks',this)\">Tanks</button>\n"
"<button class='tab' data-tab='devices' onclick=\"switchTab('devices',this)\">Devices</button>\n"
"<button class='tab' data-tab='network' onclick=\"switchTab('network',this)\">Network</button>\n"
"<button class='tab' data-tab='system' onclick=\"switchTab('system',this)\">System</button>\n"
"<button class='tab' data-tab='cloud' onclick=\"switchTab('cloud',this)\">Cloud</button>\n"
"</div></nav>\n"
"\n"
"<!-- TANKS -->\n"
"<section id='panel-tanks' class='panel active' data-panel='tanks'>\n"
"<div class='hero'>\n"
"<div>\n"
"<div class='label'>At a glance</div>\n"
"<h2 id='heroH2'>Loading tanks\u2026</h2>\n"
"<p class='lede' id='heroLede'>Reading from the Hub.</p>\n"
"</div>\n"
"<aside class='hero-summary' id='heroSummary'></aside>\n"
"</div>\n"
"<div class='section-head'><h2>Tanks</h2><span class='rule'></span><span class='meta' id='tankMeta'>\u2014</span></div>\n"
"<section class='tanks' id='tanks-grid'></section>\n"
"<section class='insights'>\n"
"<div class='insights-head'>\n"
"<h2>Things worth knowing</h2>\n"
"<div class='by'>\u2014 from your hub, computed locally</div>\n"
"<div class='upd'>Updated <span id='insUpd'>just now</span></div>\n"
"</div>\n"
"<div class='insight-list' id='insightList'></div>\n"
"</section>\n"
"<div class='status-strip' id='statusStrip'></div>\n"
"</section>\n"
"\n"
"<!-- DEVICES -->\n"
"<section id='panel-transmitters' class='panel' data-panel='devices'>\n"
"<div class='hero' style='grid-template-columns:1fr;margin-bottom:30px'>\n"
"<div><div class='label'>Devices</div><h2>Each transmitter, <em>named and calibrated</em>.</h2><p class='lede'>Pair a new sensor, edit its tank name, or trim its calibration. Calibration happens on the Hub, never in the cloud.</p></div>\n"
"</div>\n"
"<div class='ec'>\n"
"<div class='ec-head'><h3>Edit <em>device</em></h3><span style='flex:1'></span><span class='banner-meta' id='tx-addr-display'>\u2014</span><button class='btn sm' onclick='togglePairing()'>+ Pair new</button></div>\n"
"<input type='number' id='tx-addr' class='hide'>\n"
"<div class='ec-body'>\n"
"<div class='field-row c2'>\n"
"<div class='field'><span class='lbl'>Tank name</span><input type='text' id='tx-name' class='serifish' maxlength='24'><span class='help'>Up to twenty-four letters; OLED trims long names to fit.</span></div>\n"
"<div class='field'><span class='lbl'>Capacity</span><div class='is'><input type='number' id='tx-cap'><span class='sf'>litres</span></div><span class='help'>Total volume when full.</span></div>\n"
"</div>\n"
"<div class='field-row c1'>\n"
"<div class='field'><span class='lbl'>Sensor</span><select id='tx-sensor'><option value=''>Keep current</option><option value='sr04'>Ultrasonic \u2014 AJ-SR04M (0.05\u20134\u202fm)</option><option value='ld2413'>mmWave \u2014 HLK-LD2413 (0.15\u201310.5\u202fm) \u00b7 experimental</option></select><span class='help'>Distance-sensor module fitted to this transmitter. Sent on next wake; TX reboots to load the new driver.</span></div>\n"
"</div>\n"
"<div class='field-row c2'>\n"
"<div class='field'><span class='lbl'>Distance \u00b7 when full</span><div class='is'><input type='number' class='mi' id='tx-min'><span class='sf'>cm</span></div><span class='help'>Sensor \u2192 water surface when brimming.</span></div>\n"
"<div class='field'><span class='lbl'>Distance \u00b7 when empty</span><div class='is'><input type='number' class='mi' id='tx-max'><span class='sf'>cm</span></div><span class='help'>Sensor \u2192 bottom (or your minimum line). Range widens to 1050\u202fcm for mmWave.</span></div>\n"
"</div>\n"
"<div class='field-row c3'>\n"
"<div class='field'><span class='lbl'>Sleep interval</span><div class='is'><input type='number' class='mi' id='tx-sleep' min='60' max='86400'><span class='sf'>sec</span></div><span class='help'>60 seconds to a day. Longer = longer battery.</span></div>\n"
"<div class='field'><span class='lbl'>Sensor samples</span><input type='number' class='mi' id='tx-samp' min='3' max='20'><span class='help'>3 to 20 readings, averaged.</span></div>\n"
"<div class='field'><span class='lbl'>LoRa power</span><div class='is'><input type='number' class='mi' id='tx-pwr' min='0' max='22'><span class='sf'>dBm</span></div><span class='help'>0 = TX default; 1\u201322 to override.</span></div>\n"
"</div>\n"
"</div>\n"
"<div class='ec-foot'><span style='flex:1'></span><button class='btn pri' onclick='saveTx()'>Save device</button></div>\n"
"</div>\n"
"<div class='ec'>\n"
"<div class='ec-head'><h3>All <em>registered</em> devices</h3><span class='lede-sm' id='dCount'></span><span style='flex:1'></span><button class='btn dng sm' onclick='clearAllTx()'>Clear all</button></div>\n"
"<div class='tw'><table class='t'><thead><tr><th>Addr</th><th>Name</th><th>Full</th><th>Empty</th><th>Cap</th><th>Sleep</th><th>Samp</th><th>Pwr</th><th>FW</th><th>State</th><th></th></tr></thead><tbody id='tx-tbody'></tbody></table></div>\n"
"</div>\n"
"</section>\n"
"\n"
"<!-- NETWORK -->\n"
"<section id='panel-network' class='panel' data-panel='network'>\n"
"<div class='hero' style='grid-template-columns:1fr;margin-bottom:30px'>\n"
"<div><div class='label'>Network</div><h2>The <em>three signals</em> the Hub keeps open.</h2><p class='lede'>Wi\u2011Fi to your home, MQTT to your home assistant, and LoRa to the small radios in your tanks.</p></div>\n"
"</div>\n"
"<div class='ec'>\n"
"<div class='ec-head'><h3>Wi\u2011<em>Fi</em></h3><span class='lede-sm'>Connect this Hub to the local network.</span><span style='flex:1'></span><span class='state-eye' id='wifi-status'>\u2014</span></div>\n"
"<div class='ec-body'>\n"
"<div class='field-row c2'>\n"
"<div class='field'><span class='lbl'>Network \u00b7 SSID</span><input type='text' id='wifi-ssid' class='serifish'></div>\n"
"<div class='field'><span class='lbl'>Password</span><input type='password' id='wifi-pass'></div>\n"
"</div>\n"
"<div style='display:flex;gap:12px;flex-wrap:wrap;margin:16px 0 10px'><button class='btn sm' onclick='wifiScan()'>Scan networks</button></div>\n"
"<div class='wifi-list' id='scan-list'></div>\n"
"</div>\n"
"<div class='ec-foot'><span class='lede-sm'>Saving will reconnect the Hub.</span><span style='flex:1'></span><button class='btn pri' onclick='wifiConnect()'>Save Wi\u2011Fi</button></div>\n"
"</div>\n"
"<div class='ec'>\n"
"<div class='ec-head'><h3>MQTT <em>broker</em></h3><span class='lede-sm'>Publish readings to Home Assistant or any MQTT broker.</span><span style='flex:1'></span><span class='state-eye' id='mqtt-live-badge'>\u2014</span></div>\n"
"<div class='ec-body'>\n"
"<div class='field-row c2'>\n"
"<div class='field'><span class='lbl'>Host</span><input type='text' id='mqtt-host' class='mi'></div>\n"
"<div class='field'><span class='lbl'>Port</span><input type='number' id='mqtt-port' class='mi' value='1883'></div>\n"
"</div>\n"
"<div class='field-row c2'>\n"
"<div class='field'><span class='lbl'>Username</span><input type='text' id='mqtt-user'></div>\n"
"<div class='field'><span class='lbl'>Password</span><input type='password' id='mqtt-pass' placeholder='(unchanged)'></div>\n"
"</div>\n"
"<div class='field-row c2' style='margin-top:14px'>\n"
"<label class='chk'><input type='checkbox' id='mqtt-en'><div><div class='ct'>Publish readings</div><div class='ck'>Every wake cycle, levels are sent to the broker.</div></div></label>\n"
"<label class='chk'><input type='checkbox' id='mqtt-ha'><div><div class='ct'>Home Assistant discovery</div><div class='ck'>Auto\u2011publish discovery topics so HA finds the Hub.</div></div></label>\n"
"</div>\n"
"</div>\n"
"<div class='ec-foot'><span style='flex:1'></span><button class='btn pri' onclick='saveMqtt()'>Save MQTT</button></div>\n"
"</div>\n"
"<div class='ec'>\n"
"<div class='ec-head'><h3>Lo<em>Ra</em> radio</h3><span class='lede-sm'>Frequency &amp; Hub address.</span><span style='flex:1'></span><span class='state-eye' id='lora-state'>RX active</span></div>\n"
"<div class='ec-body'>\n"
"<div class='field-row c2'>\n"
"<div class='field'><span class='lbl'>Frequency</span><div class='is'><input type='number' id='lora-freq' class='mi'><span class='sf'>Hz</span></div><span class='help'>India 865 \u00b7 EU 868 \u00b7 US 915 megahertz.</span></div>\n"
"<div class='field'><span class='lbl'>Hub address</span><input type='number' id='lora-addr' class='mi' min='1' max='65535'><span class='help'>1\u201365535. Transmitters address the Hub by this number.</span></div>\n"
"</div>\n"
"<div class='compliance-hint' id='lora-compliance'></div>\n"
"</div>\n"
"<div class='ec-foot'><span class='lede-sm'>Changing radio settings may un\u2011pair existing transmitters.</span><span style='flex:1'></span><button class='btn pri' onclick='saveLora()'>Save LoRa</button></div>\n"
"</div>\n"
"</section>\n"
"\n"
"<!-- SYSTEM -->\n"
"<section id='panel-system' class='panel' data-panel='system'>\n"
"<div class='hero' style='grid-template-columns:1fr;margin-bottom:30px'>\n"
"<div><div class='label'>System</div><h2>The <em>quiet keeper</em> \u2014 display, light, firmware.</h2><p class='lede'>Choose what the OLED shows, dim the LED strip, and stage firmware updates over the air.</p></div>\n"
"</div>\n"
"<div class='ec'>\n"
"<div class='ec-head'><h3>OLED <em>screens</em></h3><span class='lede-sm'>Pick which screens auto\u2011rotate on the small display.</span></div>\n"
"<div class='ec-body'>\n"
"<div class='field-row c3'>\n"
"<label class='chk'><input type='checkbox' id='scr-water'><div><div class='ct'>Water</div><div class='ck'>All tanks at a glance.</div></div></label>\n"
"<label class='chk'><input type='checkbox' id='scr-battery'><div><div class='ct'>Health</div><div class='ck'>Battery + signal summary.</div></div></label>\n"
"<label class='chk'><input type='checkbox' id='scr-system'><div><div class='ct'>System info</div><div class='ck'>Wi\u2011Fi, IP, firmware, uptime.</div></div></label>\n"
"</div>\n"
"</div>\n"
"<div class='ec-foot'><span style='flex:1'></span><button class='btn pri' onclick='saveDisplay()'>Save display</button></div>\n"
"</div>\n"
"<div class='ec'>\n"
"<div class='ec-head'><h3>LED <em>strip</em></h3><span class='lede-sm'>An optional accessory \u2014 one LED per tank, colour\u2011coded.</span></div>\n"
"<div class='ec-body'>\n"
"<div id='led-status-row' style='display:flex;align-items:center;gap:8px;margin-bottom:12px'><span class='lbl' style='margin:0'>Strip status</span><span id='led-status-pill' class='state-eye'>Checking\u2026</span><span id='led-status-hint' class='help' style='flex:1;margin:0'></span></div>\n"
"<div class='field-row c2'>\n"
"<div class='field'><span class='lbl'>Strip type</span><select id='led-count'><option value='2'>2 LEDs (default)</option><option value='8'>8 LED strip</option><option value='24'>24 LED ring</option></select></div>\n"
"<div class='field'><span class='lbl'>Brightness \u00b7 <span id='led-bright-val'>\u2014</span></span><input type='range' id='led-bright' min='5' max='255'></div>\n"
"</div>\n"
"<div id='led-tank-colors' style='margin-top:14px'></div>\n"
"<div style='margin-top:14px;padding-top:14px;border-top:1px dashed var(--line)'><div class='lbl' style='margin-bottom:8px'>Tools</div>\n"
"<div style='display:flex;gap:8px;flex-wrap:wrap'>\n"
"<button type='button' class='btn' onclick='identifyHub()'>Identify Hub (blink LED)</button>\n"
"<button type='button' class='btn' onclick='rebootHub()'>Restart Hub</button>\n"
"</div></div>\n"
"</div>\n"
"<div class='ec-foot'><span class='lede-sm'>Brightness and colours apply live. Strip type changes apply instantly \u2014 no reboot needed.</span><span style='flex:1'></span><button class='btn pri' onclick='saveLed()'>Save</button></div>\n"
"</div>\n"
"<div class='ec'>\n"
"<div class='ec-head'><h3>Buzzer <em>alerts</em></h3><span class='lede-sm'>Audible nudges \u2014 low tanks, overflow, sensor errors.</span></div>\n"
"<div class='ec-body'>\n"
"<div style='display:flex;align-items:center;gap:14px;margin-bottom:14px;flex-wrap:wrap'>\n"
"<label class='chk' style='flex:1;min-width:240px'><input type='checkbox' id='bz-master' onchange='bzApplyDisclosure()'><div><div class='ct'>Master enable</div><div class='ck'>Mutes every alert except the boot tone.</div></div></label>\n"
"</div>\n"
"<div id='bz-collapsed-summary' style='display:none;background:var(--paper);border:1px dashed var(--line);padding:10px 12px;font-size:13px;color:var(--ink-soft);line-height:1.55;margin-bottom:6px'>Loading\u2026</div>\n"
"<div id='bz-active-form'>\n"
"<div style='display:flex;align-items:center;gap:14px;margin-bottom:14px;flex-wrap:wrap'>\n"
"<div class='field' style='max-width:260px'><span class='lbl'>Volume profile</span><select id='bz-profile' style='width:auto;min-width:140px'><option value='0'>Quiet</option><option value='1' selected>Standard</option><option value='2'>Loud</option></select></div>\n"
"</div>\n"
"<div style='display:flex;gap:14px;align-items:center;margin-bottom:14px;flex-wrap:wrap'>\n"
"<label class='chk' style='flex:1;min-width:280px'><input type='checkbox' id='bz-crit-override'><div><div class='ct'>Critical overrides quiet hours</div><div class='ck'>Critical-low (&lt;5%) still beeps after 22:00.</div></div></label>\n"
"<div class='field' style='max-width:170px'><span class='lbl'>Quiet from</span><select id='bz-qstart' style='width:120px;font-family:var(--mono);font-size:14px'></select></div>\n"
"<div class='field' style='max-width:170px'><span class='lbl'>Quiet to</span><select id='bz-qend' style='width:120px;font-family:var(--mono);font-size:14px'></select></div>\n"
"</div>\n"
"<div class='help' style='margin-bottom:12px'>Local time uses the hub's timezone (see below). Set both quiet hours to <code>0</code> to disable.</div>\n"
"<div style='overflow-x:auto;margin-top:10px'><table class='t' id='bz-alert-tbl'><thead><tr><th>Alert</th><th>On</th><th>Test</th></tr></thead><tbody id='bz-alert-rows'><tr><td colspan='3' class='help'>Loading\u2026</td></tr></tbody></table></div>\n"
"<details id='bz-advanced' style='margin-top:14px;border-top:1px solid var(--line);padding-top:12px'>\n"
"<summary style='cursor:pointer;font-family:var(--serif);font-size:14px;color:var(--ink-soft)'>Advanced alerts <span class='help' id='bz-advanced-count' style='margin-left:6px'></span></summary>\n"
"<div style='overflow-x:auto;margin-top:10px'><table class='t'><thead><tr><th>Alert</th><th>On</th><th>Test</th></tr></thead><tbody id='bz-advanced-rows'></tbody></table></div>\n"
"</details>\n"
"</div>\n"
"</div>\n"
"<div class='ec-foot'><span class='lede-sm'>Boot tone always plays \u2014 confirms the Hub is powered + initialized.</span><span style='flex:1'></span><button class='btn pri' onclick='saveBuzzer()'>Save buzzer</button></div>\n"
"</div>\n"
"<div class='ec'>\n"
"<div class='ec-head'><h3>Transmitter <em>firmware</em></h3><span class='lede-sm'>Update each tank sensor over Wi-Fi.</span></div>\n"
"<div class='ec-body'>\n"
"<!-- Block 1: per-TX status list -->\n"
"<div style='margin-bottom:18px'>\n"
"<div style='display:flex;align-items:center;gap:10px;margin-bottom:10px'><span class='lbl' style='margin:0'>Paired transmitters</span><span style='flex:1'></span><button id='tx-fw-refresh-btn' class='btn sm' onclick='loadTxFirmware()'>Refresh status</button></div>\n"
"<div id='tx-fw-list' class='help'>Loading…</div>\n"
"</div>\n"
"<!-- Block 2: how to update via Wi-Fi (collapsed by default) -->\n"
"<details style='border:1px solid var(--line);background:var(--paper);padding:12px 14px;margin-bottom:14px' id='tx-howto-details'>\n"
"<summary style='cursor:pointer;font-family:var(--serif);font-size:14px;font-weight:600'>How to update a transmitter (Wi-Fi)</summary>\n"
"<ol style='margin-top:12px;padding-left:22px;font-family:var(--serif);font-size:14px;line-height:1.7'>\n"
"<li>Download the latest TX firmware from the <a href='https://github.com/Techposts/TankSync/releases?q=tx-v' target='_blank' rel='noopener'>releases page</a> — pick the file named <code>tanksync-transmitter-tx-v…bin</code>.</li>\n"
"<li>On the transmitter, hold the <strong>BOOT</strong> button for ~5 seconds until the status LED turns blue (AP mode active).</li>\n"
"<li>On your phone or laptop, join the Wi-Fi network <strong>TankSync-TX-XXXX</strong> (no password) — XXXX is unique per device.</li>\n"
"<li>Open <a href='http://192.168.4.1' target='_blank' rel='noopener'>http://192.168.4.1</a> in your browser. The TX setup page loads.</li>\n"
"<li>Pick the .bin you downloaded → <strong>Upload firmware</strong>. Wait ~30 seconds for the flash + reboot.</li>\n"
"<li>Reconnect to your home Wi-Fi. The transmitter rejoins automatically and resumes sending tank readings.</li>\n"
"</ol>\n"
"<p class='help' style='margin-top:12px'>The transmitter pauses LoRa transmissions while in AP mode. Tank readings resume after reboot.</p>\n"
"</details>\n"
"<!-- Block 3: advanced staged-binary (kept for future LoRa-OTA, paused now) -->\n"
"<details style='border:1px dashed var(--line);background:transparent;padding:12px 14px'>\n"
"<summary style='cursor:pointer;font-family:var(--serif);font-size:13px;color:var(--ink-soft)'>Advanced · stage TX binary for LoRa-OTA (paused)</summary>\n"
"<p class='help' style='margin-top:10px'>LoRa-OTA delivery is paused in v2.5.x — packet reliability needs more work. The staging UI below is kept so the work is here when LoRa-OTA returns. The Wi-Fi method above is the supported path today.</p>\n"
"<div style='margin-top:12px'>\n"
"<div class='lbl' style='margin-bottom:8px'>Stage a TX binary</div>\n"
"<div style='display:flex;gap:10px;flex-wrap:wrap;align-items:center'><input type='file' id='tx-bin-file' accept='.bin' style='font-family:var(--serif);max-width:340px'><button class='btn sm' id='tx-upload-btn' onclick='uploadTx()'>Stage binary</button><button class='btn dng sm hide' id='tx-clear-btn' onclick='clearStaged()'>Clear staged</button></div>\n"
"<div id='tx-upload-progress' class='hide' style='margin-top:12px'><div class='pbar'><i id='tx-upload-bar'></i></div><div id='tx-upload-label' class='help' style='margin-top:4px'>0%</div></div>\n"
"<div id='tx-staged-info' class='help' style='margin-top:6px'></div>\n"
"</div>\n"
"</details>\n"
"</div>\n"
"</div>\n"
"<div class='ec'>\n"
"<div class='ec-head'><h3>Receiver <em>firmware</em></h3><span class='lede-sm'>Update the Hub itself.</span><span style='flex:1'></span><span class='banner-meta' id='rx-ver'>\u2014</span></div>\n"
"<div class='ec-body'>\n"
"<div id='ota-status' class='help' style='margin-bottom:6px'>Loading…</div>\n"
"<div id='ota-version-line' style='font-size:14px;color:var(--ink);margin-bottom:10px;display:none'></div>\n"
"<div id='ota-progress' style='display:none;margin-bottom:10px'><div class='pbar'><i id='ota-bar'></i></div><div id='ota-progress-label' class='help' style='margin-top:4px'>0%</div></div>\n"
"<div style='display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin-bottom:14px'><button id='ota-action-btn' class='btn' onclick='otaAction()'>Check for updates</button></div>\n"
"<div class='lbl' style='margin-bottom:8px'>Or upload a .bin manually</div>\n"
"<div style='display:flex;gap:10px;flex-wrap:wrap;align-items:center'><input type='file' id='bin-file' accept='.bin' style='font-family:var(--serif);max-width:340px'><button class='btn pri' onclick='uploadBin()'>Upload &amp; flash</button></div>\n"
"<div id='spiffs-info' class='help' style='margin-top:8px'></div>\n"
"</div>\n"
"</div>\n"
"<div class='ec'>\n"
"<div class='ec-head'><h3>Time <em>zone</em></h3><span class='lede-sm'>Used by quiet hours, the OLED clock, and history bucket alignment.</span></div>\n"
"<div class='ec-body'>\n"
"<div style='display:flex;gap:14px;flex-wrap:wrap;align-items:end;margin-bottom:10px'>\n"
"<div class='field' style='flex:2;min-width:240px'><span class='lbl'>POSIX TZ string</span><input type='text' id='tz-string' placeholder='e.g. IST-5:30' style='font-family:var(--mono);font-size:13px'></div>\n"
"<div class='field' style='flex:1;min-width:180px'><span class='lbl'>Auto-detected</span><div id='tz-suggested' class='help' style='margin:0'>—</div></div>\n"
"</div>\n"
"<div class='help' id='tz-status' style='margin-bottom:8px'>—</div>\n"
"<div style='display:flex;gap:8px;flex-wrap:wrap'><button class='btn' onclick='tzUseSuggested()'>Use auto-detected</button><button class='btn' onclick='tzClear()'>Reset to default</button><span style='flex:1'></span><button class='btn pri' onclick='saveTz()'>Apply</button></div>\n"
"</div>\n"
"</div>\n"
"</section>\n"
"\n"
"<!-- CLOUD -->\n"
"<section id='panel-claim' class='panel' data-panel='cloud'>\n"
"<div class='hero' style='grid-template-columns:1fr;margin-bottom:30px'>\n"
"<div><div class='label'>Cloud</div><h2>The <em>cloud</em>, if you want it.</h2><p class='lede'>Link this Hub to your TankSync account so you can see tanks from anywhere \u2014 and get a note when the borewell runs low.</p></div>\n"
"</div>\n"
"<div class='ec'>\n"
"<div class='ec-head'><h3>Cloud <em>connection</em></h3><span class='lede-sm' id='cloud-sub'>Not yet linked.</span><span style='flex:1'></span><span class='state-eye' id='claim-status-text'>\u2014</span></div>\n"
"<div class='ec-body'>\n"
"<div class='cloud-grid'>\n"
"<div>\n"
"<div class='di-card' id='claim-status-box'><div style='flex:1;min-width:0'><div class='lbl-id'>Device ID</div><div class='di' id='claim-device-id'>\u2014</div></div><button class='btn sm' onclick=\"claimCopy(document.getElementById('claim-device-id').textContent)\">Copy</button></div>\n"
"<div class='lbl' style='margin-bottom:8px'>How to claim</div>\n"
"<ol class='serif-list'>\n"
"<li>Open the <strong>TankSync</strong> app on your phone, or scan the QR.</li>\n"
"<li>Sign in or create an account.</li>\n"
"<li>Tap <strong>Add a Hub</strong> and enter the Device ID, or scan it.</li>\n"
"</ol>\n"
"<div class='help' style='margin-top:10px'>Setup URL \u00b7 <span id='claim-setup-url' class='mono' style='font-family:var(--mono);font-size:13px;word-break:break-all'>\u2014</span></div>\n"
"<div id='claim-setup-url-box' class='hide'></div>\n"
"<div class='hide' id='claim-setup-url-title'></div>\n"
"<div class='hide' id='claim-setup-url-hint'></div>\n"
"<div style='margin-top:14px;display:flex;gap:10px;flex-wrap:wrap'>\n"
"<button class='btn sm' onclick=\"claimCopy(document.getElementById('claim-setup-url').textContent)\">Copy URL</button>\n"
"<button class='btn sm' onclick=\"window.open(document.getElementById('claim-setup-url').textContent,'_blank')\">Open</button>\n"
"</div>\n"
"<div id='claim-unlink-box' style='margin-top:18px;padding-top:14px;border-top:1px dotted var(--line)'>\n"
"<div class='lbl' style='margin-bottom:6px'>Unlink from cloud</div>\n"
"<button class='btn dng sm' onclick='claimUnlink()'>Unlink</button>\n"
"<div class='help' style='margin-top:6px'>Clears MQTT broker credentials. The receiver stays on Wi\u2011Fi but stops publishing to the cloud.</div>\n"
"</div>\n"
"</div>\n"
"<div><div class='qrf'><img id='claim-qr' alt='QR code'></div><div class='help' style='text-align:center;margin-top:8px'>\u2014 scan with the TankSync app</div></div>\n"
"</div>\n"
"</div>\n"
"</div>\n"
"</section>\n"
"\n"
"</div>\n"
"\n"
"<div id='toasts' aria-live='polite'></div>\n"
"\n"
"<div class='takeover' id='pairing-overlay' role='dialog' aria-modal='true'>\n"
"<div class='tk-head'><h2 class='tk-mast'>Pair a <em>transmitter</em></h2><span style='flex:1'></span><button class='btn gh sm' onclick='closePairing()'>Close</button></div>\n"
"<div class='tk-body'><div class='tk-card'>\n"
"<div class='tk-ring' id='pair-icon'><svg viewBox='0 0 220 220'><circle class='trk' cx='110' cy='110' r='96' fill='none' stroke-width='8'/><circle class='fl' id='pair-fill' cx='110' cy='110' r='96' fill='none' stroke-width='8' stroke-linecap='round' stroke-dasharray='603.19' stroke-dashoffset='0'/></svg><div class='ce'><div class='tcc' id='pair-timer'>60</div><div class='tcl'>seconds</div></div></div>\n"
"<h3 id='pair-title' class='tkH'>Listening for a <em>new</em> transmitter\u2026</h3>\n"
"<p id='pair-msg' class='tks'>Hold the pair button on your transmitter for <strong>two seconds</strong>. The Hub will auto\u2011claim the first signal it hears.</p>\n"
"<button id='pair-cancel' class='btn dng' onclick='closePairing()'>Cancel pairing</button>\n"
"<div id='pair-sub' class='tkt'>The timer is server\u2011side; your phone may sleep without interrupting.</div>\n"
"</div></div></div>\n"
"\n"
"<nav class='bn'><div class='bn-tabs'>\n"
"<button class='active' data-tab='tanks' onclick=\"switchTab('tanks',this)\"><svg width='18' height='18' viewBox='0 0 16 16' fill='none'><rect x='3' y='2' width='10' height='12' rx='2' stroke='currentColor' stroke-width='1.4'/><path d='M3.5 9 Q5 8 6.5 9 T9.5 9 T12.5 9 V13.5 H3.5Z' fill='currentColor' opacity='.5'/></svg>Tanks</button>\n"
"<button data-tab='devices' onclick=\"switchTab('devices',this)\"><svg width='18' height='18' viewBox='0 0 16 16' fill='none'><rect x='2' y='3' width='12' height='10' rx='2' stroke='currentColor' stroke-width='1.4'/><circle cx='5' cy='8' r='1' fill='currentColor'/><path d='M8 6h4M8 8h4M8 10h3' stroke='currentColor' stroke-width='1.2' stroke-linecap='round'/></svg>Devices</button>\n"
"<button data-tab='network' onclick=\"switchTab('network',this)\"><svg width='18' height='18' viewBox='0 0 16 16' fill='none'><path d='M2.5 7c3-3 8-3 11 0M4.5 9.5c2-2 5-2 7 0M8 12.2v.01' stroke='currentColor' stroke-width='1.4' stroke-linecap='round'/></svg>Network</button>\n"
"<button data-tab='system' onclick=\"switchTab('system',this)\"><svg width='18' height='18' viewBox='0 0 16 16' fill='none'><circle cx='8' cy='8' r='2.2' stroke='currentColor' stroke-width='1.4'/><path d='M8 1.5v2M8 12.5v2M14.5 8h-2M3.5 8h-2' stroke='currentColor' stroke-width='1.3' stroke-linecap='round'/></svg>System</button>\n"
"<button data-tab='cloud' onclick=\"switchTab('cloud',this)\"><svg width='18' height='18' viewBox='0 0 16 16' fill='none'><path d='M5 11.5h6.5a3 3 0 0 0 .3-5.97A4 4 0 0 0 4.2 6.6 2.7 2.7 0 0 0 5 11.5Z' stroke='currentColor' stroke-width='1.4'/></svg>Cloud</button>\n"
"</div></nav>\n"
"\n"
"<script>\n"
"let pairPoll=null;\n"
"const $=(s,r=document)=>r.querySelector(s);const $$=(s,r=document)=>Array.from(r.querySelectorAll(s));\n"
"function showToast(m,ok=true){const t=document.createElement('div');t.className='toast'+(ok?'':' error');t.textContent='\u2014 '+m;document.getElementById('toasts').appendChild(t);setTimeout(()=>{t.style.transition='opacity .3s';t.style.opacity=0;setTimeout(()=>t.remove(),300)},2400)}\n"
"async function api(p,o){const r=await fetch(p,o);return r.json()}\n"
"function fmtL(n){return Number(n||0).toLocaleString()}\n"
"function lastSeenStr(s){if(s==null||s<0)return '\u2014';if(s<60)return s+'s ago';if(s<3600)return Math.round(s/60)+'m ago';return Math.round(s/3600)+'h ago'}\n"
"function numToWord(n){return ['Zero','One','Two','Three','Four','Five','Six','Seven','Eight','Nine','Ten'][n]||String(n)}\n"
"function escapeHTML(s){return String(s).replace(/[&<>\"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','\"':'&quot;',\"'\":'&#39;'}[c]))}\n"
"function gpToggle(){}\n"
"function switchTab(t,el){\n"
"$$('.tab,.bn button').forEach(b=>b.classList.toggle('active',b.dataset.tab===t));\n"
"$$('.panel').forEach(p=>p.classList.remove('active'));\n"
"const map={tanks:'panel-tanks',devices:'panel-transmitters',network:'panel-network',system:'panel-system',cloud:'panel-claim'};\n"
"const p=document.getElementById(map[t]);if(p)p.classList.add('active');\n"
"if(t==='devices')loadTransmitters();\n"
"if(t==='network'){loadWifi();loadMqtt();loadLora();checkLoraCompliance();}\n"
"if(t==='system'){loadDisplay();loadLed();loadOta();loadBuzzer();loadTz();}\n"
"if(t==='cloud')loadClaim();\n"
"window.scrollTo({top:0,behavior:'smooth'});\n"
"}\n"
"function tankSvg(pct,state){\n"
"const unknown=state==='offline'||state==='waiting'||pct==null;\n"
"const safe=unknown?0:Math.max(0,Math.min(100,pct));\n"
"const fillTop=12+166*(1-safe/100);\n"
"const id='g'+Math.random().toString(36).slice(2,7);\n"
"return `<svg viewBox='0 0 140 200' preserveAspectRatio='xMidYMid meet' aria-label='Level ${pct}%'>\n"
"<defs><linearGradient id='${id}' x1='0' x2='0' y1='0' y2='1'><stop offset='0' stop-color='#7eb1d6'/><stop offset='0.45' stop-color='#4a83b0'/><stop offset='1' stop-color='#1f3d5e'/></linearGradient><clipPath id='c${id}'><path d='M28 18 Q28 12 34 12 H106 Q112 12 112 18 V178 Q112 184 106 184 H34 Q28 184 28 178 Z'/></clipPath></defs>\n"
"<ellipse cx='70' cy='190' rx='46' ry='6' fill='rgba(31,45,94,0.12)'/>\n"
"<path d='M28 18 Q28 12 34 12 H106 Q112 12 112 18 V178 Q112 184 106 184 H34 Q28 184 28 178 Z' fill='#fff' stroke='#1f2d5e' stroke-width='0.8'/>\n"
"${unknown?`<text x='70' y='105' text-anchor='middle' font-family='Iowan Old Style,Georgia,serif' font-style='italic' font-size='22' fill='#9aa5b3'>\u2014 \u2014</text>`\n"
":`<g clip-path='url(#c${id})'><rect x='20' y='${fillTop}' width='100' height='200' fill='url(#${id})'/><g><animateTransform attributeName='transform' type='translate' from='0 0' to='-26 0' dur='5s' repeatCount='indefinite'/><path d='M-12 ${fillTop} q12 -3 24 0 t24 0 t24 0 t24 0 t24 0 t24 0 v6 h-150z' fill='rgba(255,255,255,0.28)'/></g></g>`}\n"
"<g stroke='#1f2d5e' stroke-width='0.5' opacity='0.35'><line x1='22' y1='${12+166*0.25}' x2='28' y2='${12+166*0.25}'/><line x1='22' y1='${12+166*0.5}' x2='28' y2='${12+166*0.5}'/><line x1='22' y1='${12+166*0.75}' x2='28' y2='${12+166*0.75}'/></g>\n"
"</svg>`}\n"
"function ico(k){const s={alert:'<svg width=14 height=14 viewBox=\"0 0 16 16\" fill=\"none\"><path d=\"M8 1L15 14H1L8 1Z\" stroke=\"currentColor\" stroke-width=\"1.4\" stroke-linejoin=\"round\"/><path d=\"M8 6v4M8 12v.5\" stroke=\"currentColor\" stroke-width=\"1.4\" stroke-linecap=\"round\"/></svg>',clock:'<svg width=14 height=14 viewBox=\"0 0 16 16\" fill=\"none\"><circle cx=\"8\" cy=\"8\" r=\"6.5\" stroke=\"currentColor\" stroke-width=\"1.4\"/><path d=\"M8 4.5V8l2.5 1.5\" stroke=\"currentColor\" stroke-width=\"1.4\" stroke-linecap=\"round\"/></svg>',wave:'<svg width=14 height=14 viewBox=\"0 0 16 16\" fill=\"none\"><path d=\"M1 8c2-2 4-2 6 0s4 2 6 0 2 0 2 0M1 11c2-2 4-2 6 0s4 2 6 0 2 0 2 0\" stroke=\"currentColor\" stroke-width=\"1.4\" stroke-linecap=\"round\"/></svg>',warn:'<svg width=14 height=14 viewBox=\"0 0 16 16\" fill=\"none\"><circle cx=\"8\" cy=\"8\" r=\"6.5\" stroke=\"currentColor\" stroke-width=\"1.4\"/><path d=\"M8 5v3.5M8 11v.5\" stroke=\"currentColor\" stroke-width=\"1.4\" stroke-linecap=\"round\"/></svg>',down:'<svg width=14 height=14 viewBox=\"0 0 16 16\" fill=\"none\"><path d=\"M8 3v9m0 0L4 8m4 4l4-4\" stroke=\"currentColor\" stroke-width=\"1.4\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/></svg>',up:'<svg width=14 height=14 viewBox=\"0 0 16 16\" fill=\"none\"><path d=\"M8 13V4m0 0L4 8m4-4l4 4\" stroke=\"currentColor\" stroke-width=\"1.4\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/></svg>',drop:'<svg width=14 height=14 viewBox=\"0 0 16 16\" fill=\"none\"><path d=\"M8 1.5C8 1.5 3 6.5 3 10a5 5 0 0 0 10 0c0-3.5-5-8.5-5-8.5z\" stroke=\"currentColor\" stroke-width=\"1.4\" stroke-linejoin=\"round\"/></svg>',rain:'<svg width=14 height=14 viewBox=\"0 0 16 16\" fill=\"none\"><path d=\"M3 9c0-2 2-4 5-4s5 2 5 4M5 12v2M8 12v2M11 12v2\" stroke=\"currentColor\" stroke-width=\"1.4\" stroke-linecap=\"round\"/></svg>'};return s[k]||s.drop}\n"
"const pwHistory={};\n"
"function pushHistory(addr,pct){if(!pwHistory[addr])pwHistory[addr]=[];const a=pwHistory[addr];a.push({t:Date.now(),p:pct});if(a.length>60)a.shift()}\n"
"const tankHist={};\n"
"let tankHistFetchedAt=0;\n"
"async function refreshTankHist(force){\n"
"if(!force&&tankHistFetchedAt&&Date.now()-tankHistFetchedAt<600000)return;\n"
"try{const r=await api('/api/tanks/history');tankHistFetchedAt=Date.now();(r.tanks||[]).forEach(h=>{tankHist[h.addr]={pct:h.pct||[],volt:h.volt||[],head_t:h.head_t||0,slot_seconds:r.slot_seconds||1800,samples:h.samples||0}})}catch(e){}\n"
"}\n"
"function fitTrend(arr,slot_seconds){\n"
"const pts=[];arr.forEach((p,i)=>{if(p!=null)pts.push({i,p})});if(pts.length<3)return{valid:false};\n"
"const n=pts.length;let sx=0,sy=0,sxx=0,sxy=0,syy=0;\n"
"pts.forEach(b=>{sx+=b.i;sy+=b.p;sxx+=b.i*b.i;sxy+=b.i*b.p;syy+=b.p*b.p});\n"
"const den=n*sxx-sx*sx;if(den===0)return{valid:false};\n"
"const slopePerSlot=(n*sxy-sx*sy)/den;const intercept=(sy-slopePerSlot*sx)/n;\n"
"let ssRes=0;pts.forEach(pt=>{const pred=intercept+slopePerSlot*pt.i;ssRes+=(pt.p-pred)*(pt.p-pred)});\n"
"const meanY=sy/n;const ssTot=syy-n*meanY*meanY;\n"
"const r2=ssTot>0?1-ssRes/ssTot:0;\n"
"return{valid:true,slopePerHour:slopePerSlot*(3600/slot_seconds),r2,n};\n"
"}\n"
"function hasFillEvent(arr){for(let i=1;i<arr.length;i++){if(arr[i]==null||arr[i-1]==null)continue;if(arr[i]-arr[i-1]>=5)return true}return false}\n"
"function predictForTank(t){\n"
"const addr=t.address||t.addr;\n"
"if(t.state==='offline'||t.state==='lost')return `<div class='predict'>${ico('alert')}<span>Last reading lost <strong>${lastSeenStr(t.last_seen_s)}</strong> \u2014 likely a flat battery or out\u2011of\u2011range radio.</span></div>`;\n"
"if(t.state==='waiting')return `<div class='predict'>${ico('clock')}<span>Waiting for first reading. Calibrate distance\u2011full and distance\u2011empty in <em>Devices</em>.</span></div>`;\n"
"const hh=tankHist[addr];\n"
"if(hh&&hh.samples>=6){\n"
"const fit=fitTrend(hh.pct,hh.slot_seconds||1800);const fill=hasFillEvent(hh.pct);\n"
"if(fit.valid&&!fill&&fit.r2>0.6&&Math.abs(fit.slopePerHour)>0.5){\n"
"const slope=fit.slopePerHour;\n"
"if(slope<0){\n"
"const cur=t.water_pct;const hrs=cur!=null?(cur-5)/-slope:0;\n"
"let eta='';\n"
"if(hrs>=1&&hrs<24)eta=` \u2014 reserve in roughly <strong>${Math.round(hrs)}h</strong>`;\n"
"else if(hrs>=24&&hrs<96){const dy=Math.round(hrs/24);eta=` \u2014 reserve in <strong>${dy} day${dy===1?'':'s'}</strong>`;}\n"
"return `<div class='predict'>${ico('down')}<span>Drawing down at <strong>${Math.abs(slope).toFixed(1)}%/hr</strong>${eta}.</span></div>`;\n"
"}\n"
"return `<div class='predict'>${ico('up')}<span>Filling now \u2014 <strong>${slope.toFixed(1)}%/hr</strong>.</span></div>`;\n"
"}\n"
"if(fit.valid&&fill)return `<div class='predict'>${ico('wave')}<span>Pump activity in the last day \u2014 pattern not steady, prediction held.</span></div>`;\n"
"if(fit.valid&&Math.abs(fit.slopePerHour)<=0.5)return `<div class='predict'>${ico('wave')}<span>Steady draw today \u2014 level held within \u00b11%.</span></div>`;\n"
"}\n"
"const h=pwHistory[addr];\n"
"if(!h||h.length<3)return `<div class='predict'>${ico('wave')}<span>Learning your usage pattern \u2014 predictions firm up after a few hours.</span></div>`;\n"
"const dt=(h[h.length-1].t-h[0].t)/3600000;\n"
"if(dt<0.05)return `<div class='predict'>${ico('wave')}<span>Steady \u2014 recent readings are within noise.</span></div>`;\n"
"const slope=(h[h.length-1].p-h[0].p)/dt;\n"
"if(Math.abs(slope)<0.5)return `<div class='predict'>${ico('wave')}<span>Steady draw today \u2014 level held within \u00b11%.</span></div>`;\n"
"if(slope<0)return `<div class='predict'>${ico('down')}<span>Drawing down at <strong>${Math.abs(slope).toFixed(1)}%/hr</strong>${(t.water_pct!=null&&t.water_pct>0&&t.water_pct<25&&t.state!=='waiting')?' \u2014 running low':''}.</span></div>`;\n"
"return `<div class='predict'>${ico('up')}<span>Filling now \u2014 <strong>${slope.toFixed(1)}%/hr</strong>.</span></div>`;\n"
"}\n"
"function tankSparkline(addr){\n"
"const hh=tankHist[addr];if(!hh||hh.samples<3)return '';\n"
"const w=240,h=26,pad=2;const arr=hh.pct;\n"
"const segs=[];let cur=[];\n"
"arr.forEach((p,i)=>{if(p==null){if(cur.length>1)segs.push(cur);cur=[]}else{const x=pad+i*(w-2*pad)/(arr.length-1);const y=pad+(100-p)*(h-2*pad)/100;cur.push(`${x.toFixed(1)},${y.toFixed(1)}`)}});\n"
"if(cur.length>1)segs.push(cur);\n"
"if(!segs.length)return '';\n"
"return `<svg class='spark' viewBox='0 0 ${w} ${h}' preserveAspectRatio='none' aria-label='24h trend'>${segs.map(s=>`<polyline points='${s.join(' ')}' fill='none' stroke='currentColor' stroke-width='1.4' stroke-linejoin='round' stroke-linecap='round'/>`).join('')}</svg>`;\n"
"}\n"
"function tankPowerPill(t){\n"
"const pct=t.battery_pct,v=t.battery_v,mode=t.power_mode,cur=t.current_ma;\n"
"if((pct==null||pct<=0)&&(v==null||v<=0))return '';\n"
"// State derived from a 1-hour rolling pct window (last 2 slots \u00d7 30 min). The\n"
"// old code used last-5-voltage-samples which on 30-min slots was 2.5h \u2014 wide\n"
"// enough to straddle a charging transition and net-out to ~0. 1-hour window\n"
"// matches PWA TankDetail logic so both surfaces agree. Also adds a '24h net'\n"
"// chip \u2014 the headline 'overall charged today' question users actually have.\n"
"let stateCls='dis',stateLabel='discharging';\n"
"const hh=tankHist[t.address];\n"
"let hourDelta=null,netDelta=null,netStart=null,netEnd=null;\n"
"if(hh&&Array.isArray(hh.pct)){\n"
"  const ps=hh.pct;\n"
"  // Find last + last-1h-ago valid slot indices.\n"
"  let lastIdx=-1;for(let i=ps.length-1;i>=0;i--){if(ps[i]!=null&&ps[i]>0){lastIdx=i;break}}\n"
"  if(lastIdx>=2&&ps[lastIdx-2]!=null&&ps[lastIdx-2]>0){hourDelta=ps[lastIdx]-ps[lastIdx-2];}\n"
"  // 24h-net = oldest valid slot vs newest valid slot (no local-TZ in hub firmware).\n"
"  let firstIdx=-1;for(let i=0;i<ps.length;i++){if(ps[i]!=null&&ps[i]>0){firstIdx=i;break}}\n"
"  if(firstIdx>=0&&lastIdx>firstIdx){netStart=ps[firstIdx];netEnd=ps[lastIdx];netDelta=netEnd-netStart;}\n"
"}\n"
"let trendKnown=false;\n"
"if(hourDelta!=null){\n"
"  trendKnown=true;\n"
"  if(Math.abs(hourDelta)<2){stateCls='hold';stateLabel='holding';}\n"
"  else if(hourDelta>0){stateCls='chg';stateLabel='charging';}\n"
"  else{stateCls='dis';stateLabel='discharging';}\n"
"}\n"
"if(!trendKnown){\n"
"  // No 1h history yet \u2014 fall back to instantaneous so user gets *some* signal.\n"
"  if(t.charging===true){stateCls='chg';stateLabel='charging';}\n"
"  else if(cur==null||cur===0){stateCls='hold';stateLabel='resting';}\n"
"}\n"
"if(pct!=null&&pct>0&&pct<=15){stateCls='alert';stateLabel='battery low';}\n"
"else if(pct!=null&&pct>0&&pct<=30){stateCls='warn';stateLabel='battery getting low';}\n"
"const parts=[];\n"
"parts.push(`<span class='pwr-pct'>${(pct!=null&&pct>0)?pct+'%':'\u2014'}</span>`);\n"
"if(v!=null&&v>0)parts.push(`<span class='pwr-num'>${v.toFixed(2)} V</span>`);\n"
"if(mode==='ina219'&&cur!=null&&cur!==0)parts.push(`<span class='pwr-num'>${Math.abs(cur)} mA</span>`);\n"
"if(mode==='ina219')parts.push(`<span class='pwr-mode'>via INA219</span>`);\n"
"// 24h-net chip: only shown if delta exceeds the \u00b12pp noise threshold; reuses the\n"
"// pre-existing .pwr-trend.up/.down CSS classes for color.\n"
"let netChip='';\n"
"if(netDelta!=null&&Math.abs(netDelta)>=2){\n"
"  const ar=netDelta>0?'\u2197':'\u2198';const sg=netDelta>0?'+':'';\n"
"  netChip=`<span class='pwr-trend ${netDelta>0?'up':'down'}' title='oldest reading in history: ${netStart}% \u2192 latest: ${netEnd}%'>24h ${ar} ${sg}${netDelta.toFixed(0)}%</span>`;\n"
"}\n"
"const body=parts.map((p,i)=>i===0?p:`<span class='pwr-sep'>\u00b7</span>${p}`).join('');\n"
"return `<div class='pwr'><span class='pwr-state ${stateCls}'><span class='dt'></span>${stateLabel}</span>${body}${netChip?` <span class='pwr-sep'>\u00b7</span>${netChip}`:''}</div>`;\n"
"}\n"
"function yesterdayDraw(addr,capacity){\n"
"const hh=tankHist[addr];if(!hh||hh.samples<10)return null;\n"
"let pctDrop=0;for(let i=1;i<hh.pct.length;i++){const a=hh.pct[i-1],b=hh.pct[i];if(a==null||b==null)continue;const d=b-a;if(d<0)pctDrop+=-d}\n"
"if(pctDrop<2)return null;\n"
"const liters=capacity?Math.round(pctDrop*capacity/100):null;\n"
"return{pctDrop,liters};\n"
"}\n"
"function solarHealth(addr){\n"
"const hh=tankHist[addr];if(!hh||!hh.volt||hh.samples<6)return null;\n"
"const arr=hh.volt.filter(v=>v!=null&&v>0);if(arr.length<4)return null;\n"
"const peak=Math.max(...arr);\n"
"const slot_seconds=hh.slot_seconds||1800;\n"
"let topIdx=-1;for(let i=hh.volt.length-1;i>=0;i--){if(hh.volt[i]!=null&&hh.volt[i]>=4.0){topIdx=i;break}}\n"
"if(topIdx<0&&peak<3.7)return{kind:'alert',msg:'no_charge_low',peak};\n"
"if(topIdx<0)return{kind:'warn',msg:'no_full_charge',peak};\n"
"const hours_ago=Math.round(((hh.volt.length-1-topIdx)*slot_seconds)/3600);\n"
"if(hours_ago<=2)return{kind:'good',msg:'full_recent',peak,hours_ago};\n"
"return{kind:'info',msg:'full_old',peak,hours_ago};\n"
"}\n"
"async function loadTanks(){\n"
"try{\n"
"refreshTankHist(false);\n"
"const d=await api('/api/data');const sys=await api('/api/system');\n"
"const txd=await api('/api/transmitters').catch(()=>({transmitters:[]}));\n"
"const txMap={};(txd.transmitters||[]).forEach(t=>{txMap[t.address]=t});\n"
"const date=new Date();const dStr=date.toLocaleDateString('en-IN',{day:'numeric',month:'short',year:'numeric'}).toUpperCase();\n"
"$('#banner-date').textContent=dStr;\n"
"$('#banner-time').textContent=date.toLocaleTimeString('en-GB',{hour:'2-digit',minute:'2-digit'})+(sys.time_synced?'':' \u00b7 no NTP');\n"
"$('#banner-host').textContent=sys.ip||'\u2014';\n"
"$('#banner-fw').textContent=sys.version?'v'+sys.version:'\u2014';\n"
"$('#banner-cc').textContent=sys.country_code&&sys.country_code!=='XX'?sys.country_code+' \u00b7 '+(countryBand(sys.country_code).split(' \u00b7')[0]):'awaiting geo\u2026';\n"
"// Merge per-tank config (capacity, sleep, distances) from /api/transmitters\n"
"// into the live readings from /api/data. /api/data omits these fields by\n"
"// design; merging here keeps every renderer downstream working from one shape.\n"
"const tanks=(d.tanks||[]).map(t=>{const x=txMap[t.address]||{};return Object.assign({},t,{capacity:x.capacity,sleep:x.sleep,min_dist:x.min_dist,max_dist:x.max_dist,lora_pwr:x.lora_pwr,pending_config:x.pending_config===true,samples:x.samples})});\n"
"tanks.forEach(t=>{if(t.water_pct!=null&&t.state!=='waiting')pushHistory(t.address||t.addr,t.water_pct)});\n"
"const total=tanks.reduce((a,t)=>a+(t.water_pct==null?0:Math.round((t.water_liters||0))),0);\n"
"const onCount=tanks.filter(t=>t.state==='online'||t.state==='connected').length;\n"
"const lastSweep=tanks.length?Math.min(...tanks.filter(t=>t.last_seen_s!=null).map(t=>t.last_seen_s)):null;\n"
"const grid=$('#tanks-grid');\n"
"if(!tanks.length){\n"
"$('#heroH2').innerHTML=`No tanks paired with this Hub yet.`;\n"
"$('#heroLede').textContent='Add your first transmitter to start tracking.';\n"
"grid.innerHTML=`<article class='tcard waiting' style='grid-column:1/-1;text-align:center'><div class='state-eye'>Empty</div><h3>No tanks paired</h3><p class='vol' style='margin-top:14px'>The Hub listens for 60 seconds while you hold the pair button on a transmitter.</p><div style='margin-top:18px'><button class='btn pri' onclick='togglePairing()'>+ Pair first device</button></div></article>`;\n"
"$('#heroSummary').innerHTML='';$('#tankMeta').textContent='\u2014';renderInsights(tanks,sys);renderStatusStrip(tanks,sys,d);return;\n"
"}\n"
"// 'lows' must exclude waiting tanks \u2014 a tank with no reading yet shows pct=0 but isn't low, it's uncalibrated.\n"
"const lows=tanks.filter(t=>t.state!=='waiting'&&t.water_pct!=null&&t.water_pct<15).length;\n"
"const stales=tanks.filter(t=>t.state==='stale').length;\n"
"const waitingN=tanks.filter(t=>t.state==='waiting').length;\n"
"const mood=lows?'one tank is running low':stales?'one transmitter is dozing':waitingN?(waitingN===1?'one tank is waiting for first reading':`${waitingN} tanks are waiting for first reading`):'a quiet update from the property';\n"
"$('#heroH2').innerHTML=`${numToWord(tanks.length)} tank${tanks.length===1?'':'s'} holding <em>${fmtL(total)} litres</em> across the property \u2014 ${mood}.`;\n"
"// 'drawn from' should name a tank that actually has a reading, not just tanks[0].\n"
"const reporting=tanks.filter(t=>t.state!=='waiting'&&t.last_seen_s!=null).sort((a,b)=>a.last_seen_s-b.last_seen_s);\n"
"const sourceTank=reporting[0]||tanks[0];\n"
"$('#heroLede').textContent=`Last reading ${lastSeenStr(lastSweep)}, drawn from ${sourceTank.name||'the first sensor'}.`;\n"
"$('#tankMeta').textContent=`${numToWord(tanks.length)} tank${tanks.length===1?'':'s'} \u00b7 last sweep ${lastSeenStr(lastSweep)}`;\n"
"$('#heroSummary').innerHTML=tanks.map(t=>`<div class='hs-row'><div class='hs-name'>${escapeHTML(t.name||'Tank')}<small>\u2116 ${t.address||t.addr}</small></div><div class='hs-pct'>${t.water_pct==null?'\u2014':t.water_pct}<sub>%</sub></div></div>`).join('');\n"
"grid.innerHTML=tanks.map(t=>{\n"
"const stateLabel={online:'Online',connected:'Online',stale:'Stale',offline:'Offline',waiting:'Waiting',lost:'Offline'}[t.state]||t.state;\n"
"const stCls={online:'online',connected:'online',stale:'stale',offline:'offline',waiting:'waiting',lost:'offline'}[t.state]||'waiting';\n"
"const pct=t.water_pct;const liters=t.water_liters!=null?Math.round(t.water_liters):null;\n"
"return `<article class='tcard ${stCls}' onclick=\"clickTankCard(${t.address||t.addr||0})\">\n"
"<button class='tcard-del' onclick=\"event.stopPropagation();deleteTank(${t.address||t.addr||0},'${escapeHTML(t.name||'Tank').replace(/'/g,\"\\\\'\")}')\" title='Remove this tank' aria-label='Remove tank'><svg viewBox='0 0 24 24'><polyline points='3 6 5 6 21 6'/><path d='M19 6l-1 14a2 2 0 0 1-2 2H8a2 2 0 0 1-2-2L5 6'/><path d='M10 11v6'/><path d='M14 11v6'/><path d='M9 6V4a1 1 0 0 1 1-1h4a1 1 0 0 1 1 1v2'/></svg></button>\n"
"<div class='state-eye'>${stateLabel}${t.state==='waiting'?'':' \u00b7 '+lastSeenStr(t.last_seen_s)}</div>\n"
"<h3>${escapeHTML(t.name||'Tank')}</h3>\n"
"<div class='addr'>\u2116 ${t.address||t.addr||'\u2014'}</div>\n"
"<div class='body-block'><div class='visual'>${tankSvg(pct,t.state)}</div><div class='pct-block'><div class='pct'>${pct==null?'\u2014':pct}<sup>%</sup></div><div class='vol'>${liters==null?'\u2014 last reading lost':'\u2014 '+fmtL(liters)+' L'+(t.capacity?' of '+fmtL(t.capacity)+' L':'')}</div></div></div>\n"
"${predictForTank(t)}\n"
"${tankSparkline(t.address||t.addr)}\n"
"${tankPowerPill(t)}\n"
"<dl class='meta-grid'><dt>Signal</dt><dt>Sleep</dt><dt>Capacity</dt><dd>${(t.rssi!=null&&t.rssi<0)?t.rssi+' dBm':'\u2014'}</dd><dd>${t.sleep?(t.sleep<120?t.sleep+' s':Math.round(t.sleep/60)+' min')+(t.pending_config?' <span class=\"pending-chip\">queued</span>':''):'\u2014'}</dd><dd>${t.capacity?fmtL(t.capacity)+' L':'\u2014'}</dd></dl>\n"
"</article>`}).join('');\n"
"renderInsights(tanks,sys);renderStatusStrip(tanks,sys,d);\n"
"$('#insUpd').textContent='just now';\n"
"}catch(e){console.warn('loadTanks',e)}\n"
"}\n"
"function renderInsights(tanks,sys){\n"
"const list=$('#insightList');const items=[];sys=sys||{};\n"
"const onTanks=tanks.filter(t=>(t.state==='online'||t.state==='connected')&&t.water_pct!=null);\n"
"const lowest=onTanks.slice().sort((a,b)=>a.water_pct-b.water_pct)[0];\n"
"if(lowest&&lowest.water_pct<25){\n"
"const h=pwHistory[lowest.address||lowest.addr]||[];\n"
"let etaText='';\n"
"if(h.length>=3){\n"
"const dt=(h[h.length-1].t-h[0].t)/3600000;\n"
"const slope=dt>0?(h[h.length-1].p-h[0].p)/dt:0;\n"
"if(slope<-0.5){const hrs=(lowest.water_pct-5)/-slope;if(hrs>=1&&hrs<48)etaText=` At the current draw of about ${Math.abs(slope).toFixed(1)}% per hour, this tank reaches its reserve line in roughly ${Math.round(hrs)} hour${Math.round(hrs)===1?'':'s'}.`;}\n"
"}\n"
"items.push({kind:'warn',title:`<em>${escapeHTML(lowest.name)}</em> is running low \u2014 <span class='num-strong'>${lowest.water_pct}%</span>`,body:`Level is below a quarter.${etaText} Consider topping up before the morning rush.`});\n"
"}else if(onTanks.length){\n"
"const totalL=onTanks.reduce((a,t)=>a+(t.water_liters||0),0);\n"
"// Capacity uses ALL paired tanks (registry-known), not just online ones \u2014\n"
"// otherwise an offline 1,000L tank vanishes from the denominator and a\n"
"// single half-full tank reads 96% 'property' capacity when reality is ~64%.\n"
"const totalCap=tanks.reduce((a,t)=>a+(t.capacity||0),0);\n"
"const onlineCap=onTanks.reduce((a,t)=>a+(t.capacity||0),0);\n"
"const pct=totalCap?Math.round(totalL/totalCap*100):0;\n"
"const days=Math.max(1,Math.round(totalL/600));\n"
"let reserveBody;\n"
"if(tanks.length===1)reserveBody=`This tank is at ${pct}% capacity. At average household draw (around 600 L/day), that's roughly ${days} day${days===1?'':'s'} of buffer.`;\n"
"else if(onTanks.length===tanks.length)reserveBody=`Across all tanks the property is at ${pct}% capacity. At average household draw (around 600 L/day), that's roughly ${days} day${days===1?'':'s'} of buffer before a refill.`;\n"
"else{const missing=tanks.length-onTanks.length;reserveBody=`Counting ${onTanks.length} reporting tank${onTanks.length===1?'':'s'} of ${tanks.length}, the property is at <strong>${pct}%</strong> of combined capacity. ${missing} tank${missing===1?'':'s'} not reporting yet \u2014 combined % will rise once ${missing===1?'it checks':'they check'} in. ~${days} day${days===1?'':'s'} of buffer at ~600 L/day from what's known now.`;}\n"
"items.push({kind:'good',title:`Total reserve \u2014 <span class='num-strong'>${fmtL(Math.round(totalL))} L</span>`,body:reserveBody});\n"
"}\n"
"{const drawTank=lowest||onTanks[0];\n"
"if(drawTank){const yd=yesterdayDraw(drawTank.address||drawTank.addr,drawTank.capacity);\n"
"if(yd){const lt=yd.liters!=null?` (about <em>${fmtL(yd.liters)} litres</em>)`:'';\n"
"items.push({kind:'info',title:`Yesterday <em>${escapeHTML(drawTank.name||'this tank')}</em> drew <span class='num-strong'>${yd.pctDrop.toFixed(0)}%</span>${lt}`,body:`Computed from the rolling 24-hour level history kept on the Hub itself \u2014 works even when the cloud is offline. Refills are filtered out so this is your real consumption.`});}}}\n"
"{const solarTank=onTanks.find(t=>solarHealth(t.address||t.addr));\n"
"if(solarTank){const sh=solarHealth(solarTank.address||solarTank.addr);\n"
"if(sh&&sh.msg==='full_recent')items.push({kind:'good',title:`<em>${escapeHTML(solarTank.name)}</em> \u2014 solar topped up <em>${sh.hours_ago<=1?'an hour ago':sh.hours_ago+' hours ago'}</em>`,body:`Battery reached ${sh.peak.toFixed(2)} V in the last 24 hours, consistent with healthy daytime charging.`});\n"
"else if(sh&&sh.msg==='full_old'){const dy=Math.max(1,Math.round(sh.hours_ago/24));items.push({kind:'warn',title:`<em>${escapeHTML(solarTank.name)}</em> \u2014 last full charge was <em>${dy} day${dy===1?'':'s'}</em> ago`,body:`The solar panel may be shaded, dusty, or angled wrong. Battery peaked at ${sh.peak.toFixed(2)} V in the last day. A quick clean of the panel surface usually helps.`});}\n"
"else if(sh&&sh.msg==='no_full_charge')items.push({kind:'warn',title:`<em>${escapeHTML(solarTank.name)}</em> \u2014 battery hasn't fully topped up`,body:`Peak voltage in the last 24 hours was ${sh.peak.toFixed(2)} V, below the typical full-charge plateau (\u2248 4.0 V). Could be a partly-shaded panel or a long cloudy stretch.`});\n"
"else if(sh&&sh.msg==='no_charge_low')items.push({kind:'alert',title:`<em>${escapeHTML(solarTank.name)}</em> \u2014 battery is dropping`,body:`Voltage hasn't recovered in the last day. Check the solar panel and connections at the transmitter end before it reaches cutoff.`});\n"
"}}\n"
"const stale=tanks.find(t=>t.state==='stale');\n"
"if(stale){items.push({kind:'warn',title:`<em>${escapeHTML(stale.name)}</em> — last reading was ${lastSeenStr(stale.last_seen_s)}`,body:`The transmitter is alive but its last reading is older than its sleep cycle. Most often this is a wet sensor head after rain, a slipped connector, or a battery slipping under voltage. Check the transmitter once when you're nearby.`});}\n"
"const offline=tanks.find(t=>t.state==='offline'||t.state==='lost');\n"
"if(offline){items.push({kind:'alert',title:`<em>${escapeHTML(offline.name)}</em> has gone <em>quiet</em>`,body:`No signal in ${lastSeenStr(offline.last_seen_s)}. Likely a flat battery, an out\u2011of\u2011range radio, or a wet electronics housing. Bringing the transmitter close to the Hub usually wakes it.`});}\n"
"// Seasonal/regional insight \u2014 gated on detected country code so the copy\n"
"// fits the user's actual climate. The borewell + monsoon framing is India-\n"
"// specific; for other regions, show neutral seasonal notes or skip.\n"
"const cc=sys.country_code;const month=new Date().getMonth();\n"
"if(cc==='IN'){\n"
"if(month>=5&&month<=9){items.push({kind:'info',title:`Monsoon rule of thumb \u2014 <em>fill the rooftop on dry afternoons</em>`,body:`It's monsoon season. Rooftop tanks tend to gain 8 \u2013 15% during overnight rain via overflow. Topping up before sunset means you wake to a near\u2011full tank without running the pump.`});}\n"
"else{items.push({kind:'info',title:`Dry months \u2014 <em>watch for sediment drift</em>`,body:`Sediment shifts the bottom of underground tanks by a few centimetres each year. If a tank reading swings by more than 4% between two consecutive sweeps, take a moment to re\u2011measure distance\u2011when\u2011empty in <strong>Devices</strong>.`});}\n"
"}else if(cc&&cc!=='XX'){\n"
"const isNH=!['AR','AU','BR','CL','NZ','UY','PY','BO','PE','EC','ZA'].includes(cc);\n"
"const winter=isNH?(month===11||month<=1):(month>=5&&month<=7);\n"
"const summer=isNH?(month>=5&&month<=7):(month===11||month<=1);\n"
"if(winter){items.push({kind:'info',title:`Cold mornings \u2014 <em>pumps work harder</em>`,body:`When ambient drops near freezing, fill pumps draw extra starting current. A small battery dip in the early hours is usually mechanical, not the sensor. Insulating exposed pipework reduces overnight loss.`});}\n"
"else if(summer){items.push({kind:'info',title:`Warm months \u2014 <em>evaporation adds up</em>`,body:`Open or partially-covered tanks can lose 1 \u2013 3% per day to evaporation in summer. If a tank shows a slow steady decline with no draw, that's likely the cause \u2014 not a leak.`});}\n"
"}\n"
"list.innerHTML=items.map(it=>`<div class='insight ${it.kind||'info'}'><div class='ico'>${ico(it.kind==='warn'?'warn':it.kind==='alert'?'alert':it.kind==='good'?'drop':'rain')}</div><div><h4>${it.title}</h4><p>${it.body}</p></div></div>`).join('');\n"
"}\n"
"function renderStatusStrip(tanks,sys,d){\n"
"const onCount=tanks.filter(t=>t.state==='online'||t.state==='connected').length;\n"
"const stales=tanks.filter(t=>t.state==='stale').length;\n"
"const offls=tanks.filter(t=>t.state==='offline'||t.state==='lost').length;\n"
"const subDev=`${onCount} online${stales?' \u00b7 '+stales+' stale':''}${offls?' \u00b7 '+offls+' offline':''}`;\n"
"const linked=!!sys.linked;\n"
"const cloudCls=linked&&d.mqtt_connected?'':'warn';\n"
"// Multi-resolution uptime: <60s \"just booted\", <1h \"Xm\", <1d \"Xh\", else \"Xd Yh\"\n"
"const us=sys.uptime_s||0;\n"
"const upStr=us<60?'just booted':us<3600?Math.round(us/60)+'m':us<86400?Math.floor(us/3600)+'h':(()=>{const d=Math.floor(us/86400);const h=Math.floor((us%86400)/3600);return h?d+'d '+h+'h':d+'d'})();\n"
"const upPart=upStr==='just booted'?upStr:upStr+' uptime';\n"
"const maxTx=sys.max_transmitters||10;\n"
"// Cloud sub-line: distinguish 'broker briefly bounced' from 'broker offline for many minutes' so users\n"
"// with a planned-down server don't think their hub is broken.\n"
"const brokerDownLong=linked&&!d.mqtt_connected&&(sys.uptime_s||0)>900;\n"
"const cloudSub=linked?(d.mqtt_connected?'broker online':(brokerDownLong?'broker unreachable \u2014 check internet':'broker reconnecting')):'unclaimed \u2014 claim on Cloud';\n"
"$('#statusStrip').innerHTML=`<div><div class='lbl'>Hub</div><div class='val'><span class='dot'></span>Operational</div><div class='sub'>v${sys.version||'?'} \u00b7 ${upPart}</div></div><div><div class='lbl'>Devices</div><div class='val'>${tanks.length} of ${maxTx}</div><div class='sub'>${subDev||'no devices'}</div></div><div><div class='lbl'>LoRa</div><div class='val'>${(sys.lora_freq_mhz||865.0).toFixed(1)} MHz</div><div class='sub'>${sys.country_code&&sys.country_code!=='XX'?countryBand(sys.country_code):'listening'}</div></div><div><div class='lbl'>Cloud</div><div class='val'><span class='dot ${cloudCls}'></span>${linked?'Linked':'Local'}</div><div class='sub'>${cloudSub}</div></div>`;\n"
"}\n"
"function countryBand(cc){const m={IN:'India band \u00b7 listening',EU:'EU band \u00b7 listening',DE:'EU band \u00b7 listening',FR:'EU band \u00b7 listening',IT:'EU band \u00b7 listening',GB:'EU band \u00b7 listening',US:'US band \u00b7 listening',CA:'US band \u00b7 listening',AU:'AU band \u00b7 listening',JP:'JP band \u00b7 listening'};return m[cc]||cc+' \u00b7 listening'}\n"
"function editFromTanks(addr){const tr=$$('#tx-tbody tr');for(const r of tr){const b=r.querySelector('button[data-edit]');if(b&&+b.dataset.edit===addr){b.click();return}}}\n"
"// Wrapper for the per-tank card onclick: switch tab, await the device table\n"
"// to populate, THEN auto-open the editor. Without the await, the first click\n"
"// after page load lands on Devices but finds an empty table because\n"
"// loadTransmitters is still in-flight, so editFromTanks finds nothing.\n"
"async function clickTankCard(addr){window.scrollTo(0,0);switchTab('devices',document.querySelector('.tab[data-tab=devices]'));await loadTransmitters();editFromTanks(addr);}\n"
"async function deleteTank(addr,name){if(!addr)return;if(!confirm(`Remove \"${name}\" from this Hub?\\n\\nThe tank stops reporting until it is paired again. If the same physical sensor returns (same MAC), its history is restored automatically.`))return;try{const r=await fetch('/api/transmitters/'+addr,{method:'DELETE'});if(!r.ok)throw new Error('HTTP '+r.status);showToast('Removed',true);loadTanks();loadTransmitters();}catch(e){showToast('Could not remove',false);console.warn('deleteTank',e)}}\n"
"async function loadTransmitters(){\n"
"const d=await api('/api/transmitters');const tb=$('#tx-tbody');if(!tb)return;\n"
"$('#dCount').textContent=d.transmitters&&d.transmitters.length?`\u2014 ${numToWord(d.transmitters.length).toLowerCase()} paired with this Hub.`:'';\n"
"if(!d.transmitters||!d.transmitters.length){tb.innerHTML=`<tr><td colspan='10' style='text-align:center;color:var(--ink3);padding:24px;font-style:italic;font-family:var(--serif)'>No transmitters paired yet.</td></tr>`;return}\n"
"tb.innerHTML=d.transmitters.map(t=>{const fw=t.fw_version&&t.fw_version!=='unknown'?'v'+escapeHTML(t.fw_version):'<span class=\"help\">—</span>';const stateChip=t.pending_config?escapeHTML(t.state)+' <span class=\"pending-chip\">queued</span>':escapeHTML(t.state);return `<tr><td class='n'>${t.address}</td><td>${escapeHTML(t.name)}</td><td class='n'>${t.min_dist}</td><td class='n'>${t.max_dist}</td><td class='n'>${t.capacity}</td><td class='n'>${t.sleep}s</td><td class='n'>${t.samples}</td><td class='n'>${t.lora_pwr||0}</td><td class='n' style='font-family:var(--mono);font-size:12px'>${fw}</td><td>${stateChip}</td><td><button class='btn sm' onclick='identifyTx(${t.address})' title='Blink this tank LED'>Identify</button> <button class='btn sm' data-edit='${t.address}' onclick='editTx(${t.address},${JSON.stringify(t.name)},${t.min_dist},${t.max_dist},${t.capacity},${t.sleep},${t.samples},${t.lora_pwr||0},${JSON.stringify(t.sensor_kind||\"\")})'>Edit</button></td></tr>`}).join('');\n"
"}\n"
"async function identifyTx(addr){try{const r=await api('/api/v1/devices/'+addr+'/identify',{method:'POST'});if(r.ok===false)showToast(r.message||'Identify failed',false);else showToast('Tank LED blinking')}catch(e){showToast('Identify failed: '+e.message,false)}}\n"
"let _ea=0;\n"
"function editTx(a,n,mn,mx,c,s,sa,pw,sk){_ea=a;$('#tx-addr').value=a;$('#tx-addr-display').textContent='\u2116 '+a+' \u00b7 '+n;$('#tx-name').value=n;$('#tx-min').value=mn;$('#tx-max').value=mx;$('#tx-cap').value=c;$('#tx-sleep').value=s;$('#tx-samp').value=sa;$('#tx-pwr').value=pw||0;$('#tx-sensor').value=sk||'';txSensorClamp();window.scrollTo({top:0,behavior:'smooth'})}\n"
// Widen the empty-distance max clamp based on the selected sensor envelope
// (SR04M caps at 400 cm; LD2413 reaches 1050 cm). 'Keep current' uses the
// permissive ceiling so users with an already-paired LD2413 can still edit.
"function txSensorClamp(){const sel=$('#tx-sensor');const sk=sel?sel.value:'';const mx=$('#tx-max');if(!mx)return;mx.max=(sk==='sr04')?400:1050;mx.title=(sk==='sr04')?'Max 400 cm (ultrasonic)':(sk==='ld2413'?'Max 1050 cm (mmWave)':'Max 1050 cm (sensor not changed)');}\n"
"document.addEventListener('change',e=>{if(e.target&&e.target.id==='tx-sensor')txSensorClamp();});\n"
"async function saveTx(){const addr=_ea||+$('#tx-addr').value;if(!addr||addr<1){showToast('Invalid address',false);return}const sleep=+$('#tx-sleep').value;const samp=+$('#tx-samp').value;const mn=+$('#tx-min').value;const mx=+$('#tx-max').value;const pwrR=+$('#tx-pwr').value;const pwr=(isNaN(pwrR)||pwrR<=0)?0:Math.min(22,Math.max(1,pwrR));const sk=$('#tx-sensor').value;if(mn>=mx){showToast('Distance when full must be less than empty',false);return}if(sleep<60){showToast('Sleep must be \u2265 60 seconds',false);return}if(samp<3||samp>20){showToast('Samples must be 3\u201320',false);return}if(sk==='ld2413'&&!confirm('Switch this transmitter to mmWave (LD2413)?\\n\\nThe LD2413 driver is built from the datasheet and has not been bench-verified. The TX will reboot on its next wake to load the new driver.'))return;const b={addr:addr,name:$('#tx-name').value,min_dist:mn,max_dist:mx,capacity:+$('#tx-cap').value,sleep:sleep,samples:samp,lora_pwr:pwr};if(sk)b.sensor_kind=sk;const r=await api('/api/transmitters',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});showToast(r.ok===false?(r.message||'Error'):(sleep>=60?'Saved \u00b7 TX config queued':'Saved'),r.ok!==false);loadTransmitters();}\n"
"async function clearAllTx(){if(confirm('Remove all paired devices?')){const r=await api('/api/transmitters/clear?confirm=1',{method:'POST'});showToast(r.ok===false?(r.message||'Failed'):'Cleared',r.ok!==false);loadTransmitters();}}\n"
"async function factoryReset(){const a=prompt('Factory reset wipes EVERYTHING:\\n  - Wi-Fi credentials\\n  - MQTT cloud link\\n  - All paired tanks + tombstones\\n  - All history\\n  - LoRa NETID\\n\\nThe hub will reboot into setup AP mode.\\n\\nType ERASE to confirm:');if(a!=='ERASE')return;try{const r=await fetch('/api/factory_reset?confirm=YES_ERASE_EVERYTHING',{method:'POST'});const j=await r.json();showToast(j.ok===false?(j.message||'Failed'):'Wiping & rebooting...',j.ok!==false);}catch(e){showToast('Reset triggered (connection dropped)',true);}}\n"
"function pairUpdateRing(secLeft){const r=603.19;const f=$('#pair-fill');const pct=Math.max(0,Math.min(1,(60-secLeft)/60));f.setAttribute('stroke-dashoffset',r*(1-pct))}\n"
"async function togglePairing(){if(pairPoll){clearInterval(pairPoll);pairPoll=null}await api('/api/lora/pairing?start=1',{method:'POST'});$('#pairing-overlay').classList.add('is-open');$('#pair-title').innerHTML='Listening for a <em>new</em> transmitter\u2026';$('#pair-timer').textContent='60';pairUpdateRing(60);pairPoll=setInterval(async()=>{try{const r=await api('/api/lora/pairing');if(r.paired){clearInterval(pairPoll);pairPoll=null;$('#pair-title').textContent='Paired!';$('#pair-msg').innerHTML='New device registered: <strong>'+(r.name||'address '+r.addr)+'</strong>';showToast('Paired',true);loadTransmitters();setTimeout(closePairing,1500)}else if(r.active){$('#pair-timer').textContent=r.time_left;pairUpdateRing(r.time_left)}else{clearInterval(pairPoll);pairPoll=null;$('#pair-title').textContent='Pairing failed';$('#pair-msg').textContent='Timeout \u2014 no transmitter responded.';showToast('Timeout',false)}}catch(e){}},1000);}\n"
"async function closePairing(){if(pairPoll){clearInterval(pairPoll);pairPoll=null}$('#pairing-overlay').classList.remove('is-open');await api('/api/lora/pairing?start=0',{method:'POST'});}\n"
"async function loadWifi(){const d=await api('/api/system');$('#wifi-status').textContent=d.ssid?'Connected \u00b7 '+d.ssid:'Disconnected'}\n"
"async function wifiScan(){const r=await api('/api/wifi/scan');const ul=$('#scan-list');ul.style.display='block';ul.innerHTML=(r.networks||[]).map(n=>`<div class='wifi-row' onclick=\"document.getElementById('wifi-ssid').value='${(n.ssid||'').replace(/'/g,'')}'\"><div class='wifi-name'>${escapeHTML(n.ssid||'(hidden)')}</div><div class='wifi-meta'>${n.rssi} dBm</div></div>`).join('')||'<div class=\"wifi-row\"><div class=\"wifi-name help\">No networks found.</div></div>'}\n"
"async function wifiConnect(){const b={ssid:$('#wifi-ssid').value,password:$('#wifi-pass').value};await api('/api/wifi/connect',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});showToast('Connecting\u2026')}\n"
"async function loadMqtt(){const d=await api('/api/mqtt');$('#mqtt-host').value=d.host||'';$('#mqtt-port').value=d.port||1883;$('#mqtt-user').value=d.user||'';$('#mqtt-en').checked=!!d.enabled;$('#mqtt-ha').checked=!!d.ha_discovery;const b=$('#mqtt-live-badge');if(b){const st=d.live_status||'disabled';b.textContent=({connected:'Online',connecting:'Connecting',disconnected:'Disconnected',disabled:'Disabled'}[st])||st;b.style.color=st==='connected'?'var(--leaf)':st==='connecting'||st==='disconnected'?'var(--warm)':'var(--ink-3)';}}\n"
"async function saveMqtt(){const b={host:$('#mqtt-host').value,port:+$('#mqtt-port').value,user:$('#mqtt-user').value,pass:$('#mqtt-pass').value,enabled:$('#mqtt-en').checked,ha_discovery:$('#mqtt-ha').checked};const r=await api('/api/mqtt',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});showToast(r.message||'Saved');loadMqtt();}\n"
"async function loadLora(){const d=await api('/api/lora');$('#lora-freq').value=d.freq;$('#lora-addr').value=d.addr;}\n"
"async function saveLora(){const b={freq:+$('#lora-freq').value,addr:+$('#lora-addr').value};const r=await api('/api/lora',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});showToast(r.message||'Saved');checkLoraCompliance();}\n"
"async function checkLoraCompliance(){try{const sys=await api('/api/system');const lora=await api('/api/lora');const cc=sys.country_code;const f=lora.freq;const el=$('#lora-compliance');if(!cc||!f){el.classList.remove('on');return}const expected={IN:865000000,EU:868000000,DE:868000000,FR:868000000,IT:868000000,GB:868000000,US:915000000,CA:915000000,AU:915000000,JP:920000000};const exp=expected[cc];if(!exp){el.classList.remove('on');return}const fMHz=Math.round(f/100000)/10;const expMHz=Math.round(exp/100000)/10;if(Math.abs(f-exp)>2000000){el.innerHTML=`Looks like you're in <strong>${cc}</strong> \u2014 typical band is <strong>${expMHz} MHz</strong>, but the Hub is set to ${fMHz} MHz. Different countries license different ISM bands; check local regulations.`;el.classList.add('on');}else{el.classList.remove('on');}}catch(e){}}\n"
"async function loadDisplay(){const d=await api('/api/display');const m=d.mask;$('#scr-water').checked=!!(m&1);$('#scr-battery').checked=!!(m&2);$('#scr-system').checked=!!(m&16);}\n"
"async function saveDisplay(){let m=0;if($('#scr-water').checked)m|=1;if($('#scr-battery').checked)m|=2;if($('#scr-system').checked)m|=16;const r=await api('/api/display',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({mask:m})});showToast(r.message||'Saved')}\n"
"const LED_COLORS=['Blue','Green','Orange','Purple','Yellow','White'];\n"
"let _ledBrightTimer=null;\n"
"async function loadLed(){const d=await api('/api/led');const pill=$('#led-status-pill'),hint=$('#led-status-hint');if(pill){if(!d.initialized){pill.textContent='Driver not started';pill.className='state-eye'}else if(d.strip_present){pill.textContent='Connected';pill.className='state-eye online'}else{pill.textContent='Not detected';pill.className='state-eye stale'}}if(hint){hint.textContent=(d.initialized&&!d.strip_present)?'Check the strip data wire on the Hub. Will retry every 5s.':''}$('#led-count').value=d.count;$('#led-bright').value=d.brightness;$('#led-bright-val').textContent=d.brightness+' of 255';$('#led-bright').oninput=function(){$('#led-bright-val').textContent=this.value+' of 255';if(_ledBrightTimer)clearTimeout(_ledBrightTimer);const v=parseInt(this.value);_ledBrightTimer=setTimeout(()=>{api('/api/led',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({brightness:v})})},200)};const c=$('#led-tank-colors');c.innerHTML='';if(d.tank_colors&&d.tank_colors.length){c.innerHTML='<div class=\"lbl\" style=\"margin-bottom:8px\">Tank colours</div>'+d.tank_colors.map(t=>`<div style='display:flex;align-items:center;gap:10px;margin-bottom:6px;padding:8px 10px;border:1px solid var(--line);background:#fff'><span style='flex:1;font-family:var(--serif);font-size:15px'>${escapeHTML(t.name)}</span><select data-addr='${t.addr}' style='width:auto'><option value='-1'>Auto</option>${LED_COLORS.map((n,i)=>`<option value='${i}' ${t.color_idx===i?'selected':''}>${n}</option>`).join('')}</select></div>`).join('');c.querySelectorAll('select').forEach(s=>{if(d.tank_colors.find(t=>t.addr==s.dataset.addr&&t.color_idx===-1))s.value='-1';s.onchange=function(){api('/api/led',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({tank_colors:[{addr:parseInt(this.dataset.addr),color_idx:parseInt(this.value)}]})})}})}}\n"
"async function saveLed(){const colors=[];$$('#led-tank-colors select').forEach(s=>colors.push({addr:parseInt(s.dataset.addr),color_idx:parseInt(s.value)}));const body={count:parseInt($('#led-count').value),brightness:parseInt($('#led-bright').value),tank_colors:colors};const r=await api('/api/led',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});showToast(r.message||'Saved');loadLed()}\n"
"function bzRow(a){return `<tr data-ev='${a.event}'><td>${escapeHTML(a.name)}</td><td><label class='chk' style='padding:0;background:none;border:0'><input type='checkbox' class='bz-en' ${a.enabled?'checked':''}><div></div></label></td><td><button class='btn sm' onclick='testBuzzer(${a.event})'>Play</button></td></tr>`}\n"
"function bzPopulateHourSelects(){const opts=Array.from({length:24},(_,h)=>`<option value='${h}'>${String(h).padStart(2,'0')}:00</option>`).join('');const qs=$('#bz-qstart'),qe=$('#bz-qend');if(qs&&!qs.options.length)qs.innerHTML=opts;if(qe&&!qe.options.length)qe.innerHTML=opts}\n"
"function bzApplyDisclosure(){const on=$('#bz-master').checked;const form=$('#bz-active-form'),sum=$('#bz-collapsed-summary');if(!form||!sum)return;if(on){form.style.display='';sum.style.display='none'}else{const profNames=['Quiet','Standard','Loud'];const prof=parseInt($('#bz-profile').value)||1;const onCount=$$('#bz-alert-rows .bz-en:checked').length;const qs=parseInt($('#bz-qstart').value)||0,qe=parseInt($('#bz-qend').value)||0;const fmt=h=>String(h).padStart(2,'0')+':00';const quiet=(qs===qe)?'no quiet hours':`quiet hours ${fmt(qs)} → ${fmt(qe)}`;sum.innerHTML=`Settings preserved: <strong>${profNames[prof]||'Standard'}</strong> volume, <strong>${onCount}</strong> essential alerts on, ${quiet}. Boot tone always plays.`;form.style.display='none';sum.style.display=''}}\n"
"async function loadBuzzer(){bzPopulateHourSelects();const d=await api('/api/buzzer');$('#bz-master').checked=!!d.master_enable;$('#bz-crit-override').checked=!!d.critical_overrides_quiet;$('#bz-profile').value=String(d.master_profile==null?1:d.master_profile);$('#bz-qstart').value=String(d.quiet_start_hour||0);$('#bz-qend').value=String(d.quiet_end_hour||0);const t1=[],t23=[];(d.alerts||[]).forEach(a=>{if(a.event===0||a.event===4)return;if(a.event===9||a.event===10||a.event===11)return;if(a.tier===1)t1.push(bzRow(a));else t23.push(bzRow(a))});$('#bz-alert-rows').innerHTML=t1.join('')||'<tr><td colspan=\"3\" class=\"help\">No essential alerts.</td></tr>';$('#bz-advanced-rows').innerHTML=t23.join('');$('#bz-advanced-count').textContent=`(${t23.length} optional)`;bzApplyDisclosure();}\n"
"async function saveBuzzer(){const rows=[...$$('#bz-alert-rows tr[data-ev]'),...$$('#bz-advanced-rows tr[data-ev]')];const alerts=rows.map(r=>({event:parseInt(r.dataset.ev),enabled:r.querySelector('.bz-en').checked}));const body={master_enable:$('#bz-master').checked,master_profile:parseInt($('#bz-profile').value)||1,critical_overrides_quiet:$('#bz-crit-override').checked,quiet_start_hour:parseInt($('#bz-qstart').value)||0,quiet_end_hour:parseInt($('#bz-qend').value)||0,alerts};const r=await api('/api/buzzer',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});showToast(r.message||'Saved')}\n"
"async function testBuzzer(ev){const prof=parseInt($('#bz-profile').value)||1;await api('/api/buzzer/test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({event:ev,profile:prof})});showToast('Playing pattern')}\n"
"async function loadTz(){const d=await api('/api/tz');$('#tz-string').value=d.tz||'';const suggest=d.suggested||'';const cc=d.country||'?\?';$('#tz-suggested').innerHTML=suggest?`${escapeHTML(suggest)} <span class='help' style='margin-left:6px'>(${escapeHTML(cc)})</span>`:`<span class='help'>No suggestion (${escapeHTML(cc)})</span>`;const synced=d.synced?'synced':'waiting for SNTP';$('#tz-status').textContent='Time '+synced+'. Empty + Apply = fall back to auto-detect on next boot.'}\n"
"async function saveTz(){const body={tz:$('#tz-string').value.trim()};const r=await api('/api/tz',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});showToast(r.message||'Saved');loadTz()}\n"
"function tzUseSuggested(){const t=$('#tz-suggested').textContent.split(' ')[0];if(t&&t!=='No')$('#tz-string').value=t}\n"
"function tzClear(){$('#tz-string').value=''}\n"
"async function identifyHub(){try{await api('/api/v1/hub/identify',{method:'POST'});showToast('Status LED is blinking')}catch(e){showToast('Identify failed: '+e.message)}}\n"
"async function rebootHub(){if(!confirm('Restart the Hub now? It will be unreachable for ~30 seconds.'))return;try{await api('/api/v1/hub/reboot',{method:'POST'});showToast('Restarting Hub…')}catch(e){showToast('Restart issued')}}\n"
"function uploadTx(){const f=$('#tx-bin-file').files[0];if(!f){showToast('Select a .bin file',false);return}const prog=$('#tx-upload-progress');const bar=$('#tx-upload-bar');const lbl=$('#tx-upload-label');const info=$('#tx-staged-info');const btn=$('#tx-upload-btn');prog.classList.remove('hide');btn.disabled=true;info.textContent='';const xhr=new XMLHttpRequest();xhr.upload.onprogress=e=>{if(e.lengthComputable){const pct=Math.round(e.loaded*100/e.total);bar.style.width=pct+'%';lbl.textContent=pct+'% ('+Math.round(e.loaded/1024)+' KB / '+Math.round(e.total/1024)+' KB)'}};xhr.onload=()=>{btn.disabled=false;try{const r=JSON.parse(xhr.responseText);if(r.ok){showToast('Staged '+Math.round(r.bytes/1024)+' KB');loadOta()}else{showToast(r.message||'Upload failed',false);info.textContent='Error: '+(r.message||'upload failed');prog.classList.add('hide')}}catch(e){showToast('Server error',false);prog.classList.add('hide')}};xhr.onerror=()=>{btn.disabled=false;showToast('Network error',false);prog.classList.add('hide')};xhr.open('POST','/api/ota/upload_tx');xhr.send(f)}\n"
"let otaPoll=null;\n"
"function compareSemverTx(a,b){const pa=String(a||'0.0.0').split('.').map(n=>parseInt(n,10)||0);const pb=String(b||'0.0.0').split('.').map(n=>parseInt(n,10)||0);for(let i=0;i<3;i++){if(pa[i]!==pb[i])return pa[i]>pb[i]?1:-1}return 0}\n"
"async function loadTxFirmware(){const el=$('#tx-fw-list');const refreshBtn=$('#tx-fw-refresh-btn');if(!el)return;el.innerHTML='<span style=\"display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--rain);margin-right:6px;animation:pulse 1s infinite\"></span>Refreshing transmitter status\u2026';if(refreshBtn){refreshBtn.disabled=true;refreshBtn.textContent='Refreshing\u2026'}showToast('Refreshing transmitter status\u2026');\n"
"// Track whether the cloud reach was network-failure (offline) vs successful-but-empty (no release).\n"
"// Distinguishes 'we can't reach the cloud' from 'cloud has no TX release yet' \u2014 same UI before, different fix.\n"
"let latestVer=null,cloudReachable=true;\n"
"try{\n"
"const statusResp=await api('/api/ota/tx_status').catch(()=>({devices:[]}));\n"
"try{const latestResp=await fetch('https://tanksync.smartghar.org/api/firmware/latest?target=tx',{signal:AbortSignal.timeout(8000)});if(latestResp.ok){const j=await latestResp.json();latestVer=(j&&j.tag_name)?j.tag_name.replace(/^v/,''):null;}else if(latestResp.status>=500){cloudReachable=false;}}catch(e){cloudReachable=false;}\n"
"const devices=statusResp.devices||[];\n"
"if(!devices.length){el.innerHTML='<p class=\"help\">No paired transmitters yet. Pair one from the <strong>Devices</strong> tab.</p>';return}\n"
"el.innerHTML=devices.map(dev=>{const cur=dev.fw_version||'';let pill,pillCls,hint='';if(!cur){pill='No version reported yet';pillCls='state-eye stale';hint='<div class=\"help\" style=\"margin-top:6px\">The transmitter hasn\\'t reported its version yet \u2014 wait for the next wake cycle, or re-pair.</div>'}else if(latestVer){const cmp=compareSemverTx(cur,latestVer);if(cmp>=0){pill='Up to date';pillCls='state-eye online'}else{pill='Update available \u00b7 v'+latestVer;pillCls='state-eye stale';hint='<div class=\"help\" style=\"margin-top:6px;color:var(--rust)\">A newer version is available. See <strong>How to update a transmitter (Wi-Fi)</strong> below for the flash steps.</div>'}}else if(!cloudReachable){pill='Cloud unreachable';pillCls='state-eye warn';hint='<div class=\"help\" style=\"margin-top:6px\">Can\\'t reach <code>tanksync.smartghar.org</code> right now to check for new TX releases. The hub itself is fine \u2014 try again later.</div>'}else{pill='No release published';pillCls='state-eye';hint='<div class=\"help\" style=\"margin-top:6px\">No TX firmware release exists on the cloud yet. Manual flash via Wi-Fi is the only option.</div>'}return `<div style='border:1px solid var(--line);padding:12px 14px;margin-bottom:8px;background:var(--paper)'><div style='display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px'><span style='font-family:var(--serif);font-size:15px'><strong>${escapeHTML(dev.name||'(unnamed)')}</strong> <span class='help'>\u00b7 addr ${dev.addr||dev.address||'?'}</span></span><span class='help' style='font-family:var(--mono);font-size:12px'>v${escapeHTML(cur||'?')}</span><span class='${pillCls}'>${pill}</span></div>${hint}</div>`}).join('')\n"
"}catch(e){el.innerHTML='<p class=\"help\">Could not load TX firmware status. Check your connection.</p>'}finally{if(refreshBtn){refreshBtn.disabled=false;refreshBtn.textContent='Refresh status'}}}\n"
"async function clearStaged(){if(!confirm('Clear staged firmware?'))return;await api('/api/ota/clear_tx',{method:'POST'});showToast('Staged firmware cleared');loadOta()}\n"
"async function loadOta(){const sys=await api('/api/system');$('#rx-ver').textContent='v'+(sys.version||'?');try{const staged=await api('/api/ota/tx_staged');const info=$('#tx-staged-info');const clrBtn=$('#tx-clear-btn');if(staged.staged){const ver=staged.version||'?';const kb=Math.round(staged.bytes/1024);if(info)info.textContent='Staged: v'+ver+' \u2014 '+kb+' KB \u00b7 ready for LoRa-OTA when re-enabled';if(clrBtn)clrBtn.classList.remove('hide')}else{if(info)info.textContent='';if(clrBtn)clrBtn.classList.add('hide')}}catch(e){}try{const sp=await api('/api/spiffs/info');$('#spiffs-info').textContent='Storage: '+Math.round(sp.used/1024)+' KB / '+Math.round(sp.total/1024)+' KB ('+Math.round(sp.free/1024)+' KB free)'}catch(e){}await otaRefresh();await loadTxFirmware()}\n"
"function otaSetDownloadGuard(on){if(on){window.onbeforeunload=()=>'Firmware download in progress \u2014 leaving will interrupt it.'}else{window.onbeforeunload=null}}\n"
"async function otaRefresh(){try{const sys=await api('/api/system');const s=await api('/api/ota/state');const cur=sys.version||'?';const text=$('#ota-status');const verLine=$('#ota-version-line');const btn=$('#ota-action-btn');const prog=$('#ota-progress');const bar=$('#ota-bar');const lbl=$('#ota-progress-label');\n"
"// Effective status: if user just clicked 'Check' but firmware hasn't woken yet (status still idle/checking), keep showing 'Checking\u2026'\n"
"const effective=(window.otaCheckPending&&(s.status==='idle'||s.status==='checking'))?'checking':s.status;\n"
"// Clear the pending flag once firmware moves past idle/checking \u2014 we got a real result\n"
"if(s.status!=='idle'&&s.status!=='checking')window.otaCheckPending=false;\n"
"window.otaCurrentStatus=effective;btn.disabled=false;btn.classList.remove('pri');btn.style.pointerEvents='';prog.style.display='none';verLine.style.display='none';otaSetDownloadGuard(false);\n"
"if(effective==='checking'){text.innerHTML='<span style=\"display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--rain);margin-right:6px;animation:pulse 1s infinite\"></span>Checking for updates\u2026';btn.textContent='Checking\u2026';btn.disabled=true;btn.style.pointerEvents='none';btn.dataset.act='check';if(!window.otaPoll)window.otaPoll=setInterval(otaRefresh,2000)}else if(effective==='available'){text.innerHTML='<strong style=\"color:var(--rain)\">New firmware available</strong>';verLine.textContent='v'+cur+' \u2192 v'+(s.latest_version||'?');verLine.style.display='';btn.textContent='Download & install v'+(s.latest_version||'?');btn.classList.add('pri');btn.dataset.act='update';if(window.otaPoll){clearInterval(window.otaPoll);window.otaPoll=null}if(window.otaJustChecked){showToast('Update available \u00b7 v'+(s.latest_version||'?'));window.otaJustChecked=false}}else if(effective==='downloading'){text.innerHTML='<strong style=\"color:var(--rain)\">Downloading firmware \u2014 do not power off or navigate away.</strong>';btn.textContent='Downloading\u2026';btn.disabled=true;btn.style.pointerEvents='none';prog.style.display='';bar.style.width=(s.progress||0)+'%';lbl.textContent=(s.progress||0)+'%';btn.dataset.act='update';otaSetDownloadGuard(true);if(!window.otaPoll)window.otaPoll=setInterval(otaRefresh,1500)}else if(effective==='done'){text.textContent='Update complete \u2014 rebooting\u2026';btn.textContent='Rebooting\u2026';btn.disabled=true;btn.style.pointerEvents='none';btn.dataset.act='check';if(window.otaPoll){clearInterval(window.otaPoll);window.otaPoll=null}otaSetDownloadGuard(false);let tries=0;const reboot=setInterval(async()=>{tries++;if(tries>40){clearInterval(reboot);text.textContent='Device not responding';return}try{const r=await fetch('/api/system',{signal:AbortSignal.timeout(3000)});if(r.ok){clearInterval(reboot);showToast('Update complete \u00b7 reloading');setTimeout(()=>location.reload(),1200)}}catch(e){}},3000)}else if(effective==='up_to_date'){text.innerHTML='<span style=\"color:var(--leaf)\">\u2713</span> You\\'re on the latest \u2014 v'+cur;btn.textContent='Check again';btn.dataset.act='check';if(window.otaPoll){clearInterval(window.otaPoll);window.otaPoll=null}if(window.otaJustChecked){showToast('You\\'re already on the latest version');window.otaJustChecked=false}}else if(effective==='error'){text.textContent='Error: '+(s.error_msg||'unknown');btn.textContent='Retry check';btn.dataset.act='check';if(window.otaPoll){clearInterval(window.otaPoll);window.otaPoll=null}window.otaJustChecked=false}else{text.textContent='Current \u00b7 v'+cur;btn.textContent='Check for updates';btn.dataset.act='check'}}catch(e){$('#ota-status').textContent='Could not load OTA status'}}\n"
"async function otaAction(){const btn=$('#ota-action-btn');const act=btn.dataset.act||'check';if(window.otaCurrentStatus==='downloading'||window.otaCurrentStatus==='checking'){showToast('Please wait \u2014 '+window.otaCurrentStatus+' in progress',false);return}if(act==='check'){btn.disabled=true;btn.style.pointerEvents='none';showToast('Checking for updates\u2026');\n"
"// Optimistic UI: set checking state immediately so user sees feedback (firmware OTA task wakes every 60s, can take a moment to actually start the check)\n"
"$('#ota-status').innerHTML='<span style=\"display:inline-block;width:8px;height:8px;border-radius:50%;background:var(--rain);margin-right:6px;animation:pulse 1s infinite\"></span>Checking for updates\u2026';\n"
"btn.textContent='Checking\u2026';\n"
"window.otaCheckPending=true;window.otaJustChecked=true;\n"
"// Auto-clear the pending flag after 90s if firmware never responds (so we don't get stuck showing 'Checking\u2026' forever)\n"
"setTimeout(()=>{if(window.otaCheckPending){window.otaCheckPending=false;showToast('Check timed out \u2014 try again',false);otaRefresh()}},90000);\n"
"if(window.otaPoll){clearInterval(window.otaPoll)}window.otaPoll=setInterval(otaRefresh,2000);\n"
"try{const r=await api('/api/ota/check',{method:'POST'});if(r&&r.ok===false){showToast(r.message||'Check failed',false);window.otaCheckPending=false;window.otaJustChecked=false}}catch(e){showToast('Network error',false);window.otaCheckPending=false;window.otaJustChecked=false}}else if(act==='update'){if(!confirm('Download and install firmware v'+($('#ota-action-btn').textContent.replace(/^.*v/,''))+'?\\n\\nThe Hub will reboot when finished. Do not power off or close this tab during the download (~30s).'))return;btn.disabled=true;btn.style.pointerEvents='none';showToast('Starting download \u2014 please keep this tab open');try{const r=await api('/api/ota/update',{method:'POST'});if(r&&r.ok===false){showToast(r.message||'Update failed to start',false)}}catch(e){showToast('Network error',false)}setTimeout(otaRefresh,1200)}}\n"
"function uploadBin(){const f=$('#bin-file').files[0];if(!f){showToast('Select file',false);return}const xhr=new XMLHttpRequest();xhr.open('POST','/api/ota/upload');function waitReboot(){showToast('Flashing \u2014 rebooting\u2026');$('#ota-status').textContent='Rebooting \u2014 waiting for device\u2026';let tries=0;const poll=setInterval(async()=>{tries++;if(tries>30){clearInterval(poll);showToast('Device not responding',false);return}try{const r=await fetch('/api/system',{signal:AbortSignal.timeout(3000)});if(r.ok){clearInterval(poll);showToast('Update complete \u2014 reloading');setTimeout(()=>location.reload(),1500)}}catch(e){}},3000)}xhr.onload=()=>{try{const r=JSON.parse(xhr.responseText);if(r.ok)waitReboot();else showToast(r.message||'Error',false)}catch(e){waitReboot()}};xhr.onerror=()=>waitReboot();showToast('Uploading\u2026');xhr.send(f)}\n"
"async function loadClaim(){try{const sys=await api('/api/system');const m=await api('/api/mqtt');const id=(sys.device_id||'').toUpperCase();const chunked=id.length===12?id.slice(0,4)+' '+id.slice(4,8)+' '+id.slice(8,12):id;$('#claim-device-id').textContent=chunked||'(unknown)';const url='https://tanksync.smartghar.org/setup?device_id='+(sys.device_id||'').toLowerCase();$('#claim-setup-url').textContent=url;$('#claim-qr').src='https://api.qrserver.com/v1/create-qr-code/?size=170x170&data='+encodeURIComponent(url);const txt=$('#claim-status-text');const linked=!!sys.linked;const isOnline=m&&m.live_status==='connected';const sub=$('#cloud-sub');const unlinkBox=$('#claim-unlink-box');if(linked&&isOnline){txt.textContent='Linked';txt.style.color='var(--leaf)';sub.textContent='Connected as '+(m.user||'(account)');unlinkBox.style.display=''}else if(linked){txt.textContent='Linked \u00b7 broker offline';txt.style.color='var(--warm)';sub.textContent='Retrying\u2026';unlinkBox.style.display=''}else{txt.textContent='Unlinked';txt.style.color='var(--ink-3)';sub.textContent='Not yet linked.';unlinkBox.style.display='none'}}catch(e){showToast('Load failed: '+e.message,false)}}\n"
"function claimCopy(t){const ok=()=>showToast('Copied');const fail=()=>showToast('Copy failed \u2014 long\u2011press to select',false);try{if(navigator.clipboard&&window.isSecureContext){navigator.clipboard.writeText(t).then(ok).catch(()=>legacyCopy(t)?ok():fail())}else{legacyCopy(t)?ok():fail()}}catch(e){legacyCopy(t)?ok():fail()}}\n"
"function legacyCopy(t){try{const ta=document.createElement('textarea');ta.value=t;ta.style.position='fixed';ta.style.opacity='0';document.body.appendChild(ta);ta.focus();ta.select();const r=document.execCommand('copy');document.body.removeChild(ta);return r}catch(e){return false}}\n"
"async function claimUnlink(){if(!confirm(\"Unlink from cloud? This clears the receiver's MQTT broker credentials. Tank data will stop publishing to the cloud until you re\u2011link via the app wizard.\"))return;try{const r=await fetch('/api/mqtt',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({host:'',port:1883,user:'',pass:'',enabled:false,ha_discovery:false,use_tls:false})});if(r.ok){showToast('Unlinked');loadClaim()}else{showToast('Unlink failed',false)}}catch(e){showToast('Unlink failed: '+e.message,false)}}\n"
"// Track the active tab so we only poll /api/data + /api/system + /api/transmitters\n"
"// when the user is actually looking at the Tanks dashboard. Other tabs get a\n"
"// single fetch on switch (their own loadX functions) and don't need 5s polling.\n"
"window.activeTab='tanks';\n"
"const origSwitchTab=switchTab;switchTab=function(t,el){window.activeTab=t;return origSwitchTab(t,el);};\n"
"loadTanks();setInterval(()=>{if(window.activeTab==='tanks')loadTanks();},5000);\n"
"</script>\n"
"<div style=\"text-align:center;padding:24px 14px 32px;color:var(--ink-3);font-family:var(--serif);font-size:12px;line-height:1.7;border-top:1px dashed var(--line);margin-top:36px\">A SmartGhar product &middot; Made with care in India &middot; Open at heart</div>\n"
"</body></html>\n";

static void send_json(httpd_req_t *req, const char *json) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, json);
}
static void json_safe_copy(char *dst, const char *src, size_t max) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < max - 1; i++) {
        if (src[i] == '"' || src[i] == '\\' || src[i] == '\n' || src[i] == '\r')
            dst[j++] = '_';
        else
            dst[j++] = src[i];
    }
    dst[j] = '\0';
}
static void send_ok(httpd_req_t *req, const char *msg) {
    char safe[64]; json_safe_copy(safe, msg ? msg : "OK", sizeof(safe));
    char buf[128]; snprintf(buf, sizeof(buf), "{\"ok\":true,\"message\":\"%s\"}", safe); send_json(req, buf);
}
static void send_err(httpd_req_t *req, const char *msg) {
    char safe[64]; json_safe_copy(safe, msg ? msg : "Error", sizeof(safe));
    char buf[128]; snprintf(buf, sizeof(buf), "{\"ok\":false,\"message\":\"%s\"}", safe); httpd_resp_set_status(req, "400 Bad Request"); send_json(req, buf);
}
static char *read_body(httpd_req_t *req) {
    int len = req->content_len; if (len <= 0 || len > 4096) return NULL;
    char *buf = malloc(len + 1); if (!buf) return NULL;
    int received = 0; while (received < len) { int r = httpd_req_recv(req, buf + received, len - received); if (r <= 0) { free(buf); return NULL; } received += r; }
    buf[len] = '\0'; return buf;
}

static esp_err_t handle_root(httpd_req_t *req) { httpd_resp_set_type(req, "text/html; charset=utf-8"); httpd_resp_sendstr(req, DASHBOARD_HTML); return ESP_OK; }

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
        // Power telemetry (TX firmware v2.0.4+; '?' for older TX firmware)
        const char *pmode_str = (data.power_mode == 'i') ? "ina219"
                              : (data.power_mode == 'v') ? "voltage"
                              : (data.power_mode == 'n') ? "none"
                              : "unknown";
        cJSON_AddStringToObject(t, "power_mode", pmode_str);
        cJSON_AddNumberToObject(t, "current_ma", (double)data.current_ma);
        cJSON_AddNumberToObject(t, "power_mw",   (double)data.power_mw);
        cJSON_AddBoolToObject  (t, "charging",   data.charging);
        int64_t age_s = data.last_update_us > 0
            ? (esp_timer_get_time() - data.last_update_us) / 1000000LL : -1;
        cJSON_AddNumberToObject(t, "last_seen_s", (double)age_s);
        cJSON_AddItemToArray(tanks, t);
    }
    char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root); send_json(req, json); free(json);
    return ESP_OK;
}

// /api/tanks/history — 48-slot half-hourly rolling window per tank.
// Output: {slot_seconds, now, synced, tanks:[{addr,head_t,samples,pct[],volt[]}]}
// pct/volt arrays are oldest→newest; nulls mark slots that were never written.
// Voltage is decoded from the 0.05 V quantisation back to a float for JS.
static esp_err_t handle_api_tanks_history(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "slot_seconds", TX_HIST_SLOT_SECS);
    int64_t now_epoch = (int64_t)time(NULL);
    cJSON_AddNumberToObject(root, "now",   (double)now_epoch);
    cJSON_AddBoolToObject  (root, "synced", geo_time_is_synced());
    cJSON *tanks = cJSON_AddArrayToObject(root, "tanks");

    for (int i = 0; i < registry_count(); i++) {
        tx_info_t info;
        if (!registry_get_info(i, &info) || !info.enabled) continue;

        uint8_t pct[TX_HIST_SLOTS], volt_dv[TX_HIST_SLOTS];
        int64_t head_t = 0;
        bool has_hist = registry_get_history(i, pct, volt_dv, &head_t);

        cJSON *t = cJSON_CreateObject();
        cJSON_AddNumberToObject(t, "addr", info.address);
        cJSON_AddNumberToObject(t, "head_t", (double)(has_hist ? head_t : 0));

        cJSON *p_arr = cJSON_AddArrayToObject(t, "pct");
        cJSON *v_arr = cJSON_AddArrayToObject(t, "volt");
        int samples = 0;

        for (int s = 0; s < TX_HIST_SLOTS; s++) {
            if (!has_hist || pct[s] == TX_HIST_PCT_EMPTY) {
                cJSON_AddItemToArray(p_arr, cJSON_CreateNull());
            } else {
                cJSON_AddItemToArray(p_arr, cJSON_CreateNumber(pct[s]));
                samples++;
            }
            if (!has_hist || volt_dv[s] == TX_HIST_VOLT_EMPTY) {
                cJSON_AddItemToArray(v_arr, cJSON_CreateNull());
            } else {
                cJSON_AddItemToArray(v_arr, cJSON_CreateNumber(volt_dv[s] * 0.05));
            }
        }
        cJSON_AddNumberToObject(t, "samples", samples);
        cJSON_AddItemToArray(tanks, t);
    }

    char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root); send_json(req, json); free(json);
    return ESP_OK;
}

// ─── SmartGhar protocol v1 — local LAN API for Home Assistant + 3rd-party clients ──
//
// Spec: https://github.com/Techposts/smartghar-homeassistant/blob/main/docs/protocol/v1.md
// Discovery: hub broadcasts _smartghar._tcp on mDNS (see wifi_manager.c::start_mdns).
//
// Phase 1.1 ships the two endpoints needed for HA's config flow to validate
// a hub and instantiate sensor entities: GET /api/v1/info and GET /api/v1/devices.
// WebSocket stream + bidirectional control land in Phase 1.2.

static esp_err_t handle_api_v1_info(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "schema_version", "1.0");
    cJSON_AddStringToObject(root, "hub_id",         wifi_manager_hub_id());
    cJSON_AddStringToObject(root, "hub_name",       wifi_manager_mdns_host());
    cJSON_AddStringToObject(root, "fw_version",     FIRMWARE_VERSION);
    cJSON_AddNumberToObject(root, "uptime_s",       (double)(esp_timer_get_time() / 1000000LL));

    wifi_status_t ws = wifi_manager_status();
    cJSON_AddNumberToObject(root, "wifi_rssi",
        ws == WIFI_ST_CONNECTED ? wifi_manager_rssi() : 0);
    cJSON_AddBoolToObject  (root, "claimed",        mqtt_manager_is_linked());

    // device_kinds — extends with "power", "gas", etc. as ecosystem ships
    cJSON *kinds = cJSON_AddArrayToObject(root, "device_kinds");
    cJSON_AddItemToArray(kinds, cJSON_CreateString("tank"));

    // OTA status — `available` is null when up-to-date or never checked
    ota_state_t ota = {0};
    ota_manager_get_state(&ota);
    cJSON *ota_obj = cJSON_AddObjectToObject(root, "ota");
    cJSON_AddStringToObject(ota_obj, "current",   FIRMWARE_VERSION);
    if (ota.status == OTA_ST_AVAILABLE && ota.latest_version[0]) {
        cJSON_AddStringToObject(ota_obj, "available", ota.latest_version);
    } else {
        cJSON_AddNullToObject(ota_obj, "available");
    }
    cJSON_AddStringToObject(ota_obj, "channel",   "stable");

    char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root); send_json(req, json); free(json);
    return ESP_OK;
}

// Forward declaration — definition below.
static cJSON *build_v1_device_obj(int idx);

static esp_err_t handle_api_v1_devices(httpd_req_t *req) {
    cJSON *root    = cJSON_CreateObject();
    cJSON *devices = cJSON_AddArrayToObject(root, "devices");
    // Use the shared per-device builder so the list endpoint, the single-device
    // endpoint, and the WebSocket snapshot all emit the same shape. Drift here
    // is how HA loses fields silently.
    for (int i = 0; i < registry_count(); i++) {
        cJSON *d = build_v1_device_obj(i);
        if (d) cJSON_AddItemToArray(devices, d);
    }
    char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root); send_json(req, json); free(json);
    return ESP_OK;
}

// Extract the device id from /api/v1/devices/<id> or /api/v1/devices/<id>/<suffix>.
// Returns 0 if URI doesn't parse. `suffix_out` (optional) is set to the path
// segment AFTER the id, e.g. "history" for /api/v1/devices/123/history; NULL
// or empty if no suffix.
static uint16_t parse_v1_device_id(const char *uri, const char **suffix_out) {
    static const char prefix[] = "/api/v1/devices/";
    const char *p = strstr(uri, prefix);
    if (!p) { if (suffix_out) *suffix_out = NULL; return 0; }
    p += sizeof(prefix) - 1;
    char *end = NULL;
    long id = strtol(p, &end, 10);
    if (end == p || id <= 0 || id > 65535) {
        if (suffix_out) *suffix_out = NULL;
        return 0;
    }
    if (suffix_out) {
        // *end is either '\0' or '/'. If '/', suffix starts after it.
        *suffix_out = (*end == '/') ? end + 1 : NULL;
    }
    return (uint16_t)id;
}

// Build the per-device JSON object used by both the list endpoint and the
// single-device endpoint. Caller owns the returned cJSON node.
//
// Phase 1.5 (2026-05-17): added power, fw, and pending_config so HA + 3rd-party
// clients see the same telemetry the dashboard and PWA do. Previously these
// were only on /api/data + /api/transmitters, so HA users couldn't track
// charging state or know which TX firmware was running.
static cJSON *build_v1_device_obj(int idx) {
    tx_info_t info; tx_data_t data;
    if (!registry_get_info(idx, &info) || !registry_get_data(idx, &data)) return NULL;
    if (!info.enabled) return NULL;

    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "id",   info.address);
    cJSON_AddStringToObject(d, "kind", "tank");
    cJSON_AddStringToObject(d, "name", info.name);
    cJSON_AddStringToObject(d, "fw",   data.fw_version[0] ? data.fw_version : "unknown");
    // Stable hardware MAC (rx-v2.7.11+). Empty string for legacy pre-MAC
    // entries — HA can fall back to numeric "id" then. Once exposed, this
    // becomes the recommended anchor for unique_id and entity_id continuity
    // across re-pairs and hub upgrades.
    {
        const uint8_t *m = info.mac;
        bool has_mac = false;
        for (int b = 0; b < 6; b++) if (m[b]) { has_mac = true; break; }
        if (has_mac) {
            char machex[13];
            snprintf(machex, sizeof(machex), "%02x%02x%02x%02x%02x%02x",
                     m[0], m[1], m[2], m[3], m[4], m[5]);
            cJSON_AddStringToObject(d, "mac", machex);
        } else {
            cJSON_AddStringToObject(d, "mac", "");
        }
    }

    cJSON *state = cJSON_AddObjectToObject(d, "state");
    cJSON_AddNumberToObject(state, "level_pct", data.water_pct);
    cJSON_AddNumberToObject(state, "voltage",   data.battery_voltage);
    cJSON_AddNumberToObject(state, "rssi",      data.rssi);
    bool synced = geo_time_is_synced();
    if (synced && data.last_update_us > 0) {
        int64_t age_s = (esp_timer_get_time() - data.last_update_us) / 1000000LL;
        cJSON_AddNumberToObject(state, "ts", (double)((int64_t)time(NULL) - age_s));
    } else {
        cJSON_AddNumberToObject(state, "ts", 0);
    }
    cJSON_AddStringToObject(state, "conn_state", registry_state_str(data.state));

    // Sensor health flags (rx-v2.8.3+): both surface to HA as binary_sensors.
    // sensor_error = explicit failure from TX (or dist=0 heuristic for legacy
    //                TX) — UI shows "Sensor not responding" with stale reading.
    // sensor_stuck = variance window detected constant reading — UI shows
    //                "Sensor stuck at <floor> cm" warning (defective hardware).
    cJSON_AddBoolToObject(state, "sensor_error", data.sensor_error);
    cJSON_AddBoolToObject(state, "sensor_stuck", data.sensor_stuck);

    // Power telemetry — same shape as /api/data so HA exposes the same sensors.
    cJSON *power = cJSON_AddObjectToObject(state, "power");
    const char *pmode_str = (data.power_mode == 'i') ? "ina219"
                          : (data.power_mode == 'v') ? "voltage"
                          : (data.power_mode == 'n') ? "none"
                          : "unknown";
    cJSON_AddStringToObject(power, "mode",       pmode_str);
    cJSON_AddNumberToObject(power, "current_ma", (double)data.current_ma);
    cJSON_AddNumberToObject(power, "power_mw",   (double)data.power_mw);
    cJSON_AddBoolToObject  (power, "charging",   data.charging);
    cJSON_AddNumberToObject(state, "battery_pct", data.battery_pct);

    cJSON *config = cJSON_AddObjectToObject(d, "config");
    cJSON_AddNumberToObject(config, "capacity_l",  info.capacity_liters);
    cJSON_AddNumberToObject(config, "min_dist_cm", info.min_dist_cm);
    cJSON_AddNumberToObject(config, "max_dist_cm", info.max_dist_cm);
    // sensor_kind sourced from the registry — falls back to empty string for
    // legacy entries that never had it queued (TX firmware then defaults SR04).
    cJSON_AddStringToObject(config, "sensor_kind", info.sensor_kind[0] ? info.sensor_kind : "sr04");
    cJSON_AddNumberToObject(config, "sleep_s",     info.sleep_s);
    cJSON_AddNumberToObject(config, "samples",     info.samples);
    cJSON_AddBoolToObject  (config, "pending_config", info.pending_config);
    return d;
}

// GET /api/v1/devices/<id> or GET /api/v1/devices/<id>/history
static esp_err_t handle_api_v1_device_get(httpd_req_t *req) {
    const char *suffix = NULL;
    uint16_t addr = parse_v1_device_id(req->uri, &suffix);
    if (addr == 0) { send_err(req, "Invalid id"); return ESP_OK; }

    int idx = -1;
    for (int i = 0; i < registry_count(); i++) {
        tx_info_t info;
        if (registry_get_info(i, &info) && info.address == addr) { idx = i; break; }
    }
    if (idx < 0) {
        httpd_resp_set_status(req, "404 Not Found");
        send_err(req, "Unknown device id");
        return ESP_OK;
    }

    if (suffix && strncmp(suffix, "history", 7) == 0) {
        // Per-device 48-slot half-hourly buffer.
        cJSON *root = cJSON_CreateObject();
        cJSON_AddNumberToObject(root, "slot_seconds", TX_HIST_SLOT_SECS);
        uint8_t pct[TX_HIST_SLOTS], volt_dv[TX_HIST_SLOTS];
        int64_t head_t = 0;
        bool has_hist = registry_get_history(idx, pct, volt_dv, &head_t);
        cJSON_AddNumberToObject(root, "head_t", (double)(has_hist ? head_t : 0));
        cJSON *samples = cJSON_AddArrayToObject(root, "samples");
        if (has_hist) {
            for (int s = 0; s < TX_HIST_SLOTS; s++) {
                if (pct[s] == TX_HIST_PCT_EMPTY) continue;
                cJSON *item = cJSON_CreateObject();
                int64_t slot_ts = head_t + (int64_t)s * TX_HIST_SLOT_SECS;
                cJSON_AddNumberToObject(item, "ts", (double)slot_ts);
                cJSON_AddNumberToObject(item, "level_pct", pct[s]);
                if (volt_dv[s] != TX_HIST_VOLT_EMPTY) {
                    cJSON_AddNumberToObject(item, "voltage", volt_dv[s] * 0.05);
                }
                cJSON_AddItemToArray(samples, item);
            }
        }
        char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root); send_json(req, json); free(json);
        return ESP_OK;
    }

    // Single-device default response.
    cJSON *d = build_v1_device_obj(idx);
    if (!d) { send_err(req, "Device unavailable"); return ESP_OK; }
    char *json = cJSON_PrintUnformatted(d); cJSON_Delete(d); send_json(req, json); free(json);
    return ESP_OK;
}

// PUT /api/v1/devices/<id> — partial update of editable fields.
//
// Hub-side fields that exist today: name, capacity_l, min_dist_cm, max_dist_cm,
// sleep_s, samples, lora_pwr. The existing config-sync MQTT path picks up the
// change on the next publish cycle so the cloud + PWA stay in sync.
//
// Forward-compat: extra fields (e.g. low_threshold_pct in protocol spec) are
// silently ignored. Adding NVS plumbing for them is queued for a later phase.
static esp_err_t handle_api_v1_device_put(httpd_req_t *req) {
    uint16_t addr = parse_v1_device_id(req->uri, NULL);
    if (addr == 0) { send_err(req, "Invalid id"); return ESP_OK; }

    char *body = read_body(req); if (!body) return ESP_OK;
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) { send_err(req, "Bad JSON"); return ESP_OK; }

    // Find current config so we only overwrite fields the client sent.
    tx_info_t info = {0};
    bool found = false;
    for (int i = 0; i < registry_count(); i++) {
        if (registry_get_info(i, &info) && info.address == addr) { found = true; break; }
    }
    if (!found) {
        cJSON_Delete(j);
        httpd_resp_set_status(req, "404 Not Found");
        send_err(req, "Unknown device id");
        return ESP_OK;
    }

    char name_buf[TX_NAME_MAX] = {0};
    strncpy(name_buf, info.name, TX_NAME_MAX - 1);
    int min_d   = info.min_dist_cm;
    int max_d   = info.max_dist_cm;
    float cap   = info.capacity_liters;
    uint32_t sleep_s = info.sleep_s;
    uint8_t  samples = info.samples;
    uint8_t  pwr     = 0;  // 0 = no override

    cJSON *f;
    if ((f = cJSON_GetObjectItem(j, "name")) && cJSON_IsString(f)) {
        strncpy(name_buf, f->valuestring, TX_NAME_MAX - 1);
        name_buf[TX_NAME_MAX - 1] = '\0';
    }
    if ((f = cJSON_GetObjectItem(j, "capacity_l")) && cJSON_IsNumber(f)) cap   = (float)f->valuedouble;
    if ((f = cJSON_GetObjectItem(j, "min_dist_cm")) && cJSON_IsNumber(f)) min_d = (int)f->valuedouble;
    if ((f = cJSON_GetObjectItem(j, "max_dist_cm")) && cJSON_IsNumber(f)) max_d = (int)f->valuedouble;
    if ((f = cJSON_GetObjectItem(j, "sleep_s"))     && cJSON_IsNumber(f)) sleep_s = (uint32_t)f->valuedouble;
    if ((f = cJSON_GetObjectItem(j, "samples"))     && cJSON_IsNumber(f)) samples = (uint8_t)f->valuedouble;
    if ((f = cJSON_GetObjectItem(j, "lora_pwr"))    && cJSON_IsNumber(f)) {
        int p = (int)f->valuedouble;
        if (p < 0) p = 0;
        if (p > 22) p = 22;
        pwr = (uint8_t)p;
    }
    cJSON_Delete(j);

    // Server-side validation matches handle_post_transmitters so HA / 3rd-party
    // clients on the v1 surface enforce the same invariants as the dashboard.
    if (min_d < 0 || max_d < 0 || min_d >= max_d) {
        send_err(req, "min_dist_cm must be less than max_dist_cm"); return ESP_OK;
    }
    if (samples > 0 && (samples < 3 || samples > 20)) {
        send_err(req, "samples must be 3-20"); return ESP_OK;
    }
    if (sleep_s > 0 && sleep_s < 60) {
        send_err(req, "sleep_s must be >= 60"); return ESP_OK;
    }

    bool ok = registry_update(addr, name_buf, min_d, max_d, cap);
    if (!ok) { send_err(req, "Update failed"); return ESP_OK; }

    if (sleep_s >= 60) {
        registry_set_remote_config(addr, sleep_s, samples > 0 ? samples : 5, pwr);
    }

    // Republish to MQTT immediately so the cloud DB picks up the change without
    // waiting for the next TANK packet (which can be many minutes away with
    // the new sleep_s applied). Mirrors handle_post_transmitters so HA-path
    // edits sync to the PWA at the same speed as legacy /api/transmitters edits.
    int idx = registry_find(addr);
    if (idx >= 0) mqtt_publish_tank(idx);

    // Return the updated device as the response body so clients can confirm.
    cJSON *d = idx >= 0 ? build_v1_device_obj(idx) : NULL;
    if (!d) { send_ok(req, "OK"); return ESP_OK; }
    char *json = cJSON_PrintUnformatted(d); cJSON_Delete(d); send_json(req, json); free(json);
    return ESP_OK;
}

static esp_err_t handle_api_system(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "version", FIRMWARE_VERSION);
    cJSON_AddStringToObject(root, "ip", wifi_manager_ip());
    cJSON_AddStringToObject(root, "ssid", wifi_manager_ssid());
    cJSON_AddStringToObject(root, "wifi_status", wifi_manager_status() == WIFI_ST_CONNECTED ? "connected" : "ap_mode");
    cJSON_AddStringToObject(root, "device_id", mqtt_manager_device_id());
    cJSON_AddBoolToObject  (root, "linked",    mqtt_manager_is_linked());
    cJSON_AddStringToObject(root, "mdns_host", wifi_manager_mdns_host());
    cJSON_AddNumberToObject(root, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));
    cJSON_AddNumberToObject(root, "free_heap", esp_get_free_heap_size());
    // Geo + time — populated by geo_time component on first boot.
    // country_code is "??" until ip-api.com lookup completes (or "IN" etc. cached).
    // lora_freq_mhz reports the radio's current frequency to let the UI compare
    // against the suggested band for the detected country.
    cJSON_AddStringToObject(root, "country_code", geo_get_country());
    cJSON_AddBoolToObject  (root, "time_synced",  geo_time_is_synced());
    // Expose registry capacity so the dashboard's "Devices N of M" strip
    // tracks the actual MAX_TRANSMITTERS rather than a hardcoded JS literal.
    cJSON_AddNumberToObject(root, "max_transmitters", MAX_TRANSMITTERS);
    {
        lora_config_t lcfg = {0};
        lora_get_config(&lcfg);
        cJSON_AddNumberToObject(root, "lora_freq_mhz", (double)lcfg.freq_hz / 1000000.0);
    }
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
        // Stable hardware identity (12-char lowercase hex). Zero-MAC entries
        // (legacy, paired before rx-v2.7.10) emit an empty string so PWA + HA
        // can detect "identity unknown" and degrade gracefully.
        {
            const uint8_t *m = info.mac;
            bool has_mac = false;
            for (int b = 0; b < 6; b++) if (m[b]) { has_mac = true; break; }
            if (has_mac) {
                char machex[13];
                snprintf(machex, sizeof(machex), "%02x%02x%02x%02x%02x%02x",
                         m[0], m[1], m[2], m[3], m[4], m[5]);
                cJSON_AddStringToObject(t, "mac", machex);
            } else {
                cJSON_AddStringToObject(t, "mac", "");
            }
        }
        cJSON_AddStringToObject(t, "name",       info.name);
        cJSON_AddNumberToObject(t, "min_dist",   info.min_dist_cm);
        cJSON_AddNumberToObject(t, "max_dist",   info.max_dist_cm);
        cJSON_AddNumberToObject(t, "capacity",   info.capacity_liters);
        cJSON_AddNumberToObject(t, "sleep",      info.sleep_s);
        cJSON_AddNumberToObject(t, "samples",    info.samples);
        cJSON_AddNumberToObject(t, "lora_pwr",   info.lora_pwr);
        // sensor_kind — what the RX last QUEUED for this tank via SET frame.
        // Empty string means no preference recorded (legacy entries / never
        // configured by the user). UI falls back to "sr04" in the dropdown.
        cJSON_AddStringToObject(t, "sensor_kind", info.sensor_kind);
        // active_sensor — what the TX is ACTUALLY running, reported by it in
        // the last TANK packet (since TX v2.0.15). Empty if the TX firmware
        // doesn't include the field. UI shows Active vs Queued for drift.
        cJSON_AddStringToObject(t, "active_sensor", data.active_sensor);
        cJSON_AddBoolToObject  (t, "pending_config", info.pending_config);
        cJSON_AddStringToObject(t, "state",      registry_state_str(data.state));
        cJSON_AddStringToObject(t, "fw_version", data.fw_version[0] ? data.fw_version : "unknown");
        cJSON_AddBoolToObject  (t, "ota_pending", ota_p);
        cJSON_AddNumberToObject(t, "ota_offset", ota_o);
        // Power telemetry (TX firmware v2.0.4+; '?' for older TX firmware)
        const char *pmode_str = (data.power_mode == 'i') ? "ina219"
                              : (data.power_mode == 'v') ? "voltage"
                              : (data.power_mode == 'n') ? "none"
                              : "unknown";
        cJSON_AddStringToObject(t, "power_mode", pmode_str);
        cJSON_AddNumberToObject(t, "current_ma", (double)data.current_ma);
        cJSON_AddNumberToObject(t, "power_mw",   (double)data.power_mw);
        cJSON_AddBoolToObject  (t, "charging",   data.charging);
        cJSON_AddItemToArray(arr, t);
    }
    char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root); send_json(req, json); free(json);
    return ESP_OK;
}

static esp_err_t handle_post_transmitters(httpd_req_t *req) {
    char *body = read_body(req); if (!body) return ESP_OK;
    cJSON *j = cJSON_Parse(body); free(body); if (!j) return ESP_OK;
    uint16_t addr = (uint16_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "addr"));
    // Copy name into a stack buffer BEFORE cJSON_Delete frees the JSON tree —
    // otherwise `name` is a dangling pointer and registry_update may read freed
    // heap. Observed 2026-04-28: posting name="pk" produced registry name="Y"
    // because the heap slot got partially overwritten before sanitize_name read it.
    const char *name_ptr = cJSON_GetStringValue(cJSON_GetObjectItem(j, "name"));
    char name_buf[TX_NAME_MAX] = {0};
    if (name_ptr) { strncpy(name_buf, name_ptr, TX_NAME_MAX - 1); }
    int min_dist = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "min_dist"));
    int max_dist = (int)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "max_dist"));
    float cap = (float)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "capacity"));
    uint32_t sleep = (uint32_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "sleep"));
    uint8_t samples = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "samples"));
    // Optional advanced setting: LoRa TX power 1-22 dBm. Absent or 0 = no override.
    cJSON *pwr_j = cJSON_GetObjectItem(j, "lora_pwr");
    uint8_t pwr = pwr_j ? (uint8_t)cJSON_GetNumberValue(pwr_j) : 0;
    if (pwr > 22) pwr = 22;   // RYLR998 max
    // Optional sensor_kind ("sr04" | "ld2413"). Absent = leave at whatever the
    // TX currently has. Validation happens in registry_set_sensor_kind below.
    const char *sensor_ptr = cJSON_GetStringValue(cJSON_GetObjectItem(j, "sensor_kind"));
    char sensor_buf[12] = {0};
    if (sensor_ptr) { strncpy(sensor_buf, sensor_ptr, sizeof(sensor_buf) - 1); }
    cJSON_Delete(j);
    const char *name = name_ptr ? name_buf : NULL;
    // Server-side validation — the JS UI checks these too, but a curl/HA/MQTT
    // caller would otherwise slip past and write invalid config that produces
    // garbage water_pct or silently un-applied sleep.
    if (addr == 0) { send_err(req, "Invalid addr"); return ESP_OK; }
    if (min_dist < 0 || max_dist < 0 || min_dist >= max_dist) {
        send_err(req, "Distance-when-full must be less than distance-when-empty"); return ESP_OK;
    }
    if (samples > 0 && (samples < 3 || samples > 20)) {
        send_err(req, "Samples must be 3-20"); return ESP_OK;
    }
    // Sleep is optional in the payload (we keep the existing value if absent),
    // but if the caller sent a value it must be >= 60. Previously sleep<60 was
    // silently dropped with no error.
    if (sleep > 0 && sleep < 60) {
        send_err(req, "Sleep must be >= 60 seconds"); return ESP_OK;
    }
    bool updated = registry_update(addr, name, min_dist, max_dist, cap);
    if (!updated) { if (registry_add(addr, name, min_dist, max_dist, cap) < 0) { send_err(req, "Full"); return ESP_OK; } }
    if (sleep >= 60) {
        registry_set_remote_config(addr, sleep, samples > 0 ? samples : 5, pwr);
    }
    // Sensor kind change goes through registry_set_sensor_kind so the next
    // SET frame to this TX includes :SENSOR=<kind>. Empty / absent string =
    // leave whatever the TX currently has. Invalid kind is rejected with a
    // 400-style error message so a typo doesn't silently no-op.
    if (sensor_buf[0]) {
        if (!registry_set_sensor_kind(addr, sensor_buf)) {
            send_err(req, "sensor_kind must be 'sr04' or 'ld2413'");
            return ESP_OK;
        }
    }
    // Immediately republish so the cloud DB picks up the change without waiting
    // for the next TANK packet (which can be 10 min away with sleep=600).
    int idx = registry_find(addr);
    if (idx >= 0) mqtt_publish_tank(idx);

    send_ok(req, sleep >= 60 ? "Saved — TX config queued" : "Saved");
    return ESP_OK;
}

static esp_err_t handle_delete_transmitter(httpd_req_t *req) {
    const char *last_slash = strrchr(req->uri, '/'); if (!last_slash) return ESP_OK;
    uint16_t addr = (uint16_t)atoi(last_slash + 1);
    // Clear the broker's retained per-tank topics BEFORE registry_remove so
    // the addr is still resolvable. mqtt_unpublish_tank() is a no-op when MQTT
    // is disconnected; safe to always call.
    mqtt_unpublish_tank(addr);
    if (registry_remove(addr)) {
        // Republish manifest so cloud reconciler prunes the row even if it
        // somehow missed the per-topic empty payloads above. Belt-and-braces.
        mqtt_publish_registry();
        send_ok(req, "OK");
    } else {
        send_err(req, "No");
    }
    return ESP_OK;
}

static esp_err_t handle_clear_transmitters(httpd_req_t *req) {
    // Cheap accident-guard: require an explicit ?confirm=1 query param so a
    // misfired script or stray curl can't wipe every paired tank in one POST.
    // The browser UI already confirms() — this just keeps non-UI calls honest.
    char qry[32] = {0}, buf[8] = {0};
    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) != ESP_OK ||
        httpd_query_key_value(qry, "confirm", buf, sizeof(buf)) != ESP_OK ||
        atoi(buf) != 1) {
        send_err(req, "Add ?confirm=1 to wipe all paired devices");
        return ESP_OK;
    }
    // Clear all retained per-tank topics before wiping the registry, then
    // republish the (now-empty) manifest so cloud subscribers reconcile.
    int n = registry_count();
    for (int i = 0; i < n; i++) {
        tx_info_t info;
        if (registry_get_info(i, &info)) mqtt_unpublish_tank(info.address);
    }
    registry_clear_all();
    mqtt_publish_registry();
    send_ok(req, "OK");
    return ESP_OK;
}

// Background task body — runs once after the HTTP response is sent so the
// browser sees "Wiping…" before the network goes down. Erases NVS (all
// namespaces: wifi creds, MQTT creds, LoRa NETID, system flags),
// transmitters.json + tx_hist.bin on SPIFFS, then reboots into clean AP mode.
static void factory_reset_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));   // let the HTTP response flush
    ESP_LOGW(TAG, "FACTORY RESET: wiping NVS + SPIFFS + restarting");
    // Wipe NVS (all namespaces in default partition)
    nvs_flash_erase();
    // Wipe SPIFFS files (transmitters, history, tombstones)
    remove("/spiffs/transmitters.json");
    remove("/spiffs/tx_hist.bin");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static esp_err_t handle_factory_reset(httpd_req_t *req) {
    // Strongest accident-guard in the entire web API: require BOTH a method
    // POST and an explicit ?confirm=YES_ERASE_EVERYTHING query string.
    // After this returns, the hub comes back in AP mode with no Wi-Fi creds,
    // no MQTT link, no paired TXs, no history, no tombstones. Use only for
    // resale or unrecoverable mis-config.
    char qry[64] = {0}, buf[32] = {0};
    if (httpd_req_get_url_query_str(req, qry, sizeof(qry)) != ESP_OK ||
        httpd_query_key_value(qry, "confirm", buf, sizeof(buf)) != ESP_OK ||
        strcmp(buf, "YES_ERASE_EVERYTHING") != 0) {
        send_err(req, "Add ?confirm=YES_ERASE_EVERYTHING to factory reset");
        return ESP_OK;
    }
    send_ok(req, "Factory reset starting — hub will reboot into setup mode in ~3s");
    xTaskCreate(factory_reset_task, "factory_reset", 4096, NULL, 5, NULL);
    return ESP_OK;
}

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
        bool active = false; uint16_t addr = 0; char name[TX_NAME_MAX] = {0}; int time_left = 0;
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
    size_t resp_len = strlen(json_arr) + 64;
    char *resp = malloc(resp_len);
    if (!resp) { free(json_arr); send_err(req, "OOM"); return ESP_OK; }
    snprintf(resp, resp_len, "{\"networks\":%s}", json_arr); free(json_arr);
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
    cJSON_AddStringToObject(root, "live_status", mqtt_live);
    char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root); send_json(req, json); free(json); return ESP_OK;
}

static esp_err_t handle_post_mqtt(httpd_req_t *req) {
    char *body = read_body(req); if (!body) return ESP_OK;
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) { send_err(req, "Bad JSON"); return ESP_OK; }
    // Start from current config so missing fields are preserved (not blanked).
    // Previously cfg was {0}, so an empty `pass` in the form wiped the stored
    // password — the UI's placeholder='(unchanged)' was lying.
    mqtt_mgr_config_t cfg;
    mqtt_manager_get_config(&cfg);
    const char *h = cJSON_GetStringValue(cJSON_GetObjectItem(j, "host"));
    const char *u = cJSON_GetStringValue(cJSON_GetObjectItem(j, "user"));
    const char *p = cJSON_GetStringValue(cJSON_GetObjectItem(j, "pass"));
    if (h) { strncpy(cfg.host, h, sizeof(cfg.host)-1); cfg.host[sizeof(cfg.host)-1] = '\0'; }
    if (u) { strncpy(cfg.user, u, sizeof(cfg.user)-1); cfg.user[sizeof(cfg.user)-1] = '\0'; }
    // Only overwrite pass if the client sent a non-empty value — matches the
    // UI placeholder semantics and prevents accidental credential loss.
    if (p && p[0]) { strncpy(cfg.pass, p, sizeof(cfg.pass)-1); cfg.pass[sizeof(cfg.pass)-1] = '\0'; }
    cJSON *port_j = cJSON_GetObjectItem(j, "port");
    if (port_j && cJSON_IsNumber(port_j)) {
        int pn = (int)port_j->valuedouble;
        if (pn < 1 || pn > 65535) { cJSON_Delete(j); send_err(req, "Port must be 1-65535"); return ESP_OK; }
        cfg.port = (uint16_t)pn;
    }
    if (cJSON_HasObjectItem(j, "enabled"))      cfg.enabled      = cJSON_IsTrue(cJSON_GetObjectItem(j, "enabled"));
    if (cJSON_HasObjectItem(j, "ha_discovery")) cfg.ha_discovery = cJSON_IsTrue(cJSON_GetObjectItem(j, "ha_discovery"));
    // If user enabled the broker, host can't be empty — refuse rather than save garbage.
    if (cfg.enabled && cfg.host[0] == '\0') {
        cJSON_Delete(j); send_err(req, "Host required when MQTT is enabled"); return ESP_OK;
    }
    cJSON_Delete(j); if (mqtt_manager_set_config(&cfg) == ESP_OK) { send_ok(req, "OK"); } else { send_err(req, "NO"); }
    return ESP_OK;
}

static esp_err_t handle_get_lora(httpd_req_t *req) {
    lora_config_t cfg; lora_get_config(&cfg);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "freq",   (double)cfg.freq_hz);
    cJSON_AddNumberToObject(root, "addr",   cfg.address);
    // Surface the per-pair-randomised NETID so support/debug can read it
    // without a serial console (was invisible before — see audit #25).
    cJSON_AddNumberToObject(root, "netid",  cfg.network_id);
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
    if ((v = cJSON_GetObjectItem(j, "freq")) && cJSON_IsNumber(v)) {
        // Reject frequencies outside the RYLR998's two supported bands.
        // Setting an invalid value used to brick the radio silently — no UI
        // feedback because the AT command was rejected after NVS write.
        uint64_t fhz = (uint64_t)v->valuedouble;
        bool ok = (fhz >= 410000000ULL && fhz <= 525000000ULL) ||
                  (fhz >= 820000000ULL && fhz <= 960000000ULL);
        if (!ok) { cJSON_Delete(j); send_err(req, "Frequency must be 410-525 or 820-960 MHz"); return ESP_OK; }
        cfg.freq_hz = (uint32_t)fhz;
    }
    if ((v = cJSON_GetObjectItem(j, "addr")) && cJSON_IsNumber(v)) {
        // Address 0 means "unpaired" in the LoRa stack — rejecting it here
        // prevents accidentally orphaning every paired TX with a single POST.
        int a = (int)v->valuedouble;
        if (a < 1 || a > 65535) { cJSON_Delete(j); send_err(req, "Address must be 1-65535"); return ESP_OK; }
        cfg.address = (uint16_t)a;
    }
    cJSON_Delete(j);
    if (lora_set_config(&cfg) == ESP_OK) { send_ok(req, "OK"); } else { send_err(req, "NO"); }
    return ESP_OK;
}

static esp_err_t handle_ota_state(httpd_req_t *req) {
    ota_state_t st; ota_manager_get_state(&st); static const char *sns[] = { "idle","checking","available","downloading","done","up_to_date","error" };
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", sns[st.status]);
    cJSON_AddNumberToObject(root, "progress", st.progress_pct);
    if (st.latest_version[0]) cJSON_AddStringToObject(root, "latest_version", st.latest_version);
    if (st.error_msg[0])      cJSON_AddStringToObject(root, "error_msg",      st.error_msg);
    char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root); send_json(req, json); free(json); return ESP_OK;
}
static esp_err_t handle_ota_check(httpd_req_t *req) {
    esp_err_t err = ota_manager_check_github();
    if (err == ESP_ERR_INVALID_STATE) {
        // Don't 4xx — surface as a friendly app-level error so the UI
        // can showToast("Please wait — download in progress")
        send_err(req, "Already busy — wait for current operation to finish");
    } else {
        send_ok(req, "OK");
    }
    return ESP_OK;
}
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

static esp_err_t handle_get_led(httpd_req_t *req) {
    uint8_t count = 2, bright = 50;
    nvs_handle_t h;
    if (nvs_open("system", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "led_count", &count);
        nvs_get_u8(h, "led_bright", &bright);
        nvs_close(h);
    }
    // Include per-tank color assignments + driver status (2.6.2)
    led_status_t st;
    led_get_status(&st);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "count", count);
    cJSON_AddNumberToObject(root, "brightness", bright);
    cJSON_AddBoolToObject  (root, "initialized",   st.initialized);
    cJSON_AddBoolToObject  (root, "strip_present", st.present);
    cJSON_AddNumberToObject(root, "fail_count",    st.fail_count);
    cJSON *cols = cJSON_AddArrayToObject(root, "tank_colors");
    int n = registry_count();
    for (int i = 0; i < n; i++) {
        tx_info_t info;
        if (registry_get_info(i, &info)) {
            cJSON *c = cJSON_CreateObject();
            cJSON_AddNumberToObject(c, "addr", info.address);
            cJSON_AddStringToObject(c, "name", info.name);
            cJSON_AddNumberToObject(c, "color_idx", info.led_color_idx);
            cJSON_AddItemToArray(cols, c);
        }
    }
    char *json = cJSON_PrintUnformatted(root); cJSON_Delete(root);
    send_json(req, json); free(json);
    return ESP_OK;
}

static esp_err_t handle_post_led(httpd_req_t *req) {
    char *body = read_body(req); if (!body) return ESP_OK;
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) { send_err(req, "Bad JSON"); return ESP_OK; }

    // 2.6.2: count + brightness now apply live via led_reinit() — no reboot.
    uint8_t new_count = 0, new_bright = 0;

    nvs_handle_t h;
    if (nvs_open("system", NVS_READWRITE, &h) == ESP_OK) {
        cJSON *v;
        if ((v = cJSON_GetObjectItem(j, "count"))) {
            uint8_t c = (uint8_t)v->valueint;
            if (c == 2 || c == 8 || c == 24) {
                nvs_set_u8(h, "led_count", c);
                new_count = c;
            }
        }
        if ((v = cJSON_GetObjectItem(j, "brightness"))) {
            uint8_t b = (uint8_t)v->valueint;
            if (b > 0) {
                nvs_set_u8(h, "led_bright", b);
                new_bright = b;
            }
        }
        nvs_commit(h);
        nvs_close(h);
    }

    // Per-tank color overrides (applied immediately via registry)
    cJSON *colors = cJSON_GetObjectItem(j, "tank_colors");
    if (colors && cJSON_IsArray(colors)) {
        cJSON *item;
        cJSON_ArrayForEach(item, colors) {
            uint16_t addr = (uint16_t)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "addr"));
            int8_t ci = (int8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(item, "color_idx"));
            if (addr > 0) registry_set_led_color(addr, ci);
        }
    }

    cJSON_Delete(j);

    // Apply live. Count change → full reinit (also picks up brightness atomically).
    // Brightness-only (slider path) → cheap led_set_brightness, no frame resize.
    if (new_count) {
        uint8_t bb = new_bright;
        if (!bb) {
            nvs_handle_t hr;
            if (nvs_open("system", NVS_READONLY, &hr) == ESP_OK) {
                nvs_get_u8(hr, "led_bright", &bb);
                nvs_close(hr);
            }
            if (!bb) bb = 50;
        }
        led_reinit(new_count, bb);
    } else if (new_bright) {
        led_set_brightness(new_bright);
    }

    send_ok(req, "Saved");
    return ESP_OK;
}

// Display mask: bit 0=water, bit 1=battery, bit 4=system. Bits 2,3 were
// 'signal' and 'diag' screens, retired 2026-05-17 — the OLED renderer no
// longer renders them so accepting those bits would silently do nothing.
#define DISPLAY_MASK_VALID  0x13   // 0b00010011
#define DISPLAY_MASK_DEFAULT 0x13  // water + battery + system

static esp_err_t handle_get_display(httpd_req_t *req) {
    uint8_t m = DISPLAY_MASK_DEFAULT;
    nvs_handle_t h;
    if (nvs_open("display", NVS_READONLY, &h) == ESP_OK) { nvs_get_u8(h, "mask", &m); nvs_close(h); }
    // Mask retired bits at read-time too so existing devices that stored 0x1F
    // (the old default) get a clean value back without needing a re-save.
    m &= DISPLAY_MASK_VALID;
    if (m == 0) m = DISPLAY_MASK_DEFAULT;  // never let the OLED go fully blank
    char buf[32]; sprintf(buf, "{\"mask\":%d}", m); send_json(req, buf); return ESP_OK;
}
static esp_err_t handle_post_display(httpd_req_t *req) {
    char *body = read_body(req); if (!body) { send_err(req, "No body"); return ESP_OK; }
    cJSON *j = cJSON_Parse(body); free(body); if (!j) { send_err(req, "Bad JSON"); return ESP_OK; }
    uint8_t m = (uint8_t)cJSON_GetNumberValue(cJSON_GetObjectItem(j, "mask"));
    cJSON_Delete(j);
    // Reject unknown bits rather than silently dropping them — surfaces typos.
    if (m & ~DISPLAY_MASK_VALID) { send_err(req, "Mask uses unknown bits"); return ESP_OK; }
    if (m == 0) { send_err(req, "Select at least one screen"); return ESP_OK; }
    nvs_handle_t h;
    if (nvs_open("display", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "mask", m); nvs_commit(h); nvs_close(h); }
    send_ok(req, "OK"); return ESP_OK;
}

// ─── Timezone (POSIX TZ) — used by buzzer quiet-hours + future local-time UI ──
static esp_err_t handle_get_tz(httpd_req_t *req) {
    char tz[64];
    geo_time_get_tz(tz, sizeof(tz));
    const char *cc = geo_get_country();
    const char *suggested = geo_suggested_tz(cc);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "tz", tz);
    cJSON_AddStringToObject(j, "country", cc);
    cJSON_AddStringToObject(j, "suggested", suggested ? suggested : "");
    cJSON_AddBoolToObject(j, "synced", geo_time_is_synced());
    char *s = cJSON_PrintUnformatted(j);
    send_json(req, s); free(s); cJSON_Delete(j);
    return ESP_OK;
}

static esp_err_t handle_post_tz(httpd_req_t *req) {
    char *body = read_body(req); if (!body) { send_err(req, "No body"); return ESP_OK; }
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) { send_err(req, "Bad JSON"); return ESP_OK; }
    const char *tz = cJSON_GetStringValue(cJSON_GetObjectItem(j, "tz"));
    if (!tz) tz = "";   // empty string = clear override + fall back to CC default
    esp_err_t err = geo_time_set_tz(tz);
    cJSON_Delete(j);
    if (err != ESP_OK) { send_err(req, "Failed to set TZ"); return ESP_OK; }
    send_ok(req, "OK");
    return ESP_OK;
}

// ─── Buzzer config + test ─────────────────────────────────────────────────────
static esp_err_t handle_get_buzzer(httpd_req_t *req) {
    buzzer_config_t cfg;
    buzzer_get_config(&cfg);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "master_enable", cfg.master_enable);
    cJSON_AddNumberToObject(j, "master_profile", cfg.master_profile);
    cJSON_AddNumberToObject(j, "quiet_start_hour", cfg.quiet_start_hour);
    cJSON_AddNumberToObject(j, "quiet_end_hour", cfg.quiet_end_hour);
    cJSON_AddBoolToObject(j, "critical_overrides_quiet", cfg.critical_overrides_quiet);
    cJSON *arr = cJSON_AddArrayToObject(j, "alerts");
    for (int i = 0; i < BUZZ__COUNT; i++) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddNumberToObject(a, "event", i);
        cJSON_AddStringToObject(a, "name", buzzer_event_label((buzzer_event_t)i));
        cJSON_AddBoolToObject(a, "enabled", cfg.alert_enable[i]);
        int tier = (i <= BUZZ_TEST_BUTTON) ? 1
                 : (i <= BUZZ_OTA_FAILURE) ? 2
                 : 3;
        cJSON_AddNumberToObject(a, "tier", tier);
        cJSON_AddItemToArray(arr, a);
    }
    char *s = cJSON_PrintUnformatted(j);
    send_json(req, s); free(s); cJSON_Delete(j);
    return ESP_OK;
}

static esp_err_t handle_post_buzzer(httpd_req_t *req) {
    char *body = read_body(req); if (!body) { send_err(req, "No body"); return ESP_OK; }
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) { send_err(req, "Bad JSON"); return ESP_OK; }

    // Start from current config so partial updates are well-defined.
    buzzer_config_t cfg;
    buzzer_get_config(&cfg);

    cJSON *m = cJSON_GetObjectItem(j, "master_enable");
    if (cJSON_IsBool(m)) cfg.master_enable = cJSON_IsTrue(m);
    cJSON *mp = cJSON_GetObjectItem(j, "master_profile");
    if (cJSON_IsNumber(mp)) cfg.master_profile = (uint8_t)mp->valueint;
    cJSON *qs = cJSON_GetObjectItem(j, "quiet_start_hour");
    if (cJSON_IsNumber(qs)) cfg.quiet_start_hour = (uint8_t)qs->valueint;
    cJSON *qe = cJSON_GetObjectItem(j, "quiet_end_hour");
    if (cJSON_IsNumber(qe)) cfg.quiet_end_hour = (uint8_t)qe->valueint;
    cJSON *co = cJSON_GetObjectItem(j, "critical_overrides_quiet");
    if (cJSON_IsBool(co)) cfg.critical_overrides_quiet = cJSON_IsTrue(co);

    cJSON *arr = cJSON_GetObjectItem(j, "alerts");
    if (cJSON_IsArray(arr)) {
        cJSON *item;
        cJSON_ArrayForEach(item, arr) {
            cJSON *ev = cJSON_GetObjectItem(item, "event");
            if (!cJSON_IsNumber(ev)) continue;
            int idx = ev->valueint;
            if (idx < 0 || idx >= BUZZ__COUNT) continue;
            cJSON *en = cJSON_GetObjectItem(item, "enabled");
            if (cJSON_IsBool(en)) cfg.alert_enable[idx] = cJSON_IsTrue(en);
        }
    }
    cJSON_Delete(j);

    esp_err_t err = buzzer_set_config(&cfg);
    if (err != ESP_OK) { send_err(req, "Failed to persist"); return ESP_OK; }
    send_ok(req, "OK");
    return ESP_OK;
}

static esp_err_t handle_post_buzzer_test(httpd_req_t *req) {
    char *body = read_body(req); if (!body) { send_err(req, "No body"); return ESP_OK; }
    cJSON *j = cJSON_Parse(body); free(body);
    if (!j) { send_err(req, "Bad JSON"); return ESP_OK; }
    int event = 4;   // BUZZ_TEST_BUTTON
    int profile = -1;
    cJSON *ev = cJSON_GetObjectItem(j, "event");
    if (cJSON_IsNumber(ev)) event = ev->valueint;
    cJSON *pr = cJSON_GetObjectItem(j, "profile");
    if (cJSON_IsNumber(pr)) profile = pr->valueint;
    cJSON_Delete(j);
    if (event < 0 || event >= BUZZ__COUNT) { send_err(req, "Bad event"); return ESP_OK; }
    buzzer_profile_t prof_override = (profile >= 0 && profile <= BUZZ_PROFILE_LOUD)
                                    ? (buzzer_profile_t)profile : 0xff;
    buzzer_test((buzzer_event_t)event, prof_override);
    send_ok(req, "OK");
    return ESP_OK;
}

// ─── Phase 1.4 — identify + reboot ────────────────────────────────────────────
//
// Identify endpoints blink a target LED briefly so the user can find a hub
// or tank physically. Reboot endpoint cleanly restarts the hub. Both are
// fire-and-forget — endpoint returns 200 immediately, the action runs in
// a separate FreeRTOS task or after a small delay so the HTTP response
// flushes first.

static void reboot_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));   // let HTTP response flush
    esp_restart();
}

static esp_err_t handle_api_v1_hub_identify(httpd_req_t *req) {
    led_identify(LED_IDX_STATUS);
    send_ok(req, "Identifying");
    return ESP_OK;
}

static esp_err_t handle_api_v1_hub_reboot(httpd_req_t *req) {
    send_ok(req, "Rebooting");
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

// POST /api/v1/devices/<id>/identify — blink the target tank's LED.
// (Single POST handler at /api/v1/devices/* dispatches by URI suffix so we
// can add other per-device actions later without burning extra route slots.)
static esp_err_t handle_api_v1_device_post(httpd_req_t *req) {
    const char *suffix = NULL;
    uint16_t addr = parse_v1_device_id(req->uri, &suffix);
    if (addr == 0) { send_err(req, "Invalid id"); return ESP_OK; }

    if (!suffix || strncmp(suffix, "identify", 8) != 0) {
        httpd_resp_set_status(req, "404 Not Found");
        send_err(req, "Unknown device action");
        return ESP_OK;
    }

    // Find the registry index so we can map address → LED slot.
    int idx = -1;
    for (int i = 0; i < registry_count(); i++) {
        tx_info_t info;
        if (registry_get_info(i, &info) && info.address == addr) { idx = i; break; }
    }
    if (idx < 0) {
        httpd_resp_set_status(req, "404 Not Found");
        send_err(req, "Unknown device id");
        return ESP_OK;
    }

    // LED layout: [0]=STATUS, [1]=LORA, [2..N]=tanks. If the configured strip
    // has fewer LEDs than (LED_IDX_TANK_START + idx), there's no physical LED
    // to blink — surface that explicitly so the UI can tell the user they need
    // an 8+ LED strip for per-tank identify (instead of toasting "blinking"
    // when nothing actually blinks).
    uint8_t led_count = 2;  // default
    nvs_handle_t h;
    if (nvs_open("system", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "led_count", &led_count);
        nvs_close(h);
    }
    int slot = LED_IDX_TANK_START + idx;
    if (slot >= led_count) {
        send_err(req, "Connect an 8+ LED strip to use per-tank identify");
        return ESP_OK;
    }
    led_identify(slot);
    send_ok(req, "Identifying");
    return ESP_OK;
}

// ─── WebSocket /api/v1/stream — real-time push to HA + 3rd-party clients ──────
//
// Each connected client gets a `hello` frame on connect, then a `snapshot`
// frame every WS_TICK_MS containing the same state HA polled previously.
// Frames are JSON; clients ignore unknown `kind` values for forward-compat.
//
// Bounded to MAX_WS_CLIENTS simultaneous clients (typical home runs 1 HA).
// FDs that fail an async send are auto-cleared from the slot table.

#define MAX_WS_CLIENTS  4
#define WS_TICK_MS      3000

static int               s_ws_fds[MAX_WS_CLIENTS];
static SemaphoreHandle_t s_ws_lock  = NULL;
static esp_timer_handle_t s_ws_timer = NULL;

static void ws_send_to_fd(int fd, const char *json) {
    if (!s_server || fd < 0) return;
    httpd_ws_frame_t pkt = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len     = strlen(json),
    };
    httpd_ws_send_frame_async(s_server, fd, &pkt);
}

static void ws_broadcast(const char *json) {
    if (!s_server) return;
    httpd_ws_frame_t pkt = {
        .type    = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)json,
        .len     = strlen(json),
    };
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] != -1) {
            esp_err_t r = httpd_ws_send_frame_async(s_server, s_ws_fds[i], &pkt);
            if (r != ESP_OK) {
                ESP_LOGI(TAG, "WS slot %d (fd=%d) closing: %s", i, s_ws_fds[i], esp_err_to_name(r));
                s_ws_fds[i] = -1;
            }
        }
    }
    xSemaphoreGive(s_ws_lock);
}

// Build the same envelope as { kind:"snapshot", hub:{...}, devices:[...] }.
// Caller frees the returned string.
static char *ws_build_snapshot_json(void) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "kind", "snapshot");
    cJSON *hub = cJSON_AddObjectToObject(root, "hub");
    cJSON_AddNumberToObject(hub, "uptime_s", (double)(esp_timer_get_time() / 1000000LL));
    wifi_status_t ws = wifi_manager_status();
    cJSON_AddNumberToObject(hub, "wifi_rssi", ws == WIFI_ST_CONNECTED ? wifi_manager_rssi() : 0);
    ota_state_t ota = {0};
    ota_manager_get_state(&ota);
    cJSON_AddBoolToObject(hub, "ota_available",
                          ota.status == OTA_ST_AVAILABLE && ota.latest_version[0]);
    cJSON *devs = cJSON_AddArrayToObject(root, "devices");
    for (int i = 0; i < registry_count(); i++) {
        cJSON *d = build_v1_device_obj(i);
        if (d) cJSON_AddItemToArray(devs, d);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static void ws_tick_cb(void *arg) {
    (void)arg;
    // Skip the broadcast if there are no clients — saves rendering JSON.
    bool any = false;
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) if (s_ws_fds[i] != -1) { any = true; break; }
    xSemaphoreGive(s_ws_lock);
    if (!any) return;

    char *json = ws_build_snapshot_json();
    if (!json) return;
    ws_broadcast(json);
    free(json);
}

static esp_err_t handle_api_v1_stream(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        // WebSocket handshake just completed. Capture the FD so the periodic
        // broadcaster can find this client later.
        int fd = httpd_req_to_sockfd(req);
        if (fd < 0) {
            ESP_LOGW(TAG, "WS req has no sockfd");
            return ESP_FAIL;
        }
        bool added = false;
        xSemaphoreTake(s_ws_lock, portMAX_DELAY);
        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            if (s_ws_fds[i] == -1) {
                s_ws_fds[i] = fd;
                added = true;
                ESP_LOGI(TAG, "WS client connected: fd=%d (slot %d)", fd, i);
                break;
            }
        }
        xSemaphoreGive(s_ws_lock);
        if (!added) {
            ESP_LOGW(TAG, "WS clients full, dropping fd=%d", fd);
            return ESP_FAIL;
        }
        // Send hello frame so the client can validate schema_version.
        cJSON *hello = cJSON_CreateObject();
        cJSON_AddStringToObject(hello, "kind",           "hello");
        cJSON_AddStringToObject(hello, "schema_version", "1.0");
        cJSON_AddStringToObject(hello, "hub_id",         wifi_manager_hub_id());
        cJSON_AddStringToObject(hello, "fw_version",     FIRMWARE_VERSION);
        char *json = cJSON_PrintUnformatted(hello);
        cJSON_Delete(hello);
        ws_send_to_fd(fd, json);
        free(json);

        // Also send an initial snapshot so the client doesn't have to wait
        // a full WS_TICK_MS before populating its state.
        char *snap = ws_build_snapshot_json();
        if (snap) {
            ws_send_to_fd(fd, snap);
            free(snap);
        }
        return ESP_OK;
    }
    // We don't currently process inbound frames (clients are expected to
    // listen only). Drain any frame the client sent so the request handler
    // returns cleanly.
    httpd_ws_frame_t in = {0};
    uint8_t buf[16];
    in.type    = HTTPD_WS_TYPE_TEXT;
    in.payload = buf;
    httpd_ws_recv_frame(req, &in, sizeof(buf));
    return ESP_OK;
}

// Called from web_server_start; safe to call once.
static void ws_init(void) {
    for (int i = 0; i < MAX_WS_CLIENTS; i++) s_ws_fds[i] = -1;
    if (!s_ws_lock) s_ws_lock = xSemaphoreCreateMutex();
    if (!s_ws_timer) {
        const esp_timer_create_args_t args = {
            .callback        = &ws_tick_cb,
            .name            = "ws-tick",
            .dispatch_method = ESP_TIMER_TASK,
        };
        if (esp_timer_create(&args, &s_ws_timer) == ESP_OK) {
            esp_timer_start_periodic(s_ws_timer, (uint64_t)WS_TICK_MS * 1000);
        }
    }
}

// ── GET /api/logs?since=N ────────────────────────────────────────────────────
// Returns plain-text log lines accumulated in the in-RAM ring buffer since
// the byte cursor N. Same shape as the TX wifi_ota /api/logs endpoint so a
// caller (curl, dashboard JS, debugging script) can use the same client.
static esp_err_t handle_get_logs(httpd_req_t *req) {
    size_t cursor = 0;
    char qbuf[40];
    if (httpd_req_get_url_query_str(req, qbuf, sizeof(qbuf)) == ESP_OK) {
        char val[20] = {0};
        if (httpd_query_key_value(qbuf, "since", val, sizeof(val)) == ESP_OK) {
            cursor = (size_t)strtoul(val, NULL, 10);
        }
    }
    static char out[3000];
    size_t n = log_buffer_read(out, sizeof(out), &cursor);
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "%u", (unsigned)cursor);
    httpd_resp_set_hdr(req, "X-Log-Cursor", hdr);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return httpd_resp_send(req, out, n);
}

#define URI(m, p, h) { .uri = (p), .method = (m), .handler = (h), .user_ctx = NULL }
static const httpd_uri_t s_routes[] = {
    URI(HTTP_GET, "/api/logs", handle_get_logs),
    URI(HTTP_GET, "/", handle_root), URI(HTTP_GET, "/api/data", handle_api_data), URI(HTTP_GET, "/api/system", handle_api_system),
    URI(HTTP_GET, "/api/tanks/history", handle_api_tanks_history),
    // SmartGhar protocol v1 — local LAN API for HA + 3rd-party clients
    URI(HTTP_GET, "/api/v1/info",    handle_api_v1_info),
    URI(HTTP_GET, "/api/v1/devices", handle_api_v1_devices),
    URI(HTTP_GET, "/api/v1/devices/*", handle_api_v1_device_get),  // /<id> and /<id>/history
    URI(HTTP_PUT, "/api/v1/devices/*", handle_api_v1_device_put),
    // Hub-scoped aliases — re-use the existing handlers under the v1 namespace
    // so HA + 3rd-party clients only need to know /api/v1/* paths.
    URI(HTTP_GET, "/api/v1/hub/led",         handle_get_led),
    URI(HTTP_PUT, "/api/v1/hub/led",         handle_post_led),
    URI(HTTP_GET, "/api/v1/hub/display",     handle_get_display),
    URI(HTTP_PUT, "/api/v1/hub/display",     handle_post_display),
    URI(HTTP_POST, "/api/v1/hub/ota/check",  handle_ota_check),
    URI(HTTP_POST, "/api/v1/hub/wifi/forget", handle_wifi_forget),
    // Phase 1.4: identify + reboot
    URI(HTTP_POST, "/api/v1/hub/identify",   handle_api_v1_hub_identify),
    URI(HTTP_POST, "/api/v1/hub/reboot",     handle_api_v1_hub_reboot),
    URI(HTTP_POST, "/api/v1/devices/*",      handle_api_v1_device_post),
    // WebSocket — real-time push to HA + 3rd-party clients
    { .uri = "/api/v1/stream", .method = HTTP_GET, .handler = handle_api_v1_stream, .user_ctx = NULL, .is_websocket = true },
    URI(HTTP_GET, "/api/transmitters", handle_get_transmitters), URI(HTTP_POST, "/api/transmitters", handle_post_transmitters),
    URI(HTTP_POST, "/api/transmitters/clear", handle_clear_transmitters), URI(HTTP_DELETE, "/api/transmitters/*", handle_delete_transmitter),
    URI(HTTP_POST, "/api/factory_reset", handle_factory_reset),
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
    URI(HTTP_GET, "/api/led", handle_get_led), URI(HTTP_POST, "/api/led", handle_post_led),
    // Timezone (POSIX TZ) — used by buzzer quiet-hours + future local-time UI
    URI(HTTP_GET,  "/api/tz",            handle_get_tz),
    URI(HTTP_POST, "/api/tz",            handle_post_tz),
    // Buzzer config + test
    URI(HTTP_GET,  "/api/buzzer",        handle_get_buzzer),
    URI(HTTP_POST, "/api/buzzer",        handle_post_buzzer),
    URI(HTTP_POST, "/api/buzzer/test",   handle_post_buzzer_test),
    // HACS-friendly aliases (mirrors /api/v1/hub/led + display pattern).
    // Same handlers, just the SmartGhar-Protocol v1 path namespace so the
    // HA integration can read/write buzzer config the same way it does LED.
    URI(HTTP_GET,  "/api/v1/hub/buzzer",      handle_get_buzzer),
    URI(HTTP_PUT,  "/api/v1/hub/buzzer",      handle_post_buzzer),
    URI(HTTP_POST, "/api/v1/hub/buzzer/test", handle_post_buzzer_test),
};

esp_err_t web_server_start(void) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port        = WEB_PORT;
    cfg.max_uri_handlers   = 60;         // 2.6.2: 30→40 (POST /api/led silently dropped). 2.7.0 Phase 1.1: 40→50. 2.7.0 Phase 1.2: 50→60 (full /api/v1/* surface). Headroom for Phase 1.3 WebSocket + ecosystem device_kinds.
    cfg.uri_match_fn       = httpd_uri_match_wildcard;
    cfg.max_open_sockets   = 6;          // leave 4+ lwIP sockets for MQTT/DNS/NTP
    cfg.recv_wait_timeout  = 15;         // balance: short enough to free sockets, long enough for TX firmware upload
    cfg.send_wait_timeout  = 5;
    cfg.lru_purge_enable   = true;       // auto-close oldest connection when sockets full
    esp_err_t err = httpd_start(&s_server, &cfg); if (err != ESP_OK) return err;
    int total = (int)(sizeof(s_routes)/sizeof(s_routes[0]));
    int registered = 0;
    for (int i = 0; i < total; i++) {
        esp_err_t r = httpd_register_uri_handler(s_server, &s_routes[i]);
        if (r == ESP_OK) registered++;
        else ESP_LOGE(TAG, "Failed to register %s %s: %s",
                      s_routes[i].method == HTTP_GET ? "GET" :
                      s_routes[i].method == HTTP_POST ? "POST" :
                      s_routes[i].method == HTTP_DELETE ? "DELETE" : "?",
                      s_routes[i].uri, esp_err_to_name(r));
    }
    ESP_LOGI(TAG, "Server started — %d/%d URI handlers registered (cap=%d)",
             registered, total, (int)cfg.max_uri_handlers);
    ws_init();
    return ESP_OK;
}
void web_server_stop(void) { if (s_server) { httpd_stop(s_server); s_server = NULL; } }
