// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

import pg from 'pg';
const { Pool } = pg;

const pool = new Pool({
  connectionString: process.env.DATABASE_URL || 'postgresql://tanksync:tanksync@localhost:5432/tanksync',
  max: 10,
});

// Schema
await pool.query(`
  CREATE TABLE IF NOT EXISTS users (
    id SERIAL PRIMARY KEY,
    email TEXT UNIQUE NOT NULL,
    password_hash TEXT NOT NULL,
    name TEXT NOT NULL DEFAULT '',
    mode TEXT NOT NULL DEFAULT 'home',
    email_verified INTEGER NOT NULL DEFAULT 0,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
  );

  CREATE TABLE IF NOT EXISTS verification_codes (
    id SERIAL PRIMARY KEY,
    email TEXT NOT NULL,
    code TEXT NOT NULL,
    expires_at TIMESTAMPTZ NOT NULL,
    used INTEGER NOT NULL DEFAULT 0,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
  );

  CREATE TABLE IF NOT EXISTS sites (
    id SERIAL PRIMARY KEY,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    name TEXT NOT NULL,
    receiver_ip TEXT,
    mqtt_device_id TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
  );

  CREATE TABLE IF NOT EXISTS devices (
    id SERIAL PRIMARY KEY,
    site_id INTEGER NOT NULL REFERENCES sites(id) ON DELETE CASCADE,
    lora_address INTEGER NOT NULL,
    name TEXT NOT NULL DEFAULT 'Tank',
    tank_capacity_l REAL DEFAULT 1000,
    min_distance_cm REAL DEFAULT 10,
    max_distance_cm REAL DEFAULT 150,
    alert_low_pct INTEGER DEFAULT 20,
    alert_high_pct INTEGER DEFAULT 95,
    fw_version TEXT,
    last_seen TIMESTAMPTZ,
    last_water_pct REAL,
    last_battery_pct REAL,
    last_rssi INTEGER,
    last_battery_v REAL,
    state TEXT DEFAULT 'waiting'
  );

  CREATE TABLE IF NOT EXISTS readings (
    id SERIAL PRIMARY KEY,
    device_id INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    water_pct REAL,
    water_liters REAL,
    battery_pct REAL,
    battery_v REAL,
    rssi INTEGER,
    distance_cm REAL,
    recorded_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
  );

  CREATE INDEX IF NOT EXISTS idx_readings_device_time ON readings(device_id, recorded_at);

  CREATE TABLE IF NOT EXISTS alerts (
    id SERIAL PRIMARY KEY,
    device_id INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    type TEXT NOT NULL,
    message TEXT NOT NULL,
    acknowledged INTEGER DEFAULT 0,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
  );

  CREATE INDEX IF NOT EXISTS idx_alerts_device ON alerts(device_id, created_at);

  CREATE TABLE IF NOT EXISTS push_subs (
    id SERIAL PRIMARY KEY,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    endpoint TEXT NOT NULL UNIQUE,
    p256dh TEXT NOT NULL,
    auth TEXT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
  );

  CREATE TABLE IF NOT EXISTS mqtt_credentials (
    id SERIAL PRIMARY KEY,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    site_id INTEGER NOT NULL REFERENCES sites(id) ON DELETE CASCADE,
    mqtt_username TEXT NOT NULL UNIQUE,
    mqtt_password TEXT NOT NULL,
    device_id TEXT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT NOW()
  );
`);

// Async query helpers that match the SQLite API shape
const db = {
  async get(sql, ...params) {
    const { rows } = await pool.query(sql, params);
    return rows[0] || undefined;
  },
  async all(sql, ...params) {
    const { rows } = await pool.query(sql, params);
    return rows;
  },
  async run(sql, ...params) {
    const needsReturning = /^\s*INSERT/i.test(sql) && !/RETURNING/i.test(sql);
    const finalSql = needsReturning ? sql.replace(/;?\s*$/, ' RETURNING id') : sql;
    const { rows, rowCount } = await pool.query(finalSql, params);
    return { changes: rowCount, lastInsertRowid: rows[0]?.id ?? null };
  },
  pool,
};

