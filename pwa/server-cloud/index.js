// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

import Fastify from 'fastify';
import cors from '@fastify/cors';
import jwt from '@fastify/jwt';
import rateLimit from '@fastify/rate-limit';
import fstatic from '@fastify/static';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';
import { existsSync } from 'fs';
import { randomInt } from 'crypto';
import bcrypt from 'bcryptjs';
import db, { stmts } from './db.js';
import { connectMqtt } from './mqtt.js';
import { addClient } from './sse.js';
import { checkDeviceTimeouts } from './alerts.js';
import { generateMqttCredentials, pushMqttToReceiver, revokeMqttCredentials } from './mqtt-credentials.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const isProd = process.env.NODE_ENV === 'production';
const PORT = parseInt(process.env.PORT || '4800');

const app = Fastify({ logger: { level: isProd ? 'warn' : 'info' } });

await app.register(cors, { origin: true, credentials: true });
await app.register(jwt, { secret: process.env.JWT_SECRET || 'tanksync-dev-secret-change-me' });
await app.register(rateLimit, { global: false });

// Security headers
app.addHook('onSend', (req, reply, payload, done) => {
  reply.header('X-Content-Type-Options', 'nosniff');
  reply.header('X-Frame-Options', 'DENY');
  reply.header('X-XSS-Protection', '0');
  reply.header('Referrer-Policy', 'strict-origin-when-cross-origin');
  reply.header('Permissions-Policy', 'camera=(), microphone=(), geolocation=()');
  done();
});

// ─── EMAIL (Resend API) ───────────────────────────────────────────────────────
const RESEND_API_KEY = process.env.RESEND_API_KEY || '';
const EMAIL_FROM = process.env.EMAIL_FROM || 'TankSync <onboarding@resend.dev>';
const TURNSTILE_SECRET = process.env.TURNSTILE_SECRET || '';

async function verifyTurnstile(token) {
  if (!TURNSTILE_SECRET) return true;
  if (!token) return false;
  try {
    const res = await fetch('https://challenges.cloudflare.com/turnstile/v0/siteverify', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: `secret=${encodeURIComponent(TURNSTILE_SECRET)}&response=${encodeURIComponent(token)}`,
    });
    const data = await res.json();
    return data.success === true;
  } catch { return false; }
}

function generateCode() { return String(randomInt(100000, 999999)); }

async function sendVerificationEmail(email, code) {
  if (!RESEND_API_KEY) {
    console.log(`[VERIFY] No RESEND_API_KEY. Code for ${email}: ${code}`);
    return true;
  }
  const res = await fetch('https://api.resend.com/emails', {
    method: 'POST',
    headers: { 'Authorization': `Bearer ${RESEND_API_KEY}`, 'Content-Type': 'application/json' },
    body: JSON.stringify({
      from: EMAIL_FROM, to: email, subject: 'TankSync - Verify your email',
      html: `<div style="font-family:sans-serif;max-width:400px;margin:0 auto;padding:24px">
        <h2 style="color:#0EA5E9">TankSync</h2>
        <p>Your verification code is:</p>
        <div style="font-size:32px;font-weight:bold;letter-spacing:8px;text-align:center;
          padding:16px;background:#f1f5f9;border-radius:12px;margin:16px 0">${code}</div>
        <p style="color:#64748b;font-size:14px">This code expires in 10 minutes.</p>
      </div>`,
    }),
  });
  if (!res.ok) {
    const err = await res.json().catch(() => ({}));
    throw new Error(err.message || `Resend API error ${res.status}`);
  }
  return true;
}

// Auth decorator
app.decorate('authenticate', async (req, reply) => {
  try { await req.jwtVerify(); }
  catch { reply.code(401).send({ error: 'Unauthorized' }); }
});

// ─── AUTH ROUTES ───────────────────────────────────────────────────────────────

const authRateLimit = { max: 5, timeWindow: '1 minute' };
const loginRateLimit = { max: 10, timeWindow: '1 minute' };

