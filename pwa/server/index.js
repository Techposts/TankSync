// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

import Fastify from 'fastify';
import cors from '@fastify/cors';
import jwt from '@fastify/jwt';
import fstatic from '@fastify/static';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';
import { existsSync } from 'fs';
import bcrypt from 'bcryptjs';
import db, { stmts } from './db.js';
import { connectMqtt } from './mqtt.js';
import { addClient } from './sse.js';
import { checkDeviceTimeouts } from './alerts.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const isProd = process.env.NODE_ENV === 'production';
const PORT = parseInt(process.env.PORT || '4800');

const app = Fastify({ logger: { level: isProd ? 'warn' : 'info' } });

await app.register(cors, { origin: true, credentials: true });
await app.register(jwt, { secret: process.env.JWT_SECRET || 'tanksync-dev-secret-change-me' });

// Auth decorator
app.decorate('authenticate', async (req, reply) => {
  try { await req.jwtVerify(); }
  catch { reply.code(401).send({ error: 'Unauthorized' }); }
});

// ─── AUTH ROUTES ───────────────────────────────────────────────────────────────

app.post('/api/auth/register', async (req, reply) => {
  const { email, password, name } = req.body || {};
  if (!email || !password) return reply.code(400).send({ error: 'Email and password required' });
  if (password.length < 6) return reply.code(400).send({ error: 'Password must be at least 6 characters' });

  const existing = stmts.getUserByEmail.get(email);
  if (existing) return reply.code(409).send({ error: 'Email already registered' });

  const hash = await bcrypt.hash(password, 10);
  const result = db.prepare('INSERT INTO users (email, password_hash, name) VALUES (?, ?, ?)').run(email, hash, name || '');
  const token = app.jwt.sign({ id: result.lastInsertRowid, email });
  return { token, user: { id: result.lastInsertRowid, email, name: name || '', mode: 'home' } };
});

app.post('/api/auth/login', async (req, reply) => {
  const { email, password } = req.body || {};
  if (!email || !password) return reply.code(400).send({ error: 'Email and password required' });

  const user = stmts.getUserByEmail.get(email);
  if (!user) return reply.code(401).send({ error: 'Invalid credentials' });

  const valid = await bcrypt.compare(password, user.password_hash);
  if (!valid) return reply.code(401).send({ error: 'Invalid credentials' });

  const token = app.jwt.sign({ id: user.id, email: user.email });
  return { token, user: { id: user.id, email: user.email, name: user.name, mode: user.mode } };
});

app.get('/api/auth/me', { preHandler: [app.authenticate] }, async (req) => {
  const user = stmts.getUserById.get(req.user.id);
  if (!user) return { error: 'User not found' };
  return { user };
});

app.put('/api/auth/me', { preHandler: [app.authenticate] }, async (req) => {
  const { name, mode } = req.body || {};
  if (name !== undefined) db.prepare('UPDATE users SET name = ? WHERE id = ?').run(name, req.user.id);
  if (mode !== undefined && ['home', 'pro'].includes(mode))
    db.prepare('UPDATE users SET mode = ? WHERE id = ?').run(mode, req.user.id);
  return stmts.getUserById.get(req.user.id);
});

// ─── SITES ─────────────────────────────────────────────────────────────────────

app.get('/api/sites', { preHandler: [app.authenticate] }, async (req) => {
  const sites = stmts.getSitesByUser.all(req.user.id);
  return sites.map(site => {
    const devices = stmts.getDevicesBySite.all(site.id);
    return { ...site, devices, deviceCount: devices.length };
  });
});

