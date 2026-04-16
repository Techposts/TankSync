// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

import { useEffect, useState, useRef } from 'react';
import { useNavigate } from 'react-router-dom';
import { useSites } from '../hooks/useSites.jsx';
import TankVisualization from '../components/TankVisualization.jsx';

export default function Dashboard() {
  const { sites, loading, activeSite, refresh } = useSites();
  const navigate = useNavigate();
  const [activeIdx, setActiveIdx] = useState(0);
  const scrollRef = useRef(null);

  useEffect(() => {
    const interval = setInterval(() => refresh(), 10000);
    return () => clearInterval(interval);
  }, [refresh]);

  if (!loading && sites.length === 0) {
    return (
      <div className="flex flex-col items-center justify-center min-h-[70vh] px-6 text-center">
        <div className="w-20 h-20 rounded-2xl bg-water/10 flex items-center justify-center mb-4">
          <svg width="36" height="36" viewBox="0 0 24 24" fill="none" stroke="#0EA5E9" strokeWidth="2" strokeLinecap="round">
            <line x1="12" y1="5" x2="12" y2="19" /><line x1="5" y1="12" x2="19" y2="12" />
          </svg>
        </div>
        <h2 className="text-xl font-bold text-white mb-2">No Sites Yet</h2>
        <p className="text-slate-400 mb-6 text-sm max-w-xs">Connect your TankSync receiver to start monitoring.</p>
        <button onClick={() => navigate('/setup')}
          className="px-6 py-3 rounded-xl bg-water text-white font-semibold active:scale-[0.98] transition-all">
          Add Your First Site
        </button>
      </div>
    );
  }

  if (loading) {
    return (
      <div className="px-4 pt-4 space-y-4">
        <div className="skeleton h-8 w-48" />
        <div className="skeleton h-64 w-full rounded-2xl" />
      </div>
    );
  }

  const devices = activeSite?.devices || [];
  const activeDevice = devices[activeIdx] || devices[0];
  const hasMultiple = devices.length > 1;

  const handleScroll = () => {
    if (!scrollRef.current || !hasMultiple) return;
    const el = scrollRef.current;
    const cardWidth = el.scrollWidth / devices.length;
    const newIdx = Math.round(el.scrollLeft / cardWidth);
    if (newIdx !== activeIdx && newIdx >= 0 && newIdx < devices.length) setActiveIdx(newIdx);
  };

  return (
    <div className="px-4 pt-1 pb-24">
      {/* Site header */}
      <div className="flex items-center justify-between mb-3">
        <div>
          <h1 className="text-2xl font-bold text-white">{activeSite?.name || 'Dashboard'}</h1>
          <div className="flex items-center gap-2 mt-0.5">
            <div className="w-1.5 h-1.5 rounded-full bg-success pulse-live" />
            <span className="text-xs text-slate-400">
              {devices.length} tank{devices.length !== 1 ? 's' : ''}
            </span>
          </div>
        </div>
        {sites.length > 1 && (
          <button onClick={() => navigate('/sites')}
            className="text-xs text-water bg-water/10 px-3 py-1.5 rounded-lg font-medium">Switch Site</button>
        )}
      </div>

      {/* Swipeable hero cards (multi-tank) or single hero */}
      {hasMultiple ? (
        <>
          <div ref={scrollRef} onScroll={handleScroll}
            className="flex gap-4 overflow-x-auto snap-x snap-mandatory -mx-4 px-4 pb-2"
            style={{ scrollbarWidth: 'none', msOverflowStyle: 'none', WebkitOverflowScrolling: 'touch' }}>
            {devices.map(device => (
              <TankHeroCard key={device.id} device={device}
                onClick={() => navigate(`/tank/${device.id}`)}
                style={{ minWidth: 'calc(100vw - 32px)', scrollSnapAlign: 'center' }} />
            ))}
          </div>
          {/* Dot indicators */}
          <div className="flex justify-center gap-1.5 mt-3 mb-2">
            {devices.map((_, idx) => (
              <div key={idx} className={`h-1.5 rounded-full transition-all duration-300 ${
                idx === activeIdx ? 'w-6 bg-water' : 'w-1.5 bg-slate-600'
              }`} />
            ))}
          </div>
        </>
      ) : activeDevice ? (
        <TankHeroCard device={activeDevice} onClick={() => navigate(`/tank/${activeDevice.id}`)} />
      ) : null}

      {/* Stats grid for visible device */}
      {activeDevice && (
        <div className="grid grid-cols-3 gap-3 mt-4 mb-6">
          <StatCard label="Battery"
            value={activeDevice.last_battery_pct != null ? `${Math.round(activeDevice.last_battery_pct)}%` : '--'}
            icon={<BatteryIcon pct={activeDevice.last_battery_pct} />}
            color={activeDevice.last_battery_pct != null && activeDevice.last_battery_pct < 20 ? 'danger' : 'success'}
            onClick={() => navigate(`/tank/${activeDevice.id}?chart=battery`)} />
          <StatCard label="Signal"
            value={activeDevice.last_rssi != null ? `${activeDevice.last_rssi}` : '--'} unit="dBm"
            icon={<SignalIcon rssi={activeDevice.last_rssi} />} color="water"
            onClick={() => navigate(`/tank/${activeDevice.id}`)} />
          <StatCard label="Status" value={activeDevice.state || 'waiting'}
            icon={<StatusDot state={activeDevice.state} />}
            color={activeDevice.state === 'online' ? 'success' : activeDevice.state === 'stale' ? 'warning' : 'slate'}
            onClick={() => navigate(`/tank/${activeDevice.id}`)} />
        </div>
      )}

      {/* All tanks compact list (multi-tank) */}
      {hasMultiple && (
        <div className="mt-2">
          <h3 className="text-xs font-semibold text-slate-500 uppercase tracking-wider mb-3 px-1">All Tanks</h3>
          <div className="space-y-2">
            {devices.map((device, idx) => (
              <button key={device.id} onClick={() => navigate(`/tank/${device.id}`)}
                className={`w-full bg-surface rounded-xl p-3 flex items-center gap-3 active:scale-[0.99] transition-all text-left ${idx === activeIdx ? 'ring-1 ring-water/30' : ''}`}>
                <TankVisualization percent={device.last_water_pct || 0} state={device.state || 'waiting'} size="sm" showLabel={false} />
                <div className="flex-1 min-w-0">
                  <div className="text-sm text-slate-300 font-medium truncate">{device.name}</div>
                  <div className="font-mono text-xl font-bold text-white">
                    {device.last_water_pct != null ? `${Math.round(device.last_water_pct)}%` : '--'}
                  </div>
                </div>
                <div className="text-right flex-shrink-0">
                  <div className="text-[10px] text-slate-500">{formatAge(device.last_seen)}</div>
                  <div className="flex items-center gap-1 mt-1 justify-end">
                    <BatteryIcon pct={device.last_battery_pct} small />
                    <span className="text-[10px] text-slate-400">
                      {device.last_battery_pct != null ? `${Math.round(device.last_battery_pct)}%` : ''}
                    </span>
                  </div>
                </div>
              </button>
            ))}
          </div>
        </div>
      )}

      {/* Quick actions */}
      <div className="mt-6 flex gap-3">
        <button onClick={() => navigate('/setup')}
          className="flex-1 py-3 rounded-xl border border-slate-700 text-slate-300 text-sm font-medium hover:bg-surface transition-all flex items-center justify-center gap-2">
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round">
            <line x1="12" y1="5" x2="12" y2="19" /><line x1="5" y1="12" x2="19" y2="12" />
          </svg>
          Add Tank
        </button>
        {activeSite?.receiver_ip && (
          <button onClick={() => window.open(`http://${activeSite.receiver_ip}`, '_blank')}
            className="flex-1 py-3 rounded-xl border border-slate-700 text-slate-300 text-sm font-medium hover:bg-surface transition-all flex items-center justify-center gap-2">
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round">
              <rect x="2" y="3" width="20" height="14" rx="2" /><line x1="8" y1="21" x2="16" y2="21" /><line x1="12" y1="17" x2="12" y2="21" />
            </svg>
            Receiver UI
          </button>
        )}
      </div>
    </div>
  );
}

