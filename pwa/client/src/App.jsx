// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

import { Routes, Route, Navigate, useLocation } from 'react-router-dom';
import { useEffect, useState } from 'react';
import { AuthProvider, useAuth } from './hooks/useAuth.jsx';
import { SitesProvider } from './hooks/useSites.jsx';
import { api } from './utils/api.js';
import BottomNav from './components/BottomNav.jsx';
import Login from './pages/Login.jsx';
import SetupWizard from './pages/SetupWizard.jsx';
import Dashboard from './pages/Dashboard.jsx';
import TankDetail from './pages/TankDetail.jsx';
import Alerts from './pages/Alerts.jsx';
import Settings from './pages/Settings.jsx';
import OTA from './pages/OTA.jsx';
import LinkDevice from './pages/LinkDevice.jsx';
import Landing from './pages/Landing.jsx';

export default function App() {
  return (
    <AuthProvider>
      <AppRoutes />
    </AuthProvider>
  );
}

function AppRoutes() {
  const { user, loading, needsVerification } = useAuth();
  const location = useLocation();

  if (loading) {
    return (
      <div className="min-h-screen flex items-center justify-center bg-slate-950">
        <div className="flex flex-col items-center gap-4">
          <div className="w-12 h-12 border-3 border-water/30 border-t-water rounded-full animate-spin" />
          <span className="text-slate-400 text-sm">Loading...</span>
        </div>
      </div>
    );
  }

  if (!user || needsVerification) {
    return (
      <Routes>
        <Route path="/" element={<Landing />} />
        <Route path="/login" element={<Login />} />
        <Route path="/link" element={<LinkDevice />} />
        <Route path="*" element={<Navigate to="/" replace />} />
      </Routes>
    );
  }

  return (
    <SitesProvider>
      <AuthenticatedApp />
    </SitesProvider>
  );
}

function AuthenticatedApp() {
  const location = useLocation();
  const [alertCount, setAlertCount] = useState(0);

  // Poll alert count
  useEffect(() => {
    const load = async () => {
      try {
        const data = await api.get('/api/alerts?limit=1');
        setAlertCount(data.unread || 0);
      } catch {}
    };
    load();
    const interval = setInterval(load, 30000);
    return () => clearInterval(interval);
  }, []);

  // SSE for real-time updates
  useEffect(() => {
    const token = localStorage.getItem('tanksync_token');
    if (!token) return;

    let es;
    const connect = () => {
      // Use fetch-based SSE since EventSource doesn't support auth headers
      // We'll use a polling fallback for now, SSE handled by token in query
      es = new EventSource(`/events?token=${encodeURIComponent(token)}`);
      es.onmessage = (event) => {
        try {
          const data = JSON.parse(event.data);
          if (data.type === 'alert') {
            setAlertCount(c => c + 1);
          }
        } catch {}
      };
      es.onerror = () => {
        es.close();
        // Reconnect after 5s
        setTimeout(connect, 5000);
      };
    };

    connect();
    return () => es?.close();
  }, []);

  const hideNav = ['/setup', '/login', '/link'].some(p => location.pathname.startsWith(p));

  return (
    <div className="min-h-screen bg-slate-950">
      {/* Header bar */}
      {!hideNav && (
        <header className="sticky top-0 z-40 glass border-b border-slate-700/50"
          style={{ paddingTop: 'var(--safe-area-top)' }}>
          <div className="flex items-center justify-between px-4 h-11">
            <div className="flex items-center gap-2">
              <svg width="24" height="24" viewBox="0 0 40 40" fill="none">
                <path d="M20 6C20 6 10 17 10 24a10 10 0 0 0 20 0C30 17 20 6 20 6z" fill="#0EA5E9" />
                <path d="M20 11C20 11 14 19 14 23a6 6 0 0 0 12 0C26 19 20 11 20 11z" fill="#38BDF8" opacity="0.5" />
              </svg>
              <span className="text-white font-bold text-xl tracking-tight">Tank<span className="text-water">Sync</span></span>
            </div>
            <div className="flex items-center gap-1.5 bg-success/10 px-2.5 py-1 rounded-full">
              <div className="w-1.5 h-1.5 rounded-full bg-success pulse-live" />
              <span className="text-[11px] text-success font-semibold">Live</span>
            </div>
          </div>
        </header>
      )}

      {/* Page content */}
      <main className={!hideNav ? 'pt-1 pb-20' : ''}>
        <Routes>
          <Route path="/" element={<Dashboard />} />
          <Route path="/setup" element={<SetupWizard />} />
          <Route path="/tank/:id" element={<TankDetail />} />
          <Route path="/alerts" element={<Alerts />} />
          <Route path="/settings" element={<Settings />} />
          <Route path="/ota" element={<OTA />} />
          <Route path="/link" element={<LinkDevice />} />
          <Route path="*" element={<Navigate to="/" replace />} />
        </Routes>
      </main>

      {/* Bottom navigation */}
      {!hideNav && <BottomNav alertCount={alertCount} />}
    </div>
  );
}
