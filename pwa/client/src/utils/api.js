// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

const BASE = '';

function getToken() {
  return localStorage.getItem('tanksync_token');
}

export function setToken(token) {
  localStorage.setItem('tanksync_token', token);
}

export function clearToken() {
  localStorage.removeItem('tanksync_token');
}

export function isAuthenticated() {
  return !!getToken();
}

async function request(method, path, body) {
  const headers = {};
  const token = getToken();
  if (token) headers['Authorization'] = `Bearer ${token}`;

  const opts = { method, headers };
  if (body !== undefined && body !== null) {
    headers['Content-Type'] = 'application/json';
    opts.body = JSON.stringify(body);
  }

  const res = await fetch(`${BASE}${path}`, opts);

  if (res.status === 401) {
    clearToken();
    window.location.href = '/login';
    throw new Error('Unauthorized');
  }

  const data = await res.json();
  if (!res.ok) throw new Error(data.error || 'Request failed');
  return data;
}

export const api = {
  get: (path) => request('GET', path),
  post: (path, body) => request('POST', path, body),
  put: (path, body) => request('PUT', path, body),
  delete: (path) => request('DELETE', path),
};

// SSE connection for real-time updates
export function connectSSE(onMessage) {
  const token = getToken();
  if (!token) return null;

  const es = new EventSource(`/events?token=${token}`);
  // Note: EventSource doesn't support Authorization header,
  // so we'll need to handle auth via query param on the server.
  // For now, use a wrapper approach.

  es.onmessage = (event) => {
    try {
      const data = JSON.parse(event.data);
      onMessage(data);
    } catch {}
  };

  es.onerror = () => {
    // Will auto-reconnect
  };

  return es;
}
