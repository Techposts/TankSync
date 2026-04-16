// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

import db, { stmts } from './db.js';
import { broadcastToUser } from './sse.js';
import webpush from 'web-push';
import { sendPush } from './push-sender.js';

/**
 * Comprehensive alert system for water tank monitoring.
 *
 * Alert types and triggers:
 * ─────────────────────────────────────────────────────────────────
 * CRITICAL (immediate push notification):
 *   - low_water        Tank below low threshold (default 20%)
 *   - tank_empty       Tank at 0% — completely dry
 *   - device_offline   No data received for 15+ minutes
 *
 * WARNING (push + in-app):
 *   - high_water       Tank above high threshold (default 95%) — possible overflow
 *   - low_battery      Transmitter battery below 15%
 *   - battery_critical Transmitter battery below 5% — will die soon
 *   - weak_signal      LoRa RSSI below -100 dBm — unreliable connection
 *   - rapid_drop       Water level dropped 20%+ in 1 hour — possible leak
 *   - rapid_rise       Water level rose 30%+ in 30 min — filling event
 *
 * INFO (in-app only):
 *   - device_online    Device came back online after being offline
 *   - device_stale     No data for 10 minutes (not yet offline)
 *   - battery_ok       Battery recovered above 25% (e.g. solar recharge)
 *   - level_recovered  Water level back above low threshold
 * ─────────────────────────────────────────────────────────────────
 *
 * All alerts use hysteresis to prevent flapping:
 *   e.g., low_water triggers at 20%, clears at 25% (5% buffer)
 */

// Per-device alert state (in-memory, survives across MQTT messages)
const alertState = new Map();

// VAPID keys for web push
const VAPID_PUBLIC = process.env.VAPID_PUBLIC_KEY || '';
const VAPID_PRIVATE = process.env.VAPID_PRIVATE_KEY || '';

// VAPID is configured in push-sender.js — not needed here
// webpush import kept only for legacy reference; sendPush handles delivery

function getState(deviceId) {
  if (!alertState.has(deviceId)) {
    alertState.set(deviceId, {
      low_water: false,
      tank_empty: false,
      high_water: false,
      low_battery: false,
      battery_critical: false,
      weak_signal: false,
      device_offline: false,
      device_stale: false,
      rapid_drop: false,
      // Track recent readings for rate-of-change detection
      recentReadings: [],     // { pct, timestamp }
      lastOnlineTime: null,
      wasOffline: false,
    });
  }
  return alertState.get(deviceId);
}

