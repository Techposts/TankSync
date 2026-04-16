// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

// Server-Sent Events for real-time updates to the frontend

const clients = new Map(); // userId -> Set<response>

export function addClient(userId, reply) {
  if (!clients.has(userId)) clients.set(userId, new Set());
  clients.get(userId).add(reply);

  reply.raw.on('close', () => {
    clients.get(userId)?.delete(reply);
    if (clients.get(userId)?.size === 0) clients.delete(userId);
  });
}

export function broadcast(data) {
  const msg = `data: ${JSON.stringify(data)}\n\n`;
  for (const [, responses] of clients) {
    for (const reply of responses) {
      try { reply.raw.write(msg); } catch {}
    }
  }
}

export function broadcastToUser(userId, data) {
  const msg = `data: ${JSON.stringify(data)}\n\n`;
  const responses = clients.get(userId);
  if (!responses) return;
  for (const reply of responses) {
    try { reply.raw.write(msg); } catch {}
  }
}