app.post('/api/auth/register', { config: { rateLimit: authRateLimit } }, async (req, reply) => {
  const { email, password, name, turnstileToken } = req.body || {};
  if (!email || !password) return reply.code(400).send({ error: 'Email and password required' });
  if (password.length < 6) return reply.code(400).send({ error: 'Password must be at least 6 characters' });
  if (!/^[^\s@]+@[^\s@]+\.[^\s@]+$/.test(email)) return reply.code(400).send({ error: 'Invalid email address' });

  if (TURNSTILE_SECRET && !(await verifyTurnstile(turnstileToken))) {
    return reply.code(403).send({ error: 'Captcha verification failed. Please try again.' });
  }

  const existing = await stmts.getUserByEmail(email);
  if (existing) return reply.code(409).send({ error: 'Email already registered' });

  const hash = await bcrypt.hash(password, 10);
  const result = await db.run('INSERT INTO users (email, password_hash, name, email_verified) VALUES ($1, $2, $3, 0)', email, hash, name || '');

  const code = generateCode();
  await db.run(`INSERT INTO verification_codes (email, code, expires_at) VALUES ($1, $2, NOW() + interval '10 minutes')`, email, code);
  try { await sendVerificationEmail(email, code); } catch (err) {
    app.log.error(`Failed to send verification email: ${err.message}`);
  }

  const token = app.jwt.sign({ id: result.lastInsertRowid, email }, { expiresIn: '30d' });
  return { token, user: { id: result.lastInsertRowid, email, name: name || '', mode: 'home', email_verified: false }, needsVerification: true };
});

app.post('/api/auth/verify', { config: { rateLimit: authRateLimit } }, async (req, reply) => {
  const { email, code } = req.body || {};
  if (!email || !code) return reply.code(400).send({ error: 'Email and code required' });

  const record = await db.get(
    'SELECT * FROM verification_codes WHERE email = $1 AND code = $2 AND used = 0 AND expires_at > NOW() ORDER BY id DESC LIMIT 1',
    email, code
  );
  if (!record) return reply.code(400).send({ error: 'Invalid or expired code' });

  await db.run('UPDATE verification_codes SET used = 1 WHERE id = $1', record.id);
  await db.run('UPDATE users SET email_verified = 1 WHERE email = $1', email);

  const user = await stmts.getUserByEmail(email);
  const token = app.jwt.sign({ id: user.id, email: user.email }, { expiresIn: '30d' });
  return { token, user: { id: user.id, email: user.email, name: user.name, mode: user.mode, email_verified: true } };
});

app.post('/api/auth/resend-code', { config: { rateLimit: { max: 3, timeWindow: '5 minutes' } } }, async (req, reply) => {
  const { email } = req.body || {};
  if (!email) return reply.code(400).send({ error: 'Email required' });
  const user = await stmts.getUserByEmail(email);
  if (!user || user.email_verified) return reply.code(400).send({ error: 'Invalid request' });
  await db.run('UPDATE verification_codes SET used = 1 WHERE email = $1 AND used = 0', email);
  const code = generateCode();
  await db.run(`INSERT INTO verification_codes (email, code, expires_at) VALUES ($1, $2, NOW() + interval '10 minutes')`, email, code);
  try { await sendVerificationEmail(email, code); } catch {}
  return { sent: true };
});

app.post('/api/auth/login', { config: { rateLimit: loginRateLimit } }, async (req, reply) => {
  const { email, password } = req.body || {};
  if (!email || !password) return reply.code(400).send({ error: 'Email and password required' });
  const user = await stmts.getUserByEmail(email);
  if (!user) return reply.code(401).send({ error: 'Invalid credentials' });
  const valid = await bcrypt.compare(password, user.password_hash);
  if (!valid) return reply.code(401).send({ error: 'Invalid credentials' });
  const token = app.jwt.sign({ id: user.id, email: user.email }, { expiresIn: '30d' });
  return { token, user: { id: user.id, email: user.email, name: user.name, mode: user.mode, email_verified: !!user.email_verified }, needsVerification: !user.email_verified };
});