export function checkAlerts(device, site) {
  const state = getState(device.id);
  const pct = device.last_water_pct;
  const bat = device.last_battery_pct;
  const rssi = device.last_rssi;
  const now = Date.now();

  // Track readings for rate-of-change analysis
  if (pct != null) {
    state.recentReadings.push({ pct, timestamp: now });
    // Keep only last 2 hours of readings
    const twoHoursAgo = now - 2 * 3600_000;
    state.recentReadings = state.recentReadings.filter(r => r.timestamp > twoHoursAgo);
  }

  // ── WATER LEVEL ALERTS ──────────────────────────────────────────

  // Tank empty (0%)
  if (pct != null && pct <= 0 && !state.tank_empty) {
    state.tank_empty = true;
    createAlert(device, site, 'tank_empty',
      `${device.name} is completely empty! 0% water remaining.`, 'critical');
  } else if (pct > 5) {
    state.tank_empty = false;
  }

  // Low water (below threshold)
  if (pct != null && pct > 0 && pct <= device.alert_low_pct && !state.low_water) {
    state.low_water = true;
    createAlert(device, site, 'low_water',
      `${device.name} water level low: ${Math.round(pct)}% (threshold: ${device.alert_low_pct}%)`, 'critical');
  } else if (pct > device.alert_low_pct + 5) {
    // Hysteresis: only clear when 5% above threshold
    if (state.low_water) {
      state.low_water = false;
      createAlert(device, site, 'level_recovered',
        `${device.name} water level recovered to ${Math.round(pct)}%`, 'info');
    }
  }

  // High water / overflow risk
  if (pct != null && pct >= device.alert_high_pct && !state.high_water) {
    state.high_water = true;
    createAlert(device, site, 'high_water',
      `${device.name} water level high: ${Math.round(pct)}% — possible overflow risk`, 'warning');
  } else if (pct < device.alert_high_pct - 5) {
    state.high_water = false;
  }

  // Rapid drop detection (possible leak)
  if (state.recentReadings.length >= 3) {
    const oneHourAgo = now - 3600_000;
    const hourReadings = state.recentReadings.filter(r => r.timestamp > oneHourAgo);
    if (hourReadings.length >= 2) {
      const oldest = hourReadings[0].pct;
      const newest = hourReadings[hourReadings.length - 1].pct;
      const drop = oldest - newest;
      if (drop >= 20 && !state.rapid_drop) {
        state.rapid_drop = true;
        createAlert(device, site, 'rapid_drop',
          `${device.name} dropped ${Math.round(drop)}% in the last hour — possible leak or heavy usage`, 'warning');
      } else if (drop < 10) {
        state.rapid_drop = false;
      }
    }
  }

  // Rapid rise detection (tank filling)
  if (state.recentReadings.length >= 2) {
    const thirtyMinAgo = now - 1800_000;
    const recentR = state.recentReadings.filter(r => r.timestamp > thirtyMinAgo);
    if (recentR.length >= 2) {
      const oldest = recentR[0].pct;
      const newest = recentR[recentR.length - 1].pct;
      const rise = newest - oldest;
      if (rise >= 30) {
        // Info-level: tank is filling — no action needed, just notify
        createAlert(device, site, 'rapid_rise',
          `${device.name} filling rapidly: +${Math.round(rise)}% in ${Math.round((now - recentR[0].timestamp) / 60000)}min`, 'info');
        // Remove oldest readings to avoid re-triggering
        state.recentReadings = recentR.slice(-2);
      }
    }
  }

  // ── BATTERY ALERTS ──────────────────────────────────────────────

  // Battery critical (< 5%)
  if (bat != null && bat <= 5 && !state.battery_critical) {
    state.battery_critical = true;
    createAlert(device, site, 'battery_critical',
      `${device.name} battery critically low: ${Math.round(bat)}% — device may shut down soon`, 'critical');
  } else if (bat > 15) {
    state.battery_critical = false;
  }

  // Low battery (< 15%)
  if (bat != null && bat > 5 && bat <= 15 && !state.low_battery) {
    state.low_battery = true;
    createAlert(device, site, 'low_battery',
      `${device.name} battery low: ${Math.round(bat)}%`, 'warning');
  } else if (bat > 25) {
    if (state.low_battery) {
      state.low_battery = false;
      createAlert(device, site, 'battery_ok',
        `${device.name} battery recovered to ${Math.round(bat)}%`, 'info');
    }
  }

  // ── SIGNAL ALERTS ───────────────────────────────────────────────

  if (rssi != null && rssi < -100 && !state.weak_signal) {
    state.weak_signal = true;
    createAlert(device, site, 'weak_signal',
      `${device.name} LoRa signal very weak: ${rssi} dBm — may lose connection`, 'warning');
  } else if (rssi != null && rssi > -90) {
    state.weak_signal = false;
  }

  // ── ONLINE/OFFLINE TRACKING ─────────────────────────────────────

  // Device came back online after being offline
  if (state.wasOffline && device.state === 'online') {
    state.wasOffline = false;
    state.device_offline = false;
    state.device_stale = false;
    createAlert(device, site, 'device_online',
      `${device.name} is back online`, 'info');
  }

  state.lastOnlineTime = now;
}

/**
 * Called periodically (every 60s from server) to check for stale/offline devices
 */
export function checkDeviceTimeouts() {
  const allDevices = stmts.getAllDevicesWithSites.all();
  const now = Date.now();

  for (const device of allDevices) {
    if (!device.last_seen) continue;
    const state = getState(device.id);
    const lastSeen = new Date(device.last_seen + 'Z').getTime();
    const ageSec = (now - lastSeen) / 1000;

    // Get site info for alert
    const site = { id: device.site_id, name: device.site_name, user_id: device.user_id };

    // Stale: no data for 10+ minutes
    if (ageSec > 600 && ageSec <= 900 && !state.device_stale) {
      state.device_stale = true;
      db.prepare('UPDATE devices SET state = ? WHERE id = ?').run('stale', device.id);
      createAlert(device, site, 'device_stale',
        `${device.name} hasn't reported in ${Math.round(ageSec / 60)} minutes`, 'info');
    }

    // Offline: no data for 15+ minutes
    if (ageSec > 900 && !state.device_offline) {
      state.device_offline = true;
      state.wasOffline = true;
      db.prepare('UPDATE devices SET state = ? WHERE id = ?').run('offline', device.id);
      createAlert(device, site, 'device_offline',
        `${device.name} is offline — no data for ${Math.round(ageSec / 60)} minutes`, 'critical');
    }
  }
}

/**
 * Smart alert creation with escalating reminders and ack-based suppression.
 *
 * Strategy:
 * ─────────────────────────────────────────────────────────────────
 * FIRST OCCURRENCE:
 *   - Create in-app alert immediately
 *   - Send push notification (if critical/warning)
 *
 * CONDITION PERSISTS (not acknowledged):
 *   CRITICAL: remind at 1hr, 4hr, 12hr, then daily
 *   WARNING:  remind at 4hr, then daily
 *   INFO:     no reminders (one-time in-app only)
 *
 * USER ACKNOWLEDGES:
 *   - Suppress all reminders for that type+device
 *   - Don't re-alert until condition CLEARS and RE-TRIGGERS
 *
 * CONDITION CLEARS:
 *   - Reset suppression so next occurrence triggers fresh
 * ─────────────────────────────────────────────────────────────────
 */

// Escalation intervals in minutes
const ESCALATION = {
  critical: [0, 60, 240, 720, 1440], // immediate, 1hr, 4hr, 12hr, daily
  warning:  [0, 240, 1440],           // immediate, 4hr, daily
  info:     [0],                       // one-time only
};