function TankHeroCard({ device, onClick, style }) {
  return (
    <button onClick={onClick} style={style}
      className="w-full bg-surface rounded-2xl p-6 active:scale-[0.99] transition-transform text-left">
      <div className="flex items-center gap-6">
        <TankVisualization percent={device.last_water_pct || 0} state={device.state || 'waiting'} size="lg" showLabel={false} />
        <div className="flex-1 min-w-0">
          <div className="text-sm text-slate-400 mb-1">{device.name}</div>
          <div className="font-mono text-5xl font-bold text-white leading-none mb-2">
            {device.last_water_pct != null ? Math.round(device.last_water_pct) : '--'}
            <span className="text-2xl text-slate-400">%</span>
          </div>
          {device.tank_capacity_l && device.last_water_pct != null && (
            <div className="font-mono text-lg text-slate-400">
              {Math.round(device.tank_capacity_l * device.last_water_pct / 100)} L
            </div>
          )}
          <div className="mt-3 text-xs text-slate-500">{formatAge(device.last_seen)}</div>
        </div>
      </div>
    </button>
  );
}

function StatCard({ label, value, unit, icon, color, onClick }) {
  const c = { water: 'text-water', success: 'text-success', danger: 'text-danger', warning: 'text-warning', slate: 'text-slate-400' };
  return (
    <button onClick={onClick} className="bg-surface rounded-xl p-3 text-center active:scale-[0.97] transition-transform">
      <div className="flex justify-center mb-1.5">{icon}</div>
      <div className={`font-mono text-lg font-bold ${c[color] || 'text-white'} capitalize`}>{value}</div>
      {unit && <span className="text-xs text-slate-500"> {unit}</span>}
      <div className="text-[10px] text-slate-500 mt-0.5">{label}</div>
    </button>
  );
}