app.get('/api/auth/me', { preHandler: [app.authenticate] }, async (req) => {
  const user = await stmts.getUserById(req.user.id);
  if (!user) return { error: 'User not found' };
  return { user: { ...user, email_verified: !!user.email_verified } };
});

app.put('/api/auth/me', { preHandler: [app.authenticate] }, async (req) => {
  const { name, mode } = req.body || {};
  if (name !== undefined) await db.run('UPDATE users SET name = $1 WHERE id = $2', name, req.user.id);
  if (mode !== undefined && ['home', 'pro'].includes(mode))
    await db.run('UPDATE users SET mode = $1 WHERE id = $2', mode, req.user.id);
  return stmts.getUserById(req.user.id);
});

// ─── SITES ─────────────────────────────────────────────────────────────────────

app.get('/api/sites', { preHandler: [app.authenticate] }, async (req) => {
  const sites = await stmts.getSitesByUser(req.user.id);
  const result = [];
  for (const site of sites) {
    const devices = await stmts.getDevicesBySite(site.id);
    result.push({ ...site, devices, deviceCount: devices.length });
  }
  return result;
});

app.post('/api/sites', { preHandler: [app.authenticate] }, async (req, reply) => {
  const { name, receiver_ip, mqtt_device_id } = req.body || {};
  if (!name) return reply.code(400).send({ error: 'Site name required' });
  let resolvedMqttId = mqtt_device_id || null;
  const result = await db.run(
    'INSERT INTO sites (user_id, name, receiver_ip, mqtt_device_id) VALUES ($1, $2, $3, $4)',
    req.user.id, name, receiver_ip || null, resolvedMqttId
  );
  return { id: result.lastInsertRowid, name, receiver_ip, mqtt_device_id: resolvedMqttId };
});

app.put('/api/sites/:id', { preHandler: [app.authenticate] }, async (req, reply) => {
  const site = await db.get('SELECT * FROM sites WHERE id = $1 AND user_id = $2', req.params.id, req.user.id);
  if (!site) return reply.code(404).send({ error: 'Site not found' });
  const { name, receiver_ip, mqtt_device_id } = req.body || {};
  await db.run('UPDATE sites SET name = COALESCE($1, name), receiver_ip = COALESCE($2, receiver_ip), mqtt_device_id = COALESCE($3, mqtt_device_id) WHERE id = $4',
    name, receiver_ip, mqtt_device_id, site.id);
  return { success: true };
});

app.delete('/api/sites/:id', { preHandler: [app.authenticate] }, async (req, reply) => {
  const site = await db.get('SELECT * FROM sites WHERE id = $1 AND user_id = $2', req.params.id, req.user.id);
  if (!site) return reply.code(404).send({ error: 'Site not found' });
  await revokeMqttCredentials(site.id);
  await db.run('DELETE FROM sites WHERE id = $1', site.id);
  return { success: true };
});

// ─── DEVICES ───────────────────────────────────────────────────────────────────

app.get('/api/sites/:siteId/devices', { preHandler: [app.authenticate] }, async (req, reply) => {
  const site = await db.get('SELECT * FROM sites WHERE id = $1 AND user_id = $2', req.params.siteId, req.user.id);
  if (!site) return reply.code(404).send({ error: 'Site not found' });
  return stmts.getDevicesBySite(site.id);
});

app.post('/api/sites/:siteId/devices', { preHandler: [app.authenticate] }, async (req, reply) => {
  const site = await db.get('SELECT * FROM sites WHERE id = $1 AND user_id = $2', req.params.siteId, req.user.id);
  if (!site) return reply.code(404).send({ error: 'Site not found' });
  const { lora_address, name, tank_capacity_l, min_distance_cm, max_distance_cm, alert_low_pct, alert_high_pct } = req.body || {};
  if (!lora_address) return reply.code(400).send({ error: 'LoRa address required' });
  const result = await db.run(
    'INSERT INTO devices (site_id, lora_address, name, tank_capacity_l, min_distance_cm, max_distance_cm, alert_low_pct, alert_high_pct) VALUES ($1,$2,$3,$4,$5,$6,$7,$8)',
    site.id, lora_address, name || 'Tank', tank_capacity_l || 1000, min_distance_cm || 10, max_distance_cm || 150, alert_low_pct || 20, alert_high_pct || 95
  );
  return { id: result.lastInsertRowid };
});