function createAlert(device, site, type, message, severity = 'info') {
  const intervals = ESCALATION[severity] || ESCALATION.info;

  // Check: was this type+device acknowledged recently?
  // If acked and condition hasn't cleared (hysteresis handles clearing), suppress entirely
  const lastAcked = db.prepare(`
    SELECT id FROM alerts
    WHERE device_id = ? AND type = ? AND acknowledged = 1
    ORDER BY created_at DESC LIMIT 1
  `).get(device.id, type);

  if (lastAcked) {
    // User already acknowledged this alert type for this device.
    // Don't re-alert until the condition clears (hysteresis resets the alert state).
    // The caller (checkAlerts) only calls createAlert when the state flag transitions,
    // so if we reach here with an acked alert, it means the condition never cleared.
    // Skip — the state machine should have prevented this, but double-check.
    const unackedSinceLast = db.prepare(`
      SELECT id FROM alerts
      WHERE device_id = ? AND type = ? AND acknowledged = 0 AND created_at > (
        SELECT created_at FROM alerts WHERE id = ?
      )
    `).get(device.id, type, lastAcked.id);
    // If there's already an unacked alert after the last ack, this is a reminder cycle
    // Check if enough time has passed for the next escalation step
    if (unackedSinceLast) {
      // Count how many unacked alerts exist since last ack = escalation step
      const step = db.prepare(`
        SELECT COUNT(*) as c FROM alerts
        WHERE device_id = ? AND type = ? AND acknowledged = 0 AND created_at > (
          SELECT created_at FROM alerts WHERE id = ?
        )
      `).get(device.id, type, lastAcked.id).c;

      if (step >= intervals.length) return; // Max escalation reached, stop

      const nextInterval = intervals[Math.min(step, intervals.length - 1)];
      const tooSoon = db.prepare(`
        SELECT id FROM alerts
        WHERE device_id = ? AND type = ? AND created_at > datetime('now', '-' || ? || ' minutes')
      `).get(device.id, type, nextInterval || 1440);
      if (tooSoon) return; // Not yet time for next reminder
    }
  } else {
    // No acked alerts — check if we already sent an unacked alert recently
    const lastAlert = db.prepare(`
      SELECT id, created_at FROM alerts
      WHERE device_id = ? AND type = ?
      ORDER BY created_at DESC LIMIT 1
    `).get(device.id, type);

    if (lastAlert) {
      // Count total unacked alerts = escalation step
      const step = db.prepare(`
        SELECT COUNT(*) as c FROM alerts
        WHERE device_id = ? AND type = ? AND acknowledged = 0
      `).get(device.id, type).c;

      if (step >= intervals.length) {
        // At max escalation — only allow daily reminders
        const tooSoon = db.prepare(`
          SELECT id FROM alerts
          WHERE device_id = ? AND type = ? AND created_at > datetime('now', '-1440 minutes')
        `).get(device.id, type);
        if (tooSoon) return;
      } else {
        const nextInterval = intervals[Math.min(step, intervals.length - 1)];
        const tooSoon = db.prepare(`
          SELECT id FROM alerts
          WHERE device_id = ? AND type = ? AND created_at > datetime('now', '-' || ? || ' minutes')
        `).get(device.id, type, Math.max(nextInterval, 1));
        if (tooSoon) return;
      }
    }
  }

  // Create the alert
  stmts.insertAlert.run(device.id, type, message);

  // SSE notification to user (always, for in-app)
  broadcastToUser(site.user_id, {
    type: 'alert',
    alert: { device_name: device.name, site_name: site.name, type, message, severity }
  });

  // Push notification for critical and warning only
  if ((severity === 'critical' || severity === 'warning') && VAPID_PUBLIC) {
    const subs = stmts.getPushSubs.all(device.id);
    if (subs.length === 0) return;

    const payload = JSON.stringify({
      title: severity === 'critical' ? '\u{1F6A8} TankSync Alert' : '\u{26A0}\u{FE0F} TankSync Warning',
      body: message,
      icon: '/icon-192.png',
      badge: '/icon-192.png',
      tag: `${type}-${device.id}`,
      renotify: severity === 'critical',
      data: { type, deviceId: device.id, severity, url: `/tank/${device.id}` }
    });

    for (const sub of subs) {
      sendPush(sub, payload).then(() => {
        console.log(`[Push] Sent to sub ${sub.id} (${type})`);
      }).catch((err) => {
        if (err.statusCode === 403 || err.statusCode === 404 || err.statusCode === 410) {
          db.prepare('DELETE FROM push_subs WHERE id = ?').run(sub.id);
          console.log(`[Push] Cleaned dead sub ${sub.id} (${err.statusCode})`);
        } else {
          console.error(`[Push] Failed sub ${sub.id}:`, err.statusCode || err.message);
        }
      });
    }
  }
}

// Export for server to call on interval
export { checkDeviceTimeouts as runTimeoutChecks };