function BatteryIcon({ pct, small }) {
  const s = small ? 14 : 20;
  const fill = pct == null ? '#475569' : pct < 20 ? '#EF4444' : pct < 50 ? '#F59E0B' : '#22C55E';
  const w = pct == null ? 0 : Math.max(2, (pct / 100) * (s * 0.55));
  return (
    <svg width={s} height={s} viewBox="0 0 24 24" fill="none">
      <rect x="2" y="7" width="18" height="10" rx="2" stroke={fill} strokeWidth="2" />
      <rect x="4" y="9" width={w} height="6" rx="1" fill={fill} />
      <rect x="20" y="10" width="2" height="4" rx="0.5" fill={fill} />
    </svg>
  );
}

function SignalIcon({ rssi }) {
  const bars = rssi == null ? 0 : rssi > -60 ? 4 : rssi > -75 ? 3 : rssi > -90 ? 2 : 1;
  return (
    <svg width="20" height="20" viewBox="0 0 24 24" fill="none">
      {[0, 1, 2, 3].map(i => (
        <rect key={i} x={3 + i * 5} y={18 - (i + 1) * 4} width="3" height={(i + 1) * 4} rx="1" fill={i < bars ? '#0EA5E9' : '#334155'} />
      ))}
    </svg>
  );
}

function StatusDot({ state }) {
  const color = state === 'online' ? '#22C55E' : state === 'stale' ? '#F59E0B' : '#64748B';
  return <div className="flex items-center justify-center"><div className={`w-3 h-3 rounded-full ${state === 'online' ? 'pulse-live' : ''}`} style={{ backgroundColor: color }} /></div>;
}

function formatAge(dateStr) {
  if (!dateStr) return 'Never';
  const diff = (Date.now() - new Date(dateStr + 'Z').getTime()) / 1000;
  if (diff < 60) return `${Math.round(diff)}s ago`;
  if (diff < 3600) return `${Math.round(diff / 60)}m ago`;
  if (diff < 86400) return `${Math.round(diff / 3600)}h ago`;
  return `${Math.round(diff / 86400)}d ago`;
}
