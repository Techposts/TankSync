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
  const opts = { clientId: `tanksync_cloud_${Date.now()}`, clean: true, reconnectPeriod: 5000 };
  if (process.env.MQTT_USER) {
    opts.username = process.env.MQTT_USER;
    opts.password = process.env.MQTT_PASS;
  }

  client = mqtt.connect(BROKER_URL, opts);

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
  setInterval(async () => {
    try { await stmts.cleanOldReadings(); } catch {}
  }, 3600_000);
}

async function handleMessage(topic, value) {
  const parts = topic.split('/');
  if (parts.length < 3 || parts[0] !== 'tanksync') return;

  const mqttDeviceId = parts[1];
  const rest = parts.slice(2);

  if (rest[0] === 'system' || rest[0] === 'status') return;
  if (rest.length !== 2) return;
  const [tankSlug, field] = rest;

  const sites = await db.all('SELECT * FROM sites WHERE mqtt_device_id = $1', mqttDeviceId);
  if (sites.length === 0) return;

  const addrMatch = tankSlug.match(/^tank_(\d+)$/);

  for (const site of sites) {
    const devices = await stmts.getDevicesBySite(site.id);

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
    await processDeviceUpdate(device, site, field, value);
  }
}

async function processDeviceUpdate(device, site, field, value) {
  const numVal = parseFloat(value);

  if (field === 'state') {
    await db.run('UPDATE devices SET state = $1, last_seen = NOW() WHERE id = $2', value, device.id);
    broadcast({ type: 'device_update', siteId: site.id, deviceId: device.id, field, value });
    return;
  }

  const fieldMap = { water_pct: 'last_water_pct', battery_pct: 'last_battery_pct', battery_v: 'last_battery_v', rssi: 'last_rssi' };
  const column = fieldMap[field];
  if (column) {
    await db.run(`UPDATE devices SET ${column} = $1, last_seen = NOW(), state = 'online' WHERE id = $2`, numVal, device.id);
  }

  if (field === 'water_pct') {
    const now = Date.now();
    const lastTime = lastReadingTime.get(device.id) || 0;
    if (now - lastTime >= MIN_READING_INTERVAL_MS) {
      const freshDevice = await db.get('SELECT * FROM devices WHERE id = $1', device.id);
      if (freshDevice) {
        await stmts.insertReading(device.id, freshDevice.last_water_pct, null, freshDevice.last_battery_pct, freshDevice.last_battery_v, freshDevice.last_rssi, null);
        lastReadingTime.set(device.id, now);
        await checkAlerts(freshDevice, site);
      }
    }
  }

  if (field === 'water_liters') {
    await db.run(
      `UPDATE readings SET water_liters = $1 WHERE id = (SELECT id FROM readings WHERE device_id = $2 ORDER BY recorded_at DESC LIMIT 1)`,
      numVal, device.id
    );
  }

  if (field === 'distance_cm') {
    await db.run(
      `UPDATE readings SET distance_cm = $1 WHERE id = (SELECT id FROM readings WHERE device_id = $2 ORDER BY recorded_at DESC LIMIT 1)`,
      numVal, device.id
    );
  }

  broadcast({ type: 'device_update', siteId: site.id, deviceId: device.id, field, value: field === 'state' ? value : numVal });
}

export function getMqttClient() {
  return client;
}
