// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

import Database from 'better-sqlite3';
import { join, dirname } from 'path';
import { fileURLToPath } from 'url';
import { mkdirSync } from 'fs';

const __dirname = dirname(fileURLToPath(import.meta.url));
const DB_DIR = join(__dirname, '..', 'data');
mkdirSync(DB_DIR, { recursive: true });

const db = new Database(join(DB_DIR, 'tanksync.db'), { wal: true });

// Performance tuning
db.pragma('journal_mode = WAL');
db.pragma('synchronous = NORMAL');
db.pragma('foreign_keys = ON');

// Schema
db.exec(`
  CREATE TABLE IF NOT EXISTS users (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    email TEXT UNIQUE NOT NULL,
    password_hash TEXT NOT NULL,
    name TEXT NOT NULL DEFAULT '',
    mode TEXT NOT NULL DEFAULT 'home',
    created_at TEXT NOT NULL DEFAULT (datetime('now'))
  );

  CREATE TABLE IF NOT EXISTS sites (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    name TEXT NOT NULL,
    receiver_ip TEXT,
    mqtt_device_id TEXT,
    created_at TEXT NOT NULL DEFAULT (datetime('now'))
  );

  CREATE TABLE IF NOT EXISTS devices (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    site_id INTEGER NOT NULL REFERENCES sites(id) ON DELETE CASCADE,
    lora_address INTEGER NOT NULL,
    name TEXT NOT NULL DEFAULT 'Tank',
    tank_capacity_l REAL DEFAULT 1000,
    min_distance_cm REAL DEFAULT 10,
    max_distance_cm REAL DEFAULT 150,
    alert_low_pct INTEGER DEFAULT 20,
    alert_high_pct INTEGER DEFAULT 95,
    fw_version TEXT,
    last_seen TEXT,
    last_water_pct REAL,
    last_battery_pct REAL,
    last_rssi INTEGER,
    last_battery_v REAL,
    state TEXT DEFAULT 'waiting'
  );

  CREATE TABLE IF NOT EXISTS readings (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    water_pct REAL,
    water_liters REAL,
    battery_pct REAL,
    battery_v REAL,
    rssi INTEGER,
    distance_cm REAL,
    recorded_at TEXT NOT NULL DEFAULT (datetime('now'))
  );

  CREATE INDEX IF NOT EXISTS idx_readings_device_time ON readings(device_id, recorded_at);

  CREATE TABLE IF NOT EXISTS alerts (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    device_id INTEGER NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
    type TEXT NOT NULL,
    message TEXT NOT NULL,
    acknowledged INTEGER DEFAULT 0,
    created_at TEXT NOT NULL DEFAULT (datetime('now'))
  );

  CREATE INDEX IF NOT EXISTS idx_alerts_device ON alerts(device_id, created_at);

  CREATE TABLE IF NOT EXISTS push_subs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    endpoint TEXT NOT NULL UNIQUE,
    p256dh TEXT NOT NULL,
    auth TEXT NOT NULL,
    created_at TEXT NOT NULL DEFAULT (datetime('now'))
  );
`);

// Prepared statements for hot paths
export const stmts = {
  insertReading: db.prepare(`
    INSERT INTO readings (device_id, water_pct, water_liters, battery_pct, battery_v, rssi, distance_cm)
    VALUES (?, ?, ?, ?, ?, ?, ?)
  `),
  updateDeviceLive: db.prepare(`
    UPDATE devices SET
      last_water_pct = ?, last_battery_pct = ?, last_rssi = ?,
      last_battery_v = ?, last_seen = datetime('now'), state = ?, fw_version = COALESCE(?, fw_version)
    WHERE id = ?
  `),
  getDeviceBySiteAndAddr: db.prepare(`
    SELECT d.* FROM devices d
    JOIN sites s ON d.site_id = s.id
    WHERE s.mqtt_device_id = ? AND d.lora_address = ?
  `),
  getDevicesBySite: db.prepare(`
    SELECT * FROM devices WHERE site_id = ? ORDER BY name
  `),
  getSiteByMqttId: db.prepare(`
    SELECT * FROM sites WHERE mqtt_device_id = ?
  `),
  getSitesByUser: db.prepare(`
    SELECT * FROM sites WHERE user_id = ? ORDER BY name
  `),
  getUserByEmail: db.prepare(`
    SELECT * FROM users WHERE email = ?
  `),
  getUserById: db.prepare(`
    SELECT id, email, name, mode, created_at FROM users WHERE id = ?
  `),
  getAlerts: db.prepare(`
    SELECT a.*, d.name as device_name, s.name as site_name
    FROM alerts a
    JOIN devices d ON a.device_id = d.id
    JOIN sites s ON d.site_id = s.id
    WHERE s.user_id = ?
    ORDER BY a.created_at DESC
    LIMIT ? OFFSET ?
  `),
  getUnackAlertCount: db.prepare(`
    SELECT COUNT(*) as count FROM alerts a
    JOIN devices d ON a.device_id = d.id
    JOIN sites s ON d.site_id = s.id
    WHERE s.user_id = ? AND a.acknowledged = 0
  `),
  insertAlert: db.prepare(`
    INSERT INTO alerts (device_id, type, message) VALUES (?, ?, ?)
  `),
  ackAlert: db.prepare(`
    UPDATE alerts SET acknowledged = 1 WHERE id = ?
  `),
  getReadings: db.prepare(`
    SELECT water_pct, water_liters, battery_pct, battery_v, rssi, distance_cm, recorded_at
    FROM readings WHERE device_id = ? AND recorded_at >= datetime('now', ?)
    ORDER BY recorded_at ASC
  `),
  cleanOldReadings: db.prepare(`
    DELETE FROM readings WHERE recorded_at < datetime('now', '-90 days')
  `),
  getPushSubs: db.prepare(`
    SELECT ps.* FROM push_subs ps
    JOIN users u ON ps.user_id = u.id
    JOIN sites s ON s.user_id = u.id
    JOIN devices d ON d.site_id = s.id
    WHERE d.id = ?
  `),
  getAllDevicesWithSites: db.prepare(`
    SELECT d.*, s.name as site_name, s.mqtt_device_id, s.user_id
    FROM devices d JOIN sites s ON d.site_id = s.id
  `)
};

export default db;
