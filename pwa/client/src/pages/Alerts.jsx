// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

import { useState, useEffect, useCallback } from 'react';
import { api } from '../utils/api.js';

const ALERT_ICONS = {
  low_water: { color: 'text-danger', bg: 'bg-danger/10', icon: '!' },
  high_water: { color: 'text-warning', bg: 'bg-warning/10', icon: '~' },
  low_battery: { color: 'text-warning', bg: 'bg-warning/10', icon: 'B' },
  offline: { color: 'text-slate-400', bg: 'bg-slate-700/50', icon: 'X' },
};

export default function Alerts() {
  const [alerts, setAlerts] = useState([]);
  const [unread, setUnread] = useState(0);
  const [loading, setLoading] = useState(true);
  const [filter, setFilter] = useState('all');

  const load = useCallback(async () => {
    try {
      const data = await api.get('/api/alerts?limit=100');
      setAlerts(data.alerts);
      setUnread(data.unread);
    } catch {} finally { setLoading(false); }
  }, []);

  useEffect(() => { load(); }, [load]);

  const ackAll = async () => {
    await api.put('/api/alerts/ack-all');
    load();
  };

  const ackOne = async (id) => {
    await api.put(`/api/alerts/${id}/ack`);
    load();
  };

  const filtered = filter === 'all'
    ? alerts
    : alerts.filter(a => a.type === filter);

  return (
    <div className="px-4 pt-2 pb-24">
      <div className="flex items-center justify-between mb-4">
        <h1 className="text-2xl font-bold text-white">Alerts</h1>
        {unread > 0 && (
          <button onClick={ackAll}
            className="text-xs text-water bg-water/10 px-3 py-1.5 rounded-lg font-medium">
            Mark All Read
          </button>
        )}
      </div>

      {/* Filter tabs */}
      <div className="flex gap-1 bg-slate-800/50 rounded-lg p-0.5 mb-4 overflow-x-auto">
        {[
          { key: 'all', label: 'All' },
          { key: 'low_water', label: 'Low Water' },
          { key: 'low_battery', label: 'Battery' },
          { key: 'offline', label: 'Offline' },
        ].map(f => (
          <button key={f.key} onClick={() => setFilter(f.key)}
            className={`px-3 py-1.5 rounded-md text-xs font-medium whitespace-nowrap transition-all ${
              filter === f.key ? 'bg-surface text-white' : 'text-slate-400'
            }`}>
            {f.label}
          </button>
        ))}
      </div>

      {loading ? (
        <div className="space-y-3">
          {[1, 2, 3].map(i => <div key={i} className="skeleton h-20 w-full rounded-xl" />)}
        </div>
      ) : filtered.length === 0 ? (
        <div className="flex flex-col items-center justify-center py-20 text-center">
          <div className="w-16 h-16 rounded-2xl bg-success/10 flex items-center justify-center mb-4">
            <svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="#22C55E" strokeWidth="2" strokeLinecap="round">
              <path d="M20 6L9 17l-5-5" />
            </svg>
          </div>
          <h3 className="text-white font-medium mb-1">All Clear</h3>
          <p className="text-slate-400 text-sm">No alerts to show</p>
        </div>
      ) : (
        <div className="space-y-2">
          {filtered.map(alert => {
            const cfg = ALERT_ICONS[alert.type] || ALERT_ICONS.offline;
            return (
              <button
                key={alert.id}
                onClick={() => !alert.acknowledged && ackOne(alert.id)}
                className={`w-full text-left bg-surface rounded-xl p-4 flex items-start gap-3 transition-all
                  ${!alert.acknowledged ? 'border-l-2 border-water' : 'opacity-60'}`}
              >
                <div className={`w-9 h-9 rounded-lg ${cfg.bg} flex items-center justify-center flex-shrink-0`}>
                  <span className={`text-sm font-bold ${cfg.color}`}>{cfg.icon}</span>
                </div>
                <div className="flex-1 min-w-0">
                  <div className="flex items-center gap-2">
                    <span className="text-sm font-medium text-white">{alert.device_name}</span>
                    <span className="text-[10px] text-slate-500">{alert.site_name}</span>
                  </div>
                  <p className="text-sm text-slate-300 mt-0.5">{alert.message}</p>
                  <span className="text-[10px] text-slate-500 mt-1 block">{formatTime(alert.created_at)}</span>
                </div>
                {!alert.acknowledged && (
                  <div className="w-2 h-2 rounded-full bg-water flex-shrink-0 mt-2" />
                )}
              </button>
            );
          })}
        </div>
      )}
    </div>
  );
}

function formatTime(dateStr) {
  if (!dateStr) return '';
  const d = new Date(dateStr + 'Z');
  const now = new Date();
  const diff = (now - d) / 1000;
  if (diff < 60) return 'Just now';
  if (diff < 3600) return `${Math.round(diff / 60)}m ago`;
  if (diff < 86400) return `${Math.round(diff / 3600)}h ago`;
  return d.toLocaleDateString(undefined, { month: 'short', day: 'numeric' });
}