app.post('/api/sites', { preHandler: [app.authenticate] }, async (req, reply) => {
  const { name, receiver_ip, mqtt_device_id } = req.body || {};
  if (!name) return reply.code(400).send({ error: 'Site name required' });

  // Auto-detect MQTT device ID if receiver_ip is provided but mqtt_device_id is not
  let resolvedMqttId = mqtt_device_id || null;
  if (receiver_ip && !resolvedMqttId) {
    // Check all sites' MQTT device IDs to find one whose system/ip matches
    // This works because the receiver publishes tanksync/{id}/system/ip as retained
    try {
      const allSites = db.prepare('SELECT mqtt_device_id FROM sites WHERE mqtt_device_id IS NOT NULL').all();
      const knownIds = new Set(allSites.map(s => s.mqtt_device_id));
      // Also scan the MQTT retained IP topics via a quick DB lookup or direct check
      // Simplest: probe the receiver and derive MAC from its response headers or mDNS
      // Fallback: search for matching IP in MQTT topics
      const { getMqttClient } = await import('./mqtt.js');
      const client = getMqttClient();
      if (client) {
        resolvedMqttId = await new Promise((resolve) => {
          const timeout = setTimeout(() => resolve(null), 3000);
          const handler = (topic, payload) => {
            // tanksync/{device_id}/system/ip = "x.x.x.x"
            const parts = topic.split('/');
            if (parts.length === 4 && parts[0] === 'tanksync' && parts[2] === 'system' && parts[3] === 'ip') {
              if (payload.toString().trim() === receiver_ip) {
                clearTimeout(timeout);
                client.removeListener('message', handler);
                resolve(parts[1]);
              }
            }
          };
          client.on('message', handler);
          // Re-subscribe to trigger retained messages
          client.subscribe('tanksync/+/system/ip', { qos: 0 });
        });
      }
    } catch {}
  }

  const result = db.prepare(
    'INSERT INTO sites (user_id, name, receiver_ip, mqtt_device_id) VALUES (?, ?, ?, ?)'
  ).run(req.user.id, name, receiver_ip || null, resolvedMqttId);
  return { id: result.lastInsertRowid, name, receiver_ip, mqtt_device_id: resolvedMqttId };
});

app.put('/api/sites/:id', { preHandler: [app.authenticate] }, async (req, reply) => {
  const site = db.prepare('SELECT * FROM sites WHERE id = ? AND user_id = ?').get(req.params.id, req.user.id);
  if (!site) return reply.code(404).send({ error: 'Site not found' });
  const { name, receiver_ip, mqtt_device_id } = req.body || {};
  db.prepare('UPDATE sites SET name = COALESCE(?, name), receiver_ip = COALESCE(?, receiver_ip), mqtt_device_id = COALESCE(?, mqtt_device_id) WHERE id = ?')
    .run(name, receiver_ip, mqtt_device_id, site.id);
  return { success: true };
});

app.delete('/api/sites/:id', { preHandler: [app.authenticate] }, async (req, reply) => {
  const result = db.prepare('DELETE FROM sites WHERE id = ? AND user_id = ?').run(req.params.id, req.user.id);
  if (result.changes === 0) return reply.code(404).send({ error: 'Site not found' });
  return { success: true };
});

// ─── DEVICES ───────────────────────────────────────────────────────────────────

app.get('/api/sites/:siteId/devices', { preHandler: [app.authenticate] }, async (req, reply) => {
  const site = db.prepare('SELECT * FROM sites WHERE id = ? AND user_id = ?').get(req.params.siteId, req.user.id);
  if (!site) return reply.code(404).send({ error: 'Site not found' });
  return stmts.getDevicesBySite.all(site.id);
});

app.post('/api/sites/:siteId/devices', { preHandler: [app.authenticate] }, async (req, reply) => {
  const site = db.prepare('SELECT * FROM sites WHERE id = ? AND user_id = ?').get(req.params.siteId, req.user.id);
  if (!site) return reply.code(404).send({ error: 'Site not found' });
  const { lora_address, name, tank_capacity_l, min_distance_cm, max_distance_cm, alert_low_pct, alert_high_pct } = req.body || {};
  if (!lora_address) return reply.code(400).send({ error: 'LoRa address required' });
  const result = db.prepare(
    `INSERT INTO devices (site_id, lora_address, name, tank_capacity_l, min_distance_cm, max_distance_cm, alert_low_pct, alert_high_pct)
     VALUES (?, ?, ?, ?, ?, ?, ?, ?)`
  ).run(site.id, lora_address, name || 'Tank', tank_capacity_l || 1000, min_distance_cm || 10, max_distance_cm || 150, alert_low_pct || 20, alert_high_pct || 95);
  return { id: result.lastInsertRowid };
});

