// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

import { useState, useEffect, useCallback } from 'react';
import { useParams, useNavigate, useSearchParams } from 'react-router-dom';
import { api } from '../utils/api.js';
import { useSites } from '../hooks/useSites.jsx';
import TankVisualization from '../components/TankVisualization.jsx';

const RANGES = [
  { key: '1h', label: '1H' },
  { key: '6h', label: '6H' },
  { key: '24h', label: '24H' },
  { key: '7d', label: '7D' },
  { key: '30d', label: '30D' },
];

export default function TankDetail() {
  const { id } = useParams();
  const navigate = useNavigate();
  const [searchParams] = useSearchParams();
  const { activeSite, updateDevice, deleteDevice, refresh } = useSites();
  const [history, setHistory] = useState([]);
  const [range, setRange] = useState('24h');
  const [loading, setLoading] = useState(true);
  const [editing, setEditing] = useState(false);
  const [editForm, setEditForm] = useState({});
  const [saving, setSaving] = useState(false);
  const [syncStatus, setSyncStatus] = useState(null); // null | 'synced' | 'failed'
  const [chartMode, setChartMode] = useState(searchParams.get('chart') === 'battery' ? 'battery' : 'water');
  const [showDeleteConfirm, setShowDeleteConfirm] = useState(false);

  const device = activeSite?.devices?.find(d => d.id === parseInt(id));

  const loadHistory = useCallback(async () => {
    try {
      const data = await api.get(`/api/devices/${id}/history?range=${range}`);
      setHistory(data);
    } catch {} finally { setLoading(false); }
  }, [id, range]);

  useEffect(() => { loadHistory(); }, [loadHistory]);

  useEffect(() => {
    const interval = setInterval(() => { refresh(); loadHistory(); }, 15000);
    return () => clearInterval(interval);
  }, [refresh, loadHistory]);

  if (!device) {
    return (
      <div className="flex items-center justify-center min-h-[60vh]">
        <div className="text-slate-400">Device not found</div>
      </div>
    );
  }

  const startEdit = () => {
    setEditForm({
      name: device.name,
      tank_capacity_l: device.tank_capacity_l,
      min_distance_cm: device.min_distance_cm,
      max_distance_cm: device.max_distance_cm,
      alert_low_pct: device.alert_low_pct,
      alert_high_pct: device.alert_high_pct,
      sleep_s: device.sleep_s || 300,
      samples: device.samples || 5,
    });
    setEditing(true);
  };

  const handleSave = async () => {
    setSaving(true);
    setSyncStatus(null);
    try {
      const result = await updateDevice(device.id, editForm);
      setEditing(false);
      setSyncStatus(result.receiver_synced ? 'synced' : 'failed');
      setTimeout(() => setSyncStatus(null), 5000);
    } catch {} finally { setSaving(false); }
  };

  const handleDelete = async () => {
    await deleteDevice(device.id);
    navigate('/');
  };

  return (
    <div className="px-4 pt-2 pb-32">
      {/* Header */}
      <div className="flex items-center justify-between mb-4">
        <button onClick={() => navigate(-1)}
          className="flex items-center gap-1 text-water text-sm font-medium -ml-1">
          <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round">
            <path d="M15 18l-6-6 6-6" />
          </svg>
          Back
        </button>
        <button onClick={editing ? handleSave : startEdit}
          disabled={saving}
          className="text-sm text-water font-medium disabled:opacity-50">
          {saving ? 'Saving...' : editing ? 'Save' : 'Edit'}
        </button>
      </div>

      {/* Sync status toast */}
      {syncStatus && (
        <div className={`mb-3 px-4 py-2.5 rounded-xl text-sm font-medium text-center ${
          syncStatus === 'synced'
            ? 'bg-success/10 border border-success/30 text-success'
            : 'bg-warning/10 border border-warning/30 text-warning'
        }`}>
          {syncStatus === 'synced'
            ? 'Config saved and synced to receiver'
            : 'Saved locally but could not reach receiver — it will use old values until synced'}
        </div>
      )}

      {/* Hero tank */}
      {!editing && (
        <div className="flex flex-col items-center mb-6">
          <TankVisualization
            percent={device.last_water_pct || 0}
            state={device.state || 'waiting'}
            size="lg"
          />
          <h2 className="text-xl font-bold text-white mt-4">{device.name}</h2>
          <div className="font-mono text-4xl font-bold text-white mt-2">
            {device.last_water_pct != null ? Math.round(device.last_water_pct) : '--'}
            <span className="text-xl text-slate-400">%</span>
          </div>
          {device.tank_capacity_l && device.last_water_pct != null && (
            <div className="font-mono text-lg text-slate-400">
              {Math.round(device.tank_capacity_l * device.last_water_pct / 100)} / {device.tank_capacity_l} L
            </div>
          )}
        </div>
      )}

      {/* Edit form */}
      {editing && (
        <div className="space-y-4 mb-6">
          <div className="bg-surface rounded-2xl p-4">
            <h3 className="text-sm font-semibold text-white mb-4">Tank Info</h3>
            <div className="space-y-3">
              <Field label="Tank Name" value={editForm.name}
                onChange={v => setEditForm({...editForm, name: v})} />
              <div className="flex items-center gap-2 px-3 py-2.5 rounded-lg bg-slate-800/50 border border-slate-700">
                <span className="text-xs text-slate-500">LoRa Address</span>
                <span className="text-sm text-slate-300 font-mono ml-auto">#{device.lora_address}</span>
              </div>
            </div>
          </div>
          <div className="bg-surface rounded-2xl p-4">
            <h3 className="text-sm font-semibold text-white mb-4">Sensor Calibration</h3>
            <div className="space-y-3">
              <Field label="Tank Capacity (Liters)" type="number" value={editForm.tank_capacity_l}
                onChange={v => setEditForm({...editForm, tank_capacity_l: parseInt(v) || 0})} />
              <div className="grid grid-cols-2 gap-3">
                <Field label="Distance at Full (cm)" type="number" value={editForm.min_distance_cm}
                  onChange={v => setEditForm({...editForm, min_distance_cm: parseInt(v) || 0})}
                  help="Sensor to water when tank is full" />
                <Field label="Distance at Empty (cm)" type="number" value={editForm.max_distance_cm}
                  onChange={v => setEditForm({...editForm, max_distance_cm: parseInt(v) || 0})}
                  help="Sensor to tank bottom when empty" />
              </div>
            </div>
          </div>
          <div className="bg-surface rounded-2xl p-4">
            <h3 className="text-sm font-semibold text-white mb-4">Transmitter Settings</h3>
            <div className="grid grid-cols-2 gap-3">
              <Field label="Sleep Interval (sec)" type="number" value={editForm.sleep_s}
                onChange={v => setEditForm({...editForm, sleep_s: Math.max(60, parseInt(v) || 60)})}
                help="60-86400 sec" />
              <Field label="Sensor Samples" type="number" value={editForm.samples}
                onChange={v => setEditForm({...editForm, samples: Math.min(20, Math.max(3, parseInt(v) || 5))})}
                help="3-20 readings per wake" />
            </div>
          </div>
          <div className="bg-surface rounded-2xl p-4">
            <h3 className="text-sm font-semibold text-white mb-4">Alert Thresholds</h3>
            <div className="grid grid-cols-2 gap-3">
              <Field label="Low Water Alert (%)" type="number" value={editForm.alert_low_pct}
                onChange={v => setEditForm({...editForm, alert_low_pct: parseInt(v) || 0})} />
              <Field label="High Water Alert (%)" type="number" value={editForm.alert_high_pct}
                onChange={v => setEditForm({...editForm, alert_high_pct: parseInt(v) || 0})} />
            </div>
          </div>
          <div className="flex gap-3">
            <button onClick={() => setEditing(false)}
              className="flex-1 py-2.5 rounded-xl border border-slate-600 text-slate-300 text-sm font-medium">
              Cancel
            </button>
            <button onClick={handleSave} disabled={saving}
              className="flex-1 py-2.5 rounded-xl bg-water text-white text-sm font-semibold disabled:opacity-50">
              {saving ? 'Saving...' : 'Save Changes'}
            </button>
          </div>
        </div>
      )}

      {/* Stats grid */}
      <div className="grid grid-cols-2 gap-3 mb-6">
        <InfoCard label="Battery" value={device.last_battery_pct != null ? `${Math.round(device.last_battery_pct)}%` : '--'}
          sub={device.last_battery_v != null ? `${device.last_battery_v.toFixed(2)}V` : ''}
          color={device.last_battery_pct != null && device.last_battery_pct < 20 ? 'danger' : ''} />
        <InfoCard label="Signal" value={device.last_rssi != null ? `${device.last_rssi} dBm` : '--'}
          sub={signalQuality(device.last_rssi)} />
        <InfoCard label="LoRa Address" value={`#${device.lora_address}`}
          sub={`FW: ${device.fw_version || 'Unknown'}`} />
        <InfoCard label="Last Seen" value={formatAge(device.last_seen)}
          sub={device.last_seen ? new Date(device.last_seen + 'Z').toLocaleTimeString() : ''} />
      </div>

      {/* History charts */}
      <div className="bg-surface rounded-2xl p-4 mb-6">
        {/* Chart mode toggle + range selector */}
        <div className="flex items-center justify-between mb-4">
          <div className="flex gap-1 bg-slate-800 rounded-lg p-0.5">
            <button onClick={() => setChartMode('water')}
              className={`px-3 py-1.5 rounded-md text-xs font-medium transition-all ${
                chartMode === 'water' ? 'bg-water text-white' : 'text-slate-400'
              }`}>Water</button>
            <button onClick={() => setChartMode('battery')}
              className={`px-3 py-1.5 rounded-md text-xs font-medium transition-all ${
                chartMode === 'battery' ? 'bg-success text-white' : 'text-slate-400'
              }`}>Battery</button>
          </div>
          <div className="flex gap-1 bg-slate-800 rounded-lg p-0.5">
            {RANGES.map(r => (
              <button key={r.key} onClick={() => setRange(r.key)}
                className={`px-2 py-1 rounded-md text-[10px] font-medium transition-all ${
                  range === r.key ? 'bg-slate-600 text-white' : 'text-slate-400'
                }`}>
                {r.label}
              </button>
            ))}
          </div>
        </div>

        {loading ? (
          <div className="skeleton h-40 w-full rounded-xl" />
        ) : history.length > 0 ? (
          <MiniChart
            data={history}
            field={chartMode === 'water' ? 'water_pct' : 'battery_pct'}
            color={chartMode === 'water' ? '#0EA5E9' : '#22C55E'}
            unit="%"
          />
        ) : (
          <div className="h-40 flex items-center justify-center text-slate-500 text-sm">
            No data for this period
          </div>
        )}
      </div>

      {/* Alert thresholds (read-only, edit is above now) */}
      {!editing && (
        <div className="bg-surface rounded-2xl p-4 mb-6">
          <h3 className="text-xs font-semibold text-slate-500 uppercase tracking-wider mb-3">Alert Thresholds</h3>
          <div className="flex gap-3">
            <div className="flex-1 text-center py-3 bg-danger/10 rounded-xl">
              <div className="text-[10px] text-slate-500 uppercase">Low Water</div>
              <div className="font-mono text-xl text-danger font-bold mt-1">{device.alert_low_pct}%</div>
            </div>
            <div className="flex-1 text-center py-3 bg-warning/10 rounded-xl">
              <div className="text-[10px] text-slate-500 uppercase">High Water</div>
              <div className="font-mono text-xl text-warning font-bold mt-1">{device.alert_high_pct}%</div>
            </div>
            <div className="flex-1 text-center py-3 bg-surface-elevated rounded-xl">
              <div className="text-[10px] text-slate-500 uppercase">Capacity</div>
              <div className="font-mono text-xl text-white font-bold mt-1">{device.tank_capacity_l}L</div>
            </div>
          </div>
        </div>
      )}

      {/* Delete device */}
      {!editing && (
        <div className="mt-4">
          {showDeleteConfirm ? (
            <div className="bg-danger/10 border border-danger/30 rounded-xl p-4 text-center">
              <p className="text-sm text-white mb-3">Remove this device and all its history?</p>
              <div className="flex gap-3">
                <button onClick={() => setShowDeleteConfirm(false)}
                  className="flex-1 py-2.5 rounded-xl border border-slate-600 text-slate-300 text-sm">Cancel</button>
                <button onClick={handleDelete}
                  className="flex-1 py-2.5 rounded-xl bg-danger text-white text-sm font-semibold">Delete</button>
              </div>
            </div>
          ) : (
            <button onClick={() => setShowDeleteConfirm(true)}
              className="w-full py-2.5 rounded-xl border border-danger/30 text-danger text-sm font-medium hover:bg-danger/10 transition-all">
              Remove Device
            </button>
          )}
        </div>
      )}
    </div>
  );
}

