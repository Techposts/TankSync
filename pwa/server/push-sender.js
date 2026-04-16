// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

/**
 * Web Push sender using curl subprocess.
 *
 * Why curl instead of Node's fetch/https?
 *   Node.js (both https module and undici/fetch) fails to connect to
 *   Apple's web.push.apple.com on some ISP networks (tested: zeonet.co.in).
 *   curl on the same machine works fine. The root cause is likely a TLS
 *   negotiation difference between OpenSSL (Node) and the system's libcurl.
 *
 *   Using curl as a subprocess is a proven production pattern (used by
 *   several push notification services) that's reliable across all networks.
 *
 * This module uses web-push for VAPID JWT + payload encryption,
 * then sends via curl instead of Node's HTTP client.
 */

import { spawn } from 'child_process';
import { writeFileSync, unlinkSync, mkdtempSync } from 'fs';
import { join } from 'path';
import { tmpdir } from 'os';
import webpush from 'web-push';

const VAPID_PUBLIC = process.env.VAPID_PUBLIC_KEY || '';
const VAPID_PRIVATE = process.env.VAPID_PRIVATE_KEY || '';
const VAPID_SUBJECT = 'mailto:your-email@example.com';

let vapidConfigured = false;
if (VAPID_PUBLIC && VAPID_PRIVATE) {
  webpush.setVapidDetails(VAPID_SUBJECT, VAPID_PUBLIC, VAPID_PRIVATE);
  vapidConfigured = true;
}

/**
 * Send a push notification using curl.
 * @param {Object} sub - { endpoint, p256dh, auth }
 * @param {string} payload - JSON string
 * @returns {Promise<{statusCode: number}>}
 */
export async function sendPush(sub, payload) {
  if (!vapidConfigured) throw new Error('VAPID keys not configured');

  const pushSub = {
    endpoint: sub.endpoint,
    keys: { p256dh: sub.p256dh, auth: sub.auth }
  };

  // Use web-push to generate encrypted payload + VAPID auth headers
  const details = webpush.generateRequestDetails(pushSub, payload);

  // Build curl command with all the headers
  const args = [
    '-s', '-o', '/dev/null',       // silent, discard body
    '-w', '%{http_code}',          // output status code
    '--connect-timeout', '10',
    '--max-time', '15',
    '-X', details.method || 'POST',
  ];

  // Add headers
  for (const [key, value] of Object.entries(details.headers)) {
    args.push('-H', `${key}: ${value}`);
  }

  // Write encrypted body to temp file (curl reads it)
  let tmpFile = null;
  if (details.body) {
    tmpFile = join(tmpdir(), `push-${Date.now()}-${Math.random().toString(36).slice(2)}.bin`);
    writeFileSync(tmpFile, details.body);
    args.push('--data-binary', `@${tmpFile}`);
  }

  args.push(details.endpoint);

  try {
    const statusCode = await new Promise((resolve, reject) => {
      const proc = spawn('curl', args, { timeout: 20000 });
      let stdout = '';
      proc.stdout.on('data', d => { stdout += d; });
      proc.stderr.on('data', () => {}); // discard stderr
      proc.on('close', (code) => {
        if (code !== 0) return reject(new Error(`curl exited with code ${code}`));
        resolve(parseInt(stdout.trim()));
      });
      proc.on('error', reject);
    });

    if (statusCode === 201) {
      return { statusCode: 201 };
    }

    const err = new Error(`Push failed with status ${statusCode}`);
    err.statusCode = statusCode;
    throw err;
  } finally {
    if (tmpFile) try { unlinkSync(tmpFile); } catch {}
  }
}