app.put('/api/devices/:id', { preHandler: [app.authenticate] }, async (req, reply) => {
  const device = db.prepare(`
    SELECT d.*, s.receiver_ip FROM devices d JOIN sites s ON d.site_id = s.id WHERE d.id = ? AND s.user_id = ?
  `).get(req.params.id, req.user.id);
  if (!device) return reply.code(404).send({ error: 'Device not found' });
  const { name, tank_capacity_l, min_distance_cm, max_distance_cm, alert_low_pct, alert_high_pct } = req.body || {};

  // Update local DB
  db.prepare(`UPDATE devices SET
    name = COALESCE(?, name), tank_capacity_l = COALESCE(?, tank_capacity_l),
    min_distance_cm = COALESCE(?, min_distance_cm), max_distance_cm = COALESCE(?, max_distance_cm),
    alert_low_pct = COALESCE(?, alert_low_pct), alert_high_pct = COALESCE(?, alert_high_pct)
    WHERE id = ?`
  ).run(name, tank_capacity_l, min_distance_cm, max_distance_cm, alert_low_pct, alert_high_pct, device.id);

  // Forward config to receiver so it updates its registry (used for distance→% conversion)
  let receiver_synced = false;
  if (device.receiver_ip) {
    const updated = db.prepare('SELECT * FROM devices WHERE id = ?').get(device.id);
    try {
      const res = await fetch(`http://${device.receiver_ip}/api/transmitters`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({
          addr: updated.lora_address,
          name: updated.name,
          min_dist: updated.min_distance_cm,
          max_dist: updated.max_distance_cm,
          capacity: updated.tank_capacity_l,
        }),
        signal: AbortSignal.timeout(5000),
      });
      receiver_synced = res.ok;
    } catch (err) {
      app.log.warn(`Failed to sync config to receiver ${device.receiver_ip}: ${err.message}`);
    }
  }

  return { success: true, receiver_synced };
});

app.delete('/api/devices/:id', { preHandler: [app.authenticate] }, async (req, reply) => {
  const result = db.prepare(`
    DELETE FROM devices WHERE id = ? AND site_id IN (SELECT id FROM sites WHERE user_id = ?)
  `).run(req.params.id, req.user.id);
  if (result.changes === 0) return reply.code(404).send({ error: 'Device not found' });
  return { success: true };
});

// ─── QR CODE DEVICE LINKING ────────────────────────────────────────────────────
// Called when user scans the QR code from the receiver's web UI.
// Verifies the claim token against the receiver, creates the site + devices.

app.post('/api/link/claim', { preHandler: [app.authenticate] }, async (req, reply) => {
  const { device_id, token, receiver_ip } = req.body || {};
  if (!device_id || !token || !receiver_ip) {
    return reply.code(400).send({ error: 'Missing device_id, token, or receiver_ip' });
  }

  // 1. Verify token by calling the receiver's /api/link endpoint
  let receiverData;
  try {
    const resp = await fetch(`http://${receiver_ip}/api/link`, { signal: AbortSignal.timeout(5000) });
    receiverData = await resp.json();
  } catch {
    return reply.code(502).send({ error: 'Could not reach receiver. Make sure the server and receiver are on the same network.' });
  }

  if (receiverData.device_id !== device_id || receiverData.token !== token) {
    return reply.code(403).send({ error: 'Invalid or expired link token. Try scanning the QR code again.' });
  }

  // 2. Check if site already exists for this user + device
  const existing = db.prepare(
    'SELECT id FROM sites WHERE user_id = ? AND mqtt_device_id = ?'
  ).get(req.user.id, device_id);
  if (existing) {
    return { site_id: existing.id, message: 'Device already linked', already_linked: true };
  }

  // 3. Create site
  const site = db.prepare(
    'INSERT INTO sites (user_id, name, receiver_ip, mqtt_device_id) VALUES (?, ?, ?, ?)'
  ).run(req.user.id, 'My Tank', receiver_ip, device_id);
  const siteId = site.lastInsertRowid;

  // 4. Auto-discover devices from receiver's /api/data endpoint
  let deviceCount = 0;
  try {
    const dataResp = await fetch(`http://${receiver_ip}/api/data`, { signal: AbortSignal.timeout(5000) });
    const data = await dataResp.json();
    if (data.tanks && data.tanks.length > 0) {
      for (const tank of data.tanks) {
        try {
          db.prepare(
            'INSERT INTO devices (site_id, lora_address, name) VALUES (?, ?, ?)'
          ).run(siteId, tank.address, tank.name || `Tank ${tank.address}`);
          deviceCount++;
        } catch {}
      }
    }
  } catch {
    // Couldn't fetch devices — user can add manually later
  }

  // 5. Also try /api/transmitters for capacity/distance config
  try {
    const txResp = await fetch(`http://${receiver_ip}/api/transmitters`, { signal: AbortSignal.timeout(5000) });
    const txData = await txResp.json();
    if (txData.transmitters) {
      for (const tx of txData.transmitters) {
        db.prepare(`
          UPDATE devices SET
            tank_capacity_l = ?, min_distance_cm = ?, max_distance_cm = ?,
            fw_version = ?
          WHERE site_id = ? AND lora_address = ?
        `).run(tx.capacity, tx.min_dist, tx.max_dist, tx.fw_version || null, siteId, tx.address);
      }
    }
  } catch {}

  return { site_id: siteId, device_count: deviceCount, message: 'Device linked successfully' };
});

