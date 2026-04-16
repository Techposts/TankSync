// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

import { randomBytes } from 'crypto';
import { execSync } from 'child_process';
import { readFileSync, writeFileSync } from 'fs';
import db from './db.js';

const PASSWD_FILE = '/etc/mosquitto/tanksync_passwd';
const ACL_FILE = '/etc/mosquitto/tanksync_acl.conf';
const MQTT_PUBLIC_HOST = process.env.MQTT_PUBLIC_HOST || 'mqtt.smartghar.org';
const MQTT_PUBLIC_PORT = parseInt(process.env.MQTT_PUBLIC_PORT || '8883');

/**
 * Generate MQTT credentials for a user+site, add to Mosquitto, push to receiver.
 * Returns { mqtt_username, mqtt_password, mqtt_host, mqtt_port }
 */
export async function generateMqttCredentials(userId, siteId, deviceId) {
  // Check if credentials already exist for this site
  const existing = await db.get(
    'SELECT * FROM mqtt_credentials WHERE site_id = $1', siteId
  );
  if (existing) {
    return {
      mqtt_username: existing.mqtt_username,
      mqtt_password: existing.mqtt_password,
      mqtt_host: MQTT_PUBLIC_HOST,
      mqtt_port: MQTT_PUBLIC_PORT,
    };
  }

  // Generate unique username and password
  const shortId = deviceId.slice(0, 6);
  const mqtt_username = `ts_u${userId}_${shortId}`;
  const mqtt_password = randomBytes(16).toString('base64url');

  // Add to Mosquitto password file
  try {
    execSync(`sudo mosquitto_passwd -b ${PASSWD_FILE} "${mqtt_username}" "${mqtt_password}"`, { timeout: 5000 });
  } catch (err) {
    console.error(`[MQTT-CRED] Failed to add password: ${err.message}`);
    throw new Error('Failed to create MQTT credentials');
  }

  // Store in database
  await db.run(
    'INSERT INTO mqtt_credentials (user_id, site_id, mqtt_username, mqtt_password, device_id) VALUES ($1, $2, $3, $4, $5)',
    userId, siteId, mqtt_username, mqtt_password, deviceId
  );

  // Regenerate ACL file
  await regenerateAcl();

  // Reload Mosquitto config
  try {
    execSync('sudo kill -HUP $(pidof mosquitto)', { timeout: 5000 });
  } catch {
    console.warn('[MQTT-CRED] Could not reload Mosquitto — may need manual restart');
  }

  return { mqtt_username, mqtt_password, mqtt_host: MQTT_PUBLIC_HOST, mqtt_port: MQTT_PUBLIC_PORT };
}

/**
 * Push MQTT config to receiver via its local HTTP API.
 */
export async function pushMqttToReceiver(receiverIp, mqttCreds) {
  try {
    const res = await fetch(`http://${receiverIp}/api/mqtt`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        host: mqttCreds.mqtt_host,
        port: mqttCreds.mqtt_port,
        user: mqttCreds.mqtt_username,
        pass: mqttCreds.mqtt_password,
        enabled: true,
        ha_discovery: false,
        use_tls: true,
      }),
      signal: AbortSignal.timeout(5000),
    });
    return res.ok;
  } catch (err) {
    console.warn(`[MQTT-CRED] Could not push to receiver ${receiverIp}: ${err.message}`);
    return false;
  }
}

/**
 * Revoke MQTT credentials for a site (called when site is deleted).
 */
export async function revokeMqttCredentials(siteId) {
  const cred = await db.get('SELECT * FROM mqtt_credentials WHERE site_id = $1', siteId);
  if (!cred) return;

  // Remove from Mosquitto password file
  try {
    execSync(`sudo mosquitto_passwd -D ${PASSWD_FILE} "${cred.mqtt_username}"`, { timeout: 5000 });
  } catch {}

  // Remove from DB
  await db.run('DELETE FROM mqtt_credentials WHERE site_id = $1', siteId);

  // Regenerate ACL and reload
  await regenerateAcl();
  try { execSync('sudo kill -HUP $(pidof mosquitto)', { timeout: 5000 }); } catch {}
}

/**
 * Regenerate the ACL file from all credentials in the database.
 */
async function regenerateAcl() {
  const allCreds = await db.all('SELECT * FROM mqtt_credentials');

  let acl = `# TankSync MQTT ACL — auto-generated, do not edit manually
# Server account — full access
user tanksync_server
topic readwrite tanksync/#

`;

  for (const cred of allCreds) {
    acl += `# User ${cred.user_id}, Site ${cred.site_id}\n`;
    acl += `user ${cred.mqtt_username}\n`;
    acl += `topic readwrite tanksync/${cred.device_id}/#\n\n`;
  }

  writeFileSync(ACL_FILE, acl);
}