function Field({ label, value, onChange, type = 'text', help }) {
  return (
    <div>
      <label className="text-xs text-slate-400">{label}</label>
      <input type={type} value={value ?? ''} onChange={e => onChange(e.target.value)}
        className="w-full mt-1 px-3 py-2.5 rounded-lg bg-slate-800 border border-slate-600 text-white text-sm outline-none focus:border-water transition-colors" />
      {help && <div className="text-[10px] text-slate-500 mt-1">{help}</div>}
    </div>
  );
}

function InfoCard({ label, value, sub, color }) {
  return (
    <div className="bg-surface rounded-xl p-3">
      <div className="text-[10px] text-slate-500 uppercase tracking-wider">{label}</div>
      <div className={`font-mono text-base font-semibold mt-1 ${color ? `text-${color}` : 'text-white'}`}>{value}</div>
      {sub && <div className="text-xs text-slate-400 mt-0.5">{sub}</div>}
    </div>
  );
}

function MiniChart({ data, field, color, unit }) {
  if (!data.length) return null;

  const width = 320;
  const height = 140;
  const padding = { top: 20, right: 40, bottom: 20, left: 35 };
  const chartW = width - padding.left - padding.right;
  const chartH = height - padding.top - padding.bottom;

  const values = data.map(d => d[field] ?? 0).filter(v => v !== null);
  if (values.length === 0) return <div className="h-40 flex items-center justify-center text-slate-500 text-sm">No {field.replace('_', ' ')} data</div>;

  const min = Math.max(0, Math.min(...values) - 5);
  const max = Math.min(100, Math.max(...values) + 5);
  const range = max - min || 1;
  const latest = values[values.length - 1];

  const points = values.map((v, i) => ({
    x: padding.left + (i / (values.length - 1 || 1)) * chartW,
    y: padding.top + chartH - ((v - min) / range) * chartH
  }));

  const pathD = points.map((p, i) => `${i === 0 ? 'M' : 'L'}${p.x},${p.y}`).join(' ');
  const areaD = pathD + ` L${points[points.length - 1].x},${padding.top + chartH} L${points[0].x},${padding.top + chartH} Z`;
  const uid = `chart-${field}`;

  return (
    <div className="relative">
      <svg viewBox={`0 0 ${width} ${height}`} className="w-full" preserveAspectRatio="none">
        {/* Latest value label above the dot */}
        <text x={points[points.length - 1].x} y={Math.max(12, points[points.length - 1].y - 10)}
          textAnchor="middle" fill={color} fontSize="11" fontFamily="JetBrains Mono" fontWeight="bold">
          {Math.round(latest)}{unit}
        </text>
        <defs>
          <linearGradient id={`${uid}-grad`} x1="0" y1="0" x2="0" y2="1">
            <stop offset="0%" stopColor={color} stopOpacity="0.3" />
            <stop offset="100%" stopColor={color} stopOpacity="0" />
          </linearGradient>
        </defs>
        {[min, min + range / 2, max].map((v, i) => {
          const y = padding.top + chartH - ((v - min) / range) * chartH;
          return (
            <g key={i}>
              <line x1={padding.left} y1={y} x2={width - padding.right} y2={y} stroke="#1E293B" strokeWidth="1" />
              <text x={padding.left - 4} y={y + 4} textAnchor="end" fill="#64748B" fontSize="10" fontFamily="JetBrains Mono">
                {Math.round(v)}{unit}
              </text>
            </g>
          );
        })}
        <path d={areaD} fill={`url(#${uid}-grad)`} />
        <path d={pathD} fill="none" stroke={color} strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" />
        {points.length > 0 && (
          <circle cx={points[points.length - 1].x} cy={points[points.length - 1].y}
            r="4" fill={color} stroke="#0F172A" strokeWidth="2" />
        )}
      </svg>
    </div>
  );
}

function signalQuality(rssi) {
  if (rssi == null) return '';
  if (rssi > -60) return 'Excellent';
  if (rssi > -75) return 'Good';
  if (rssi > -90) return 'Fair';
  return 'Weak';
}

function formatAge(dateStr) {
  if (!dateStr) return 'Never';
  const diff = (Date.now() - new Date(dateStr + 'Z').getTime()) / 1000;
  if (diff < 60) return `${Math.round(diff)}s ago`;
  if (diff < 3600) return `${Math.round(diff / 60)}m ago`;
  if (diff < 86400) return `${Math.round(diff / 3600)}h ago`;
  return `${Math.round(diff / 86400)}d ago`;
}