app.put('/api/devices/:id', { preHandler: [app.authenticate] }, async (req, reply) => {
  const device = await db.get(
    'SELECT d.*, s.receiver_ip FROM devices d JOIN sites s ON d.site_id = s.id WHERE d.id = $1 AND s.user_id = $2',
    req.params.id, req.user.id
  );
  if (!device) return reply.code(404).send({ error: 'Device not found' });
  const { name, tank_capacity_l, min_distance_cm, max_distance_cm, alert_low_pct, alert_high_pct } = req.body || {};

  await db.run(
    `UPDATE devices SET name=COALESCE($1,name), tank_capacity_l=COALESCE($2,tank_capacity_l),
     min_distance_cm=COALESCE($3,min_distance_cm), max_distance_cm=COALESCE($4,max_distance_cm),
     alert_low_pct=COALESCE($5,alert_low_pct), alert_high_pct=COALESCE($6,alert_high_pct) WHERE id=$7`,
    name, tank_capacity_l, min_distance_cm, max_distance_cm, alert_low_pct, alert_high_pct, device.id
  );

  let receiver_synced = false;
  if (device.receiver_ip) {
    const updated = await db.get('SELECT * FROM devices WHERE id = $1', device.id);
    try {
      const res = await fetch(`http://${device.receiver_ip}/api/transmitters`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ addr: updated.lora_address, name: updated.name, min_dist: updated.min_distance_cm, max_dist: updated.max_distance_cm, capacity: updated.tank_capacity_l }),
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
  const result = await db.run(
    'DELETE FROM devices WHERE id = $1 AND site_id IN (SELECT id FROM sites WHERE user_id = $2)',
    req.params.id, req.user.id
  );
  if (result.changes === 0) return reply.code(404).send({ error: 'Device not found' });
  return { success: true };
});

// ─── QR CODE DEVICE LINKING ────────────────────────────────────────────────────
// The phone (on LAN) verifies the receiver token and discovers devices,
// then sends the pre-verified data here. The server never contacts the receiver.

app.post('/api/link/claim', { preHandler: [app.authenticate] }, async (req, reply) => {
  const { device_id, receiver_ip, tanks, transmitters } = req.body || {};
  if (!device_id || !receiver_ip) return reply.code(400).send({ error: 'Missing device_id or receiver_ip' });

  // Check if already linked
  const existing = await db.get('SELECT id FROM sites WHERE user_id = $1 AND mqtt_device_id = $2', req.user.id, device_id);
  if (existing) {
    // Still generate MQTT creds if missing
    let mqttCreds = null;
    try { mqttCreds = await generateMqttCredentials(req.user.id, existing.id, device_id); } catch {}
    return { site_id: existing.id, message: 'Device already linked', already_linked: true, mqtt: mqttCreds };
  }

  // Create site
  const site = await db.run('INSERT INTO sites (user_id, name, receiver_ip, mqtt_device_id) VALUES ($1, $2, $3, $4)',
    req.user.id, 'My Tank', receiver_ip, device_id);
  const siteId = site.lastInsertRowid;

  // Add discovered tanks (sent by the phone)
  let deviceCount = 0;
  if (tanks?.length > 0) {
    for (const tank of tanks) {
      try {
        await db.run('INSERT INTO devices (site_id, lora_address, name) VALUES ($1, $2, $3)',
          siteId, tank.address, tank.name || `Tank ${tank.address}`);
        deviceCount++;
      } catch {}
    }
  }

  // Update with transmitter details
  if (transmitters?.length > 0) {
    for (const tx of transmitters) {
      await db.run('UPDATE devices SET tank_capacity_l=$1, min_distance_cm=$2, max_distance_cm=$3, fw_version=$4 WHERE site_id=$5 AND lora_address=$6',
        tx.capacity, tx.min_dist, tx.max_dist, tx.fw_version || null, siteId, tx.address);
    }
  }

  // Generate MQTT credentials (server-side only — phone pushes to receiver)
  let mqttCreds = null;
  try {
    mqttCreds = await generateMqttCredentials(req.user.id, siteId, device_id);
  } catch (err) {
    app.log.warn(`MQTT credential setup failed: ${err.message}`);
  }

  return {
    site_id: siteId,
    device_count: deviceCount,
    mqtt: mqttCreds,
    message: 'Device linked successfully',
  };
});

// ─── HISTORY ───────────────────────────────────────────────────────────────────

app.get('/api/devices/:id/history', { preHandler: [app.authenticate] }, async (req, reply) => {
  const device = await db.get(
    'SELECT d.* FROM devices d JOIN sites s ON d.site_id = s.id WHERE d.id = $1 AND s.user_id = $2',
    req.params.id, req.user.id
  );
  if (!device) return reply.code(404).send({ error: 'Device not found' });
  const range = req.query.range || '24h';
  const rangeMap = { '1h': '-1 hours', '6h': '-6 hours', '24h': '-1 days', '7d': '-7 days', '30d': '-30 days', '90d': '-90 days' };
  return stmts.getReadings(device.id, rangeMap[range] || '-1 days');
});

// ─── ALERTS ────────────────────────────────────────────────────────────────────

app.get('/api/alerts', { preHandler: [app.authenticate] }, async (req) => {
  const page = parseInt(req.query.page || '1');
  const limit = Math.min(parseInt(req.query.limit || '50'), 100);
  const offset = (page - 1) * limit;
  const alerts = await stmts.getAlerts(req.user.id, limit, offset);
  const countRow = await stmts.getUnackAlertCount(req.user.id);
  return { alerts, unread: countRow?.count || 0 };
});

app.put('/api/alerts/:id/ack', { preHandler: [app.authenticate] }, async (req) => {
  await stmts.ackAlert(req.params.id);
  return { success: true };
});

app.put('/api/alerts/ack-all', { preHandler: [app.authenticate] }, async (req) => {
  await db.run(
    'UPDATE alerts SET acknowledged = 1 WHERE device_id IN (SELECT d.id FROM devices d JOIN sites s ON d.site_id = s.id WHERE s.user_id = $1)',
    req.user.id
  );
  return { success: true };
});

// ─── PUSH SUBSCRIPTIONS ───────────────────────────────────────────────────────

app.post('/api/push/subscribe', { preHandler: [app.authenticate] }, async (req) => {
  const { endpoint, keys } = req.body || {};
  if (!endpoint || !keys?.p256dh || !keys?.auth) return { error: 'Invalid subscription' };
  await db.run(
    'INSERT INTO push_subs (user_id, endpoint, p256dh, auth) VALUES ($1, $2, $3, $4) ON CONFLICT (endpoint) DO UPDATE SET user_id=$1, p256dh=$3, auth=$4',
    req.user.id, endpoint, keys.p256dh, keys.auth
  );
  return { success: true };
});

app.post('/api/push/unsubscribe', { preHandler: [app.authenticate] }, async (req) => {
  const { endpoint } = req.body || {};
  await db.run('DELETE FROM push_subs WHERE user_id = $1 AND endpoint = $2', req.user.id, endpoint);
  return { success: true };
});

// ─── RECEIVER PROXY ────────────────────────────────────────────────────────────

app.get('/api/sites/:siteId/proxy/data', { preHandler: [app.authenticate] }, async (req, reply) => {
  const site = await db.get('SELECT * FROM sites WHERE id = $1 AND user_id = $2', req.params.siteId, req.user.id);
  if (!site?.receiver_ip) return reply.code(400).send({ error: 'No receiver IP configured' });
  try {
    const resp = await fetch(`http://${site.receiver_ip}/api/data`, { signal: AbortSignal.timeout(5000) });
    return resp.json();
  } catch { return reply.code(502).send({ error: 'Receiver unreachable' }); }
});

app.get('/api/sites/:siteId/proxy/system', { preHandler: [app.authenticate] }, async (req, reply) => {
  const site = await db.get('SELECT * FROM sites WHERE id = $1 AND user_id = $2', req.params.siteId, req.user.id);
  if (!site?.receiver_ip) return reply.code(400).send({ error: 'No receiver IP configured' });
  try {
    const resp = await fetch(`http://${site.receiver_ip}/api/system`, { signal: AbortSignal.timeout(5000) });
    return resp.json();
  } catch { return reply.code(502).send({ error: 'Receiver unreachable' }); }
});

app.get('/api/sites/:siteId/proxy/ota/state', { preHandler: [app.authenticate] }, async (req, reply) => {
  const site = await db.get('SELECT * FROM sites WHERE id = $1 AND user_id = $2', req.params.siteId, req.user.id);
  if (!site?.receiver_ip) return reply.code(400).send({ error: 'No receiver IP configured' });
  try {
    const resp = await fetch(`http://${site.receiver_ip}/api/ota/state`, { signal: AbortSignal.timeout(5000) });
    return resp.json();
  } catch { return reply.code(502).send({ error: 'Receiver unreachable' }); }
});

app.post('/api/sites/:siteId/proxy/ota/upload', { preHandler: [app.authenticate] }, async (req, reply) => {
  const site = await db.get('SELECT * FROM sites WHERE id = $1 AND user_id = $2', req.params.siteId, req.user.id);
  if (!site?.receiver_ip) return reply.code(400).send({ error: 'No receiver IP configured' });
  try {
    const body = await req.body;
    const resp = await fetch(`http://${site.receiver_ip}/api/ota/upload`, {
      method: 'POST', headers: { 'Content-Type': 'application/octet-stream' },
      body, signal: AbortSignal.timeout(120000)
    });
    return resp.json();
  } catch { return reply.code(502).send({ error: 'OTA upload failed' }); }
});

// ─── VAPID PUBLIC KEY ─────────────────────────────────────────────────────────

app.get('/api/push/vapid-key', async () => {
  return { key: process.env.VAPID_PUBLIC_KEY || '' };
});

// ─── SSE ──────────────────────────────────────────────────────────────────────

app.get('/events', async (req, reply) => {
  const token = req.query.token;
  if (!token) return reply.code(401).send({ error: 'Token required' });
  try {
    const decoded = app.jwt.verify(token);
    req.user = decoded;
  } catch { return reply.code(401).send({ error: 'Invalid token' }); }

  reply.raw.writeHead(200, {
    'Content-Type': 'text/event-stream', 'Cache-Control': 'no-cache',
    'Connection': 'keep-alive', 'X-Accel-Buffering': 'no'
  });
  reply.raw.write(`data: ${JSON.stringify({ type: 'connected' })}\n\n`);
  addClient(req.user.id, reply);
  const keepAlive = setInterval(() => {
    try { reply.raw.write(': keepalive\n\n'); } catch { clearInterval(keepAlive); }
  }, 30000);
  req.raw.on('close', () => clearInterval(keepAlive));
  await new Promise(() => {});
});

// ─── SERVE STATIC ─────────────────────────────────────────────────────────────

const distPath = join(__dirname, '..', 'dist');
if (isProd && existsSync(distPath)) {
  await app.register(fstatic, { root: distPath, prefix: '/' });
  app.setNotFoundHandler((req, reply) => {
    if (req.url.startsWith('/api/') || req.url.startsWith('/events')) return reply.code(404).send({ error: 'Not found' });
    return reply.sendFile('index.html');
  });
}

// ─── START ─────────────────────────────────────────────────────────────────────

connectMqtt();
setInterval(async () => {
  try { await checkDeviceTimeouts(); } catch (err) { console.error('[Alerts] Timeout check error:', err.message); }
}, 60_000);

try {
  await app.listen({ port: PORT, host: '0.0.0.0' });
  console.log(`TankSync Cloud server running on port ${PORT}`);
} catch (err) {
  app.log.error(err);
  process.exit(1);
}
