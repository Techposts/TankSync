// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

import db, { stmts } from './db.js';
import { broadcastToUser } from './sse.js';
import { sendPush } from './push-sender.js';

const alertState = new Map();
const VAPID_PUBLIC = process.env.VAPID_PUBLIC_KEY || '';

function getState(deviceId) {
  if (!alertState.has(deviceId)) {
    alertState.set(deviceId, {
      low_water: false, tank_empty: false, high_water: false,
      low_battery: false, battery_critical: false, weak_signal: false,
      device_offline: false, device_stale: false, rapid_drop: false,
      recentReadings: [], lastOnlineTime: null, wasOffline: false,
    });
  }
  return alertState.get(deviceId);
}

export async function checkAlerts(device, site) {
  const state = getState(device.id);
  const pct = device.last_water_pct;
  const bat = device.last_battery_pct;
  const rssi = device.last_rssi;
  const now = Date.now();

  if (pct != null) {
    state.recentReadings.push({ pct, timestamp: now });
    const twoHoursAgo = now - 2 * 3600_000;
    state.recentReadings = state.recentReadings.filter(r => r.timestamp > twoHoursAgo);
  }

  // Tank empty
  if (pct != null && pct <= 0 && !state.tank_empty) {
    state.tank_empty = true;
    await createAlert(device, site, 'tank_empty', `${device.name} is completely empty! 0% water remaining.`, 'critical');
  } else if (pct > 5) { state.tank_empty = false; }

  // Low water
  if (pct != null && pct > 0 && pct <= device.alert_low_pct && !state.low_water) {
    state.low_water = true;
    await createAlert(device, site, 'low_water', `${device.name} water level low: ${Math.round(pct)}% (threshold: ${device.alert_low_pct}%)`, 'critical');
  } else if (pct > device.alert_low_pct + 5) {
    if (state.low_water) {
      state.low_water = false;
      await createAlert(device, site, 'level_recovered', `${device.name} water level recovered to ${Math.round(pct)}%`, 'info');
    }
  }

  // High water
  if (pct != null && pct >= device.alert_high_pct && !state.high_water) {
    state.high_water = true;
    await createAlert(device, site, 'high_water', `${device.name} water level high: ${Math.round(pct)}% — possible overflow risk`, 'warning');
  } else if (pct < device.alert_high_pct - 5) { state.high_water = false; }

  // Rapid drop
  if (state.recentReadings.length >= 3) {
    const oneHourAgo = now - 3600_000;
    const hourReadings = state.recentReadings.filter(r => r.timestamp > oneHourAgo);
    if (hourReadings.length >= 2) {
      const drop = hourReadings[0].pct - hourReadings[hourReadings.length - 1].pct;
      if (drop >= 20 && !state.rapid_drop) {
        state.rapid_drop = true;
        await createAlert(device, site, 'rapid_drop', `${device.name} dropped ${Math.round(drop)}% in the last hour — possible leak`, 'warning');
      } else if (drop < 10) { state.rapid_drop = false; }
    }
  }

  // Rapid rise
  if (state.recentReadings.length >= 2) {
    const thirtyMinAgo = now - 1800_000;
    const recentR = state.recentReadings.filter(r => r.timestamp > thirtyMinAgo);
    if (recentR.length >= 2) {
      const rise = recentR[recentR.length - 1].pct - recentR[0].pct;
      if (rise >= 30) {
        await createAlert(device, site, 'rapid_rise', `${device.name} filling rapidly: +${Math.round(rise)}% in ${Math.round((now - recentR[0].timestamp) / 60000)}min`, 'info');
        state.recentReadings = recentR.slice(-2);
      }
    }
  }

  // Battery critical
  if (bat != null && bat <= 5 && !state.battery_critical) {
    state.battery_critical = true;
    await createAlert(device, site, 'battery_critical', `${device.name} battery critically low: ${Math.round(bat)}%`, 'critical');
  } else if (bat > 15) { state.battery_critical = false; }

  // Low battery
  if (bat != null && bat > 5 && bat <= 15 && !state.low_battery) {
    state.low_battery = true;
    await createAlert(device, site, 'low_battery', `${device.name} battery low: ${Math.round(bat)}%`, 'warning');
  } else if (bat > 25) {
    if (state.low_battery) {
      state.low_battery = false;
      await createAlert(device, site, 'battery_ok', `${device.name} battery recovered to ${Math.round(bat)}%`, 'info');
    }
  }

  // Weak signal
  if (rssi != null && rssi < -100 && !state.weak_signal) {
    state.weak_signal = true;
    await createAlert(device, site, 'weak_signal', `${device.name} LoRa signal very weak: ${rssi} dBm`, 'warning');
  } else if (rssi != null && rssi > -90) { state.weak_signal = false; }

  // Online recovery
  if (state.wasOffline && device.state === 'online') {
    state.wasOffline = false;
    state.device_offline = false;
    state.device_stale = false;
    await createAlert(device, site, 'device_online', `${device.name} is back online`, 'info');
  }

  state.lastOnlineTime = now;
}