// Pre-built query functions (replacing SQLite prepared statements)
export const stmts = {
  async insertReading(device_id, water_pct, water_liters, battery_pct, battery_v, rssi, distance_cm) {
    return db.run(
      'INSERT INTO readings (device_id, water_pct, water_liters, battery_pct, battery_v, rssi, distance_cm) VALUES ($1,$2,$3,$4,$5,$6,$7)',
      device_id, water_pct, water_liters, battery_pct, battery_v, rssi, distance_cm
    );
  },
  async updateDeviceLive(water_pct, battery_pct, rssi, battery_v, state, fw_version, id) {
    return db.run(
      `UPDATE devices SET last_water_pct=$1, last_battery_pct=$2, last_rssi=$3, last_battery_v=$4, last_seen=NOW(), state=$5, fw_version=COALESCE($6, fw_version) WHERE id=$7`,
      water_pct, battery_pct, rssi, battery_v, state, fw_version, id
    );
  },
  async getDeviceBySiteAndAddr(mqtt_device_id, lora_address) {
    return db.get(
      'SELECT d.* FROM devices d JOIN sites s ON d.site_id = s.id WHERE s.mqtt_device_id = $1 AND d.lora_address = $2',
      mqtt_device_id, lora_address
    );
  },
  async getDevicesBySite(site_id) {
    return db.all('SELECT * FROM devices WHERE site_id = $1 ORDER BY name', site_id);
  },
  async getSiteByMqttId(mqtt_device_id) {
    return db.get('SELECT * FROM sites WHERE mqtt_device_id = $1', mqtt_device_id);
  },
  async getSitesByUser(user_id) {
    return db.all('SELECT * FROM sites WHERE user_id = $1 ORDER BY name', user_id);
  },
  async getUserByEmail(email) {
    return db.get('SELECT * FROM users WHERE email = $1', email);
  },
  async getUserById(id) {
    return db.get('SELECT id, email, name, mode, email_verified, created_at FROM users WHERE id = $1', id);
  },
  async getAlerts(user_id, limit, offset) {
    return db.all(
      `SELECT a.*, d.name as device_name, s.name as site_name FROM alerts a
       JOIN devices d ON a.device_id = d.id JOIN sites s ON d.site_id = s.id
       WHERE s.user_id = $1 ORDER BY a.created_at DESC LIMIT $2 OFFSET $3`,
      user_id, limit, offset
    );
  },
  async getUnackAlertCount(user_id) {
    return db.get(
      `SELECT COUNT(*) as count FROM alerts a JOIN devices d ON a.device_id = d.id
       JOIN sites s ON d.site_id = s.id WHERE s.user_id = $1 AND a.acknowledged = 0`,
      user_id
    );
  },
  async insertAlert(device_id, type, message) {
    return db.run('INSERT INTO alerts (device_id, type, message) VALUES ($1,$2,$3)', device_id, type, message);
  },
  async ackAlert(id) {
    return db.run('UPDATE alerts SET acknowledged = 1 WHERE id = $1', id);
  },
  async getReadings(device_id, interval) {
    return db.all(
      `SELECT water_pct, water_liters, battery_pct, battery_v, rssi, distance_cm, recorded_at
       FROM readings WHERE device_id = $1 AND recorded_at >= NOW() + $2::interval
       ORDER BY recorded_at ASC`,
      device_id, interval
    );
  },
  async cleanOldReadings() {
    return db.run("DELETE FROM readings WHERE recorded_at < NOW() - interval '90 days'");
  },
  async getPushSubs(device_id) {
    return db.all(
      `SELECT ps.* FROM push_subs ps JOIN users u ON ps.user_id = u.id
       JOIN sites s ON s.user_id = u.id JOIN devices d ON d.site_id = s.id WHERE d.id = $1`,
      device_id
    );
  },
  async getAllDevicesWithSites() {
    return db.all(
      'SELECT d.*, s.name as site_name, s.mqtt_device_id, s.user_id FROM devices d JOIN sites s ON d.site_id = s.id'
    );
  },
};

export default db;
