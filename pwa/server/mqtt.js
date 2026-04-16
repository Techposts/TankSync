// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

import mqtt from 'mqtt';
import db, { stmts } from './db.js';
import { broadcast } from './sse.js';
import { checkAlerts } from './alerts.js';

let client = null;
const BROKER_URL = process.env.MQTT_URL || 'mqtt://localhost:1883';

// Rate-limit readings storage (at most one per 30s per device)
const lastReadingTime = new Map();
const MIN_READING_INTERVAL_MS = 30_000;

export function connectMqtt() {
  client = mqtt.connect(BROKER_URL, {
    clientId: `tanksync_pwa_${Date.now()}`,
    clean: true,
    reconnectPeriod: 5000
  });

  client.on('connect', () => {
    console.log('[MQTT] Connected to', BROKER_URL);
    client.subscribe('tanksync/#', { qos: 1 });
  });

  client.on('message', (topic, payload) => {
    try {
      handleMessage(topic, payload.toString());
    } catch (err) {
      console.error('[MQTT] Error handling message:', err.message);
    }
  });

  client.on('error', (err) => {
    console.error('[MQTT] Error:', err.message);
  });

  // Clean old readings every hour
  setInterval(() => {
    try { stmts.cleanOldReadings.run(); } catch {}
  }, 3600_000);
}

function handleMessage(topic, value) {
  // topic formats:
  //   tanksync/{device_id}/tank_{addr}/{field}
  //   tanksync/{device_id}/{name_slug}/{field}
  //   tanksync/{device_id}/status
  //   tanksync/{device_id}/system/{field}
  const parts = topic.split('/');
  if (parts.length < 3 || parts[0] !== 'tanksync') return;

  const mqttDeviceId = parts[1];
  const rest = parts.slice(2);

  // Skip system topics and status
  if (rest[0] === 'system' || rest[0] === 'status') return;

  // Tank data: rest = [tank_slug, field]
  if (rest.length !== 2) return;
  const [tankSlug, field] = rest;

  // Find ALL sites with this mqtt_device_id (multiple users can link the same receiver)
  const sites = db.prepare('SELECT * FROM sites WHERE mqtt_device_id = ?').all(mqttDeviceId);
  if (sites.length === 0) return;

  const addrMatch = tankSlug.match(/^tank_(\d+)$/);

  // Update matching device in EVERY site that has this receiver linked
  for (const site of sites) {
    const devices = stmts.getDevicesBySite.all(site.id);

    let device;
    if (addrMatch) {
      device = devices.find(d => d.lora_address === parseInt(addrMatch[1]));
    } else {
      device = devices.find(d => {
        const slug = d.name.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_|_$/g, '');
        return slug === tankSlug;
      }) || devices.find(d => d.lora_address.toString() === tankSlug);
    }

    if (!device) continue;

    processDeviceUpdate(device, site, field, value);
  }
}

function processDeviceUpdate(device, site, field, value) {
  const numVal = parseFloat(value);

  // Handle state field specially (string value)
  if (field === 'state') {
    db.prepare(`UPDATE devices SET state = ?, last_seen = datetime('now') WHERE id = ?`).run(value, device.id);
    broadcast({ type: 'device_update', siteId: site.id, deviceId: device.id, field, value });
    return;
  }

  // Numeric fields — update the appropriate live column
  const fieldMap = {
    water_pct: 'last_water_pct',
    battery_pct: 'last_battery_pct',
    battery_v: 'last_battery_v',
    rssi: 'last_rssi',
  };

  const column = fieldMap[field];
  if (column) {
    db.prepare(`UPDATE devices SET ${column} = ?, last_seen = datetime('now'), state = 'online' WHERE id = ?`)
      .run(numVal, device.id);
  }

  // Store a reading when water_pct arrives (rate-limited)
  if (field === 'water_pct') {
    const now = Date.now();
    const lastTime = lastReadingTime.get(device.id) || 0;
    if (now - lastTime >= MIN_READING_INTERVAL_MS) {
      const freshDevice = db.prepare('SELECT * FROM devices WHERE id = ?').get(device.id);
      if (freshDevice) {
        stmts.insertReading.run(
          device.id,
          freshDevice.last_water_pct,
          null, // water_liters populated by next message
          freshDevice.last_battery_pct,
          freshDevice.last_battery_v,
          freshDevice.last_rssi,
          null  // distance_cm
        );
        lastReadingTime.set(device.id, now);
        checkAlerts(freshDevice, site);
      }
    }
  }

  // Also store water_liters if available
  if (field === 'water_liters') {
    // Update the most recent reading's water_liters
    db.prepare(`
      UPDATE readings SET water_liters = ?
      WHERE id = (SELECT id FROM readings WHERE device_id = ? ORDER BY recorded_at DESC LIMIT 1)
    `).run(numVal, device.id);
  }

  if (field === 'distance_cm') {
    db.prepare(`
      UPDATE readings SET distance_cm = ?
      WHERE id = (SELECT id FROM readings WHERE device_id = ? ORDER BY recorded_at DESC LIMIT 1)
    `).run(numVal, device.id);
  }

  // Broadcast SSE update to all connected clients
  broadcast({
    type: 'device_update',
    siteId: site.id,
    deviceId: device.id,
    field,
    value: field === 'state' ? value : numVal
  });
}

export function getMqttClient() {
  return client;
}