export async function checkDeviceTimeouts() {
  const allDevices = await stmts.getAllDevicesWithSites();
  const now = Date.now();

  for (const device of allDevices) {
    if (!device.last_seen) continue;
    const state = getState(device.id);
    const lastSeen = new Date(device.last_seen).getTime();
    const ageSec = (now - lastSeen) / 1000;
    const site = { id: device.site_id, name: device.site_name, user_id: device.user_id };

    if (ageSec > 600 && ageSec <= 900 && !state.device_stale) {
      state.device_stale = true;
      await db.run('UPDATE devices SET state = $1 WHERE id = $2', 'stale', device.id);
      await createAlert(device, site, 'device_stale', `${device.name} hasn't reported in ${Math.round(ageSec / 60)} minutes`, 'info');
    }

    if (ageSec > 900 && !state.device_offline) {
      state.device_offline = true;
      state.wasOffline = true;
      await db.run('UPDATE devices SET state = $1 WHERE id = $2', 'offline', device.id);
      await createAlert(device, site, 'device_offline', `${device.name} is offline — no data for ${Math.round(ageSec / 60)} minutes`, 'critical');
    }
  }
}

const ESCALATION = {
  critical: [0, 60, 240, 720, 1440],
  warning: [0, 240, 1440],
  info: [0],
};

async function createAlert(device, site, type, message, severity = 'info') {
  const intervals = ESCALATION[severity] || ESCALATION.info;

  const lastAcked = await db.get(
    'SELECT id FROM alerts WHERE device_id = $1 AND type = $2 AND acknowledged = 1 ORDER BY created_at DESC LIMIT 1',
    device.id, type
  );

  if (lastAcked) {
    const unackedSinceLast = await db.get(
      'SELECT id FROM alerts WHERE device_id = $1 AND type = $2 AND acknowledged = 0 AND created_at > (SELECT created_at FROM alerts WHERE id = $3)',
      device.id, type, lastAcked.id
    );
    if (unackedSinceLast) {
      const step = (await db.get(
        'SELECT COUNT(*) as c FROM alerts WHERE device_id = $1 AND type = $2 AND acknowledged = 0 AND created_at > (SELECT created_at FROM alerts WHERE id = $3)',
        device.id, type, lastAcked.id
      )).c;
      if (step >= intervals.length) return;
      const nextInterval = intervals[Math.min(step, intervals.length - 1)];
      const tooSoon = await db.get(
        `SELECT id FROM alerts WHERE device_id = $1 AND type = $2 AND created_at > NOW() - ($3 || ' minutes')::interval`,
        device.id, type, nextInterval || 1440
      );
      if (tooSoon) return;
    }
  } else {
    const lastAlert = await db.get(
      'SELECT id, created_at FROM alerts WHERE device_id = $1 AND type = $2 ORDER BY created_at DESC LIMIT 1',
      device.id, type
    );
    if (lastAlert) {
      const step = (await db.get(
        'SELECT COUNT(*) as c FROM alerts WHERE device_id = $1 AND type = $2 AND acknowledged = 0',
        device.id, type
      )).c;
      if (step >= intervals.length) {
        const tooSoon = await db.get(
          `SELECT id FROM alerts WHERE device_id = $1 AND type = $2 AND created_at > NOW() - interval '1440 minutes'`,
          device.id, type
        );
        if (tooSoon) return;
      } else {
        const nextInterval = intervals[Math.min(step, intervals.length - 1)];
        const tooSoon = await db.get(
          `SELECT id FROM alerts WHERE device_id = $1 AND type = $2 AND created_at > NOW() - ($3 || ' minutes')::interval`,
          device.id, type, Math.max(nextInterval, 1)
        );
        if (tooSoon) return;
      }
    }
  }

  await stmts.insertAlert(device.id, type, message);

  broadcastToUser(site.user_id, {
    type: 'alert',
    alert: { device_name: device.name, site_name: site.name, type, message, severity }
  });

  if ((severity === 'critical' || severity === 'warning') && VAPID_PUBLIC) {
    const subs = await stmts.getPushSubs(device.id);
    if (subs.length === 0) return;
    const payload = JSON.stringify({
      title: severity === 'critical' ? '\u{1F6A8} TankSync Alert' : '\u{26A0}\u{FE0F} TankSync Warning',
      body: message, icon: '/icon-192.png', badge: '/icon-192.png',
      tag: `${type}-${device.id}`, renotify: severity === 'critical',
      data: { type, deviceId: device.id, severity, url: `/tank/${device.id}` }
    });
    for (const sub of subs) {
      sendPush(sub, payload).then(() => {
        console.log(`[Push] Sent to sub ${sub.id} (${type})`);
      }).catch(async (err) => {
        if ([403, 404, 410].includes(err.statusCode)) {
          await db.run('DELETE FROM push_subs WHERE id = $1', sub.id);
        } else {
          console.error(`[Push] Failed sub ${sub.id}:`, err.statusCode || err.message);
        }
      });
    }
  }
}

export { checkDeviceTimeouts as runTimeoutChecks };