// ─── HISTORY ───────────────────────────────────────────────────────────────────

app.get('/api/devices/:id/history', { preHandler: [app.authenticate] }, async (req, reply) => {
  const device = db.prepare(`
    SELECT d.* FROM devices d JOIN sites s ON d.site_id = s.id WHERE d.id = ? AND s.user_id = ?
  `).get(req.params.id, req.user.id);
  if (!device) return reply.code(404).send({ error: 'Device not found' });

  const range = req.query.range || '24h';
  const rangeMap = { '1h': '-1 hours', '6h': '-6 hours', '24h': '-1 days', '7d': '-7 days', '30d': '-30 days', '90d': '-90 days' };
  const sqlRange = rangeMap[range] || '-1 days';

  return stmts.getReadings.all(device.id, sqlRange);
});

// ─── ALERTS ────────────────────────────────────────────────────────────────────

app.get('/api/alerts', { preHandler: [app.authenticate] }, async (req) => {
  const page = parseInt(req.query.page || '1');
  const limit = Math.min(parseInt(req.query.limit || '50'), 100);
  const offset = (page - 1) * limit;
  const alerts = stmts.getAlerts.all(req.user.id, limit, offset);
  const { count } = stmts.getUnackAlertCount.get(req.user.id);
  return { alerts, unread: count };
});

app.put('/api/alerts/:id/ack', { preHandler: [app.authenticate] }, async (req) => {
  stmts.ackAlert.run(req.params.id);
  return { success: true };
});

app.put('/api/alerts/ack-all', { preHandler: [app.authenticate] }, async (req) => {
  db.prepare(`
    UPDATE alerts SET acknowledged = 1
    WHERE device_id IN (SELECT d.id FROM devices d JOIN sites s ON d.site_id = s.id WHERE s.user_id = ?)
  `).run(req.user.id);
  return { success: true };
});

// ─── PUSH SUBSCRIPTIONS ───────────────────────────────────────────────────────

app.post('/api/push/subscribe', { preHandler: [app.authenticate] }, async (req) => {
  const { endpoint, keys } = req.body || {};
  if (!endpoint || !keys?.p256dh || !keys?.auth) return { error: 'Invalid subscription' };
  db.prepare(
    'INSERT OR REPLACE INTO push_subs (user_id, endpoint, p256dh, auth) VALUES (?, ?, ?, ?)'
  ).run(req.user.id, endpoint, keys.p256dh, keys.auth);
  return { success: true };
});

app.delete('/api/push/unsubscribe', { preHandler: [app.authenticate] }, async (req) => {
  const { endpoint } = req.body || {};
  db.prepare('DELETE FROM push_subs WHERE user_id = ? AND endpoint = ?').run(req.user.id, endpoint);
  return { success: true };
});

// ─── RECEIVER PROXY ────────────────────────────────────────────────────────────

app.get('/api/sites/:siteId/proxy/data', { preHandler: [app.authenticate] }, async (req, reply) => {
  const site = db.prepare('SELECT * FROM sites WHERE id = ? AND user_id = ?').get(req.params.siteId, req.user.id);
  if (!site?.receiver_ip) return reply.code(400).send({ error: 'No receiver IP configured' });
  try {
    const resp = await fetch(`http://${site.receiver_ip}/api/data`, { signal: AbortSignal.timeout(5000) });
    return resp.json();
  } catch {
    return reply.code(502).send({ error: 'Receiver unreachable' });
  }
});

app.get('/api/sites/:siteId/proxy/system', { preHandler: [app.authenticate] }, async (req, reply) => {
  const site = db.prepare('SELECT * FROM sites WHERE id = ? AND user_id = ?').get(req.params.siteId, req.user.id);
  if (!site?.receiver_ip) return reply.code(400).send({ error: 'No receiver IP configured' });
  try {
    const resp = await fetch(`http://${site.receiver_ip}/api/system`, { signal: AbortSignal.timeout(5000) });
    return resp.json();
  } catch {
    return reply.code(502).send({ error: 'Receiver unreachable' });
  }
});

// ─── OTA PROXY ─────────────────────────────────────────────────────────────────

app.get('/api/sites/:siteId/proxy/ota/state', { preHandler: [app.authenticate] }, async (req, reply) => {
  const site = db.prepare('SELECT * FROM sites WHERE id = ? AND user_id = ?').get(req.params.siteId, req.user.id);
  if (!site?.receiver_ip) return reply.code(400).send({ error: 'No receiver IP configured' });
  try {
    const resp = await fetch(`http://${site.receiver_ip}/api/ota/state`, { signal: AbortSignal.timeout(5000) });
    return resp.json();
  } catch {
    return reply.code(502).send({ error: 'Receiver unreachable' });
  }
});

app.post('/api/sites/:siteId/proxy/ota/upload', { preHandler: [app.authenticate] }, async (req, reply) => {
  const site = db.prepare('SELECT * FROM sites WHERE id = ? AND user_id = ?').get(req.params.siteId, req.user.id);
  if (!site?.receiver_ip) return reply.code(400).send({ error: 'No receiver IP configured' });
  try {
    const body = await req.body;
    const resp = await fetch(`http://${site.receiver_ip}/api/ota/upload`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream' },
      body,
      signal: AbortSignal.timeout(120000)
    });
    return resp.json();
  } catch {
    return reply.code(502).send({ error: 'OTA upload failed' });
  }
});

// ─── VAPID PUBLIC KEY (for push subscription on frontend) ─────────────────────

app.get('/api/push/vapid-key', async () => {
  return { key: process.env.VAPID_PUBLIC_KEY || '' };
});

// ─── SSE (Server-Sent Events) ─────────────────────────────────────────────────

app.get('/events', async (req, reply) => {
  // SSE supports auth via query param (EventSource doesn't support headers)
  const token = req.query.token;
  if (!token) return reply.code(401).send({ error: 'Token required' });
  try {
    const decoded = app.jwt.verify(token);
    req.user = decoded;
  } catch {
    return reply.code(401).send({ error: 'Invalid token' });
  }

  reply.raw.writeHead(200, {
    'Content-Type': 'text/event-stream',
    'Cache-Control': 'no-cache',
    'Connection': 'keep-alive',
    'X-Accel-Buffering': 'no'
  });
  reply.raw.write(`data: ${JSON.stringify({ type: 'connected' })}\n\n`);
  addClient(req.user.id, reply);

  // Keep alive every 30s
  const keepAlive = setInterval(() => {
    try { reply.raw.write(': keepalive\n\n'); } catch { clearInterval(keepAlive); }
  }, 30000);

  req.raw.on('close', () => clearInterval(keepAlive));
  // Don't end the response — SSE stays open
  await new Promise(() => {});
});

// ─── SERVE STATIC (production) ────────────────────────────────────────────────

const distPath = join(__dirname, '..', 'dist');
if (isProd && existsSync(distPath)) {
  await app.register(fstatic, { root: distPath, prefix: '/' });
  app.setNotFoundHandler((req, reply) => {
    if (req.url.startsWith('/api/') || req.url.startsWith('/events')) {
      return reply.code(404).send({ error: 'Not found' });
    }
    return reply.sendFile('index.html');
  });
}

// ─── START ─────────────────────────────────────────────────────────────────────

connectMqtt();

// Check for stale/offline devices every 60 seconds
setInterval(() => {
  try { checkDeviceTimeouts(); } catch (err) {
    console.error('[Alerts] Timeout check error:', err.message);
  }
}, 60_000);

try {
  await app.listen({ port: PORT, host: '0.0.0.0' });
  console.log(`TankSync PWA server running on port ${PORT}`);
} catch (err) {
  app.log.error(err);
  process.exit(1);
}
