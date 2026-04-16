import { useState, useEffect } from 'react';
import { useAuth } from '../hooks/useAuth.jsx';
import { useSites } from '../hooks/useSites.jsx';
import { useNavigate } from 'react-router-dom';
import { api } from '../utils/api.js';

export default function Settings() {
  const { user, updateUser, logout } = useAuth();
  const { sites, activeSite, setActiveSiteId, updateSite, deleteSite } = useSites();
  const navigate = useNavigate();
  const [editingSite, setEditingSite] = useState(null);
  const [siteForm, setSiteForm] = useState({});
  const [showDeleteConfirm, setShowDeleteConfirm] = useState(null);
  const [pushEnabled, setPushEnabled] = useState(() => {
    try { return typeof Notification !== 'undefined' && Notification.permission === 'granted'; }
    catch { return false; }
  });

  const handleModeToggle = async () => {
    const newMode = user.mode === 'home' ? 'pro' : 'home';
    await updateUser({ mode: newMode });
  };

  const [pushError, setPushError] = useState('');
  const [pushLoading, setPushLoading] = useState(false);

  const enablePush = async () => {
    setPushError('');
    setPushLoading(true);
    try {
      // 1. Request notification permission
      const permission = await Notification.requestPermission();
      if (permission !== 'granted') {
        setPushError('Notification permission denied. Enable in browser settings.');
        setPushLoading(false);
        return;
      }

      // 2. Get VAPID public key from server
      const { key: vapidKey } = await api.get('/api/push/vapid-key');
      if (!vapidKey) {
        setPushError('Push not configured on server. Add VAPID keys to .env');
        setPushLoading(false);
        return;
      }

      // 3. Subscribe via service worker
      const reg = await navigator.serviceWorker?.ready;
      if (!reg) {
        setPushError('Service worker not available. Install the PWA first.');
        setPushLoading(false);
        return;
      }

      // Convert VAPID key from base64url to Uint8Array
      const urlBase64ToUint8Array = (base64String) => {
        const padding = '='.repeat((4 - base64String.length % 4) % 4);
        const base64 = (base64String + padding).replace(/-/g, '+').replace(/_/g, '/');
        const rawData = atob(base64);
        return Uint8Array.from([...rawData].map(c => c.charCodeAt(0)));
      };

      const sub = await reg.pushManager.subscribe({
        userVisibleOnly: true,
        applicationServerKey: urlBase64ToUint8Array(vapidKey)
      });

      // 4. Send subscription to backend
      const { endpoint, keys } = sub.toJSON();
      await api.post('/api/push/subscribe', { endpoint, keys });

      setPushEnabled(true);
    } catch (err) {
      setPushError(`Push setup failed: ${err.message}`);
    } finally {
      setPushLoading(false);
    }
  };

  const disablePush = async () => {
    setPushLoading(true);
    try {
      const reg = await navigator.serviceWorker?.ready;
      const sub = await reg?.pushManager?.getSubscription();
      if (sub) {
        const { endpoint } = sub.toJSON();
        await api.post('/api/push/unsubscribe', { endpoint });
        await sub.unsubscribe();
      }
      setPushEnabled(false);
    } catch (err) {
      setPushError(`Failed to disable: ${err.message}`);
    } finally {
      setPushLoading(false);
    }
  };

  const handleSiteSave = async () => {
    if (editingSite) {
      await updateSite(editingSite.id, siteForm);
      setEditingSite(null);
    }
  };

  const handleSiteDelete = async (id) => {
    await deleteSite(id);
    setShowDeleteConfirm(null);
  };

  return (
    <div className="px-4 pt-2 pb-32">
      <h1 className="text-2xl font-bold text-white mb-6">Settings</h1>

      {/* Profile section */}
      <Section title="Profile">
        <div className="space-y-3">
          <div className="flex items-center justify-between py-2">
            <div>
              <div className="text-sm font-medium text-white">{user?.name || 'User'}</div>
              <div className="text-xs text-slate-400">{user?.email}</div>
            </div>
            <div className="w-10 h-10 rounded-full bg-water/20 flex items-center justify-center">
              <span className="text-water font-bold text-sm">
                {(user?.name || user?.email || '?')[0].toUpperCase()}
              </span>
            </div>
          </div>
        </div>
      </Section>

      {/* Mode toggle — hidden until fleet view is built */}

      {/* Notifications */}
      <Section title="Notifications">
        <div className="flex items-center justify-between py-2">
          <div className="flex items-center gap-3">
            <div className="w-9 h-9 rounded-lg bg-warning/10 flex items-center justify-center flex-shrink-0">
              <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="#F59E0B" strokeWidth="2" strokeLinecap="round">
                <path d="M18 8A6 6 0 0 0 6 8c0 7-3 9-3 9h18s-3-2-3-9" /><path d="M13.73 21a2 2 0 0 1-3.46 0" />
              </svg>
            </div>
            <div>
              <div className="text-sm font-medium text-white">Push Notifications</div>
              <div className="text-xs text-slate-400">
                {typeof Notification === 'undefined'
                  ? 'Install the PWA to enable push notifications'
                  : 'Get alerts for low water, offline devices'}
              </div>
            </div>
          </div>
          {typeof Notification === 'undefined' ? (
            <span className="text-xs text-slate-500">N/A</span>
          ) : (
            <button onClick={pushEnabled ? disablePush : enablePush} disabled={pushLoading}>
              <ToggleSwitch on={pushEnabled} />
            </button>
          )}
        </div>
      </Section>

      {pushError && (
        <div className="bg-danger/10 border border-danger/30 rounded-xl px-4 py-2.5 text-danger text-xs mb-4 mx-0">
          {pushError}
        </div>
      )}

      {/* Locations */}
      <Section title="Locations" action={
        <button onClick={() => navigate('/setup')} className="text-xs text-water font-medium">
          Add Tank
        </button>
      }>
        {sites.length === 0 ? (
          <div className="text-sm text-slate-400 py-4 text-center">No locations configured</div>
        ) : (
          <div className="space-y-2">
            {sites.map(site => (
              <div key={site.id} className="py-2">
                {editingSite?.id === site.id ? (
                  <div className="space-y-2">
                    <input value={siteForm.name || ''} onChange={e => setSiteForm({...siteForm, name: e.target.value})}
                      className="w-full px-3 py-2 rounded-lg bg-slate-800 border border-slate-600 text-white text-sm outline-none focus:border-water" />
                    <input value={siteForm.receiver_ip || ''} onChange={e => setSiteForm({...siteForm, receiver_ip: e.target.value})}
                      placeholder="Receiver IP" className="w-full px-3 py-2 rounded-lg bg-slate-800 border border-slate-600 text-white text-sm outline-none focus:border-water font-mono" />
                    <input value={siteForm.mqtt_device_id || ''} onChange={e => setSiteForm({...siteForm, mqtt_device_id: e.target.value})}
                      placeholder="MQTT Device ID" className="w-full px-3 py-2 rounded-lg bg-slate-800 border border-slate-600 text-white text-sm outline-none focus:border-water font-mono" />
                    <div className="flex gap-2">
                      <button onClick={handleSiteSave} className="flex-1 py-2 rounded-lg bg-water text-white text-sm font-medium">Save</button>
                      <button onClick={() => setEditingSite(null)} className="flex-1 py-2 rounded-lg border border-slate-600 text-slate-300 text-sm">Cancel</button>
                    </div>
                  </div>
                ) : (
                  <div className="flex items-center justify-between">
                    <div>
                      <div className="flex items-center gap-2">
                        <div className="text-sm font-medium text-white">{site.name}</div>
                        {site.id === activeSite?.id && (
                          <span className="text-[10px] bg-water/20 text-water px-1.5 py-0.5 rounded">Active</span>
                        )}
                      </div>
                      <div className="text-xs text-slate-400 font-mono">{site.receiver_ip || 'No IP'}</div>
                      <div className="text-xs text-slate-500">{site.deviceCount || 0} tank{(site.deviceCount || 0) !== 1 ? 's' : ''}</div>
                    </div>
                    <div className="flex gap-1">
                      {site.id !== activeSite?.id && (
                        <button onClick={() => setActiveSiteId(site.id)}
                          className="p-2 text-slate-400 hover:text-water transition-colors text-xs">
                          Select
                        </button>
                      )}
                      <button onClick={() => { setEditingSite(site); setSiteForm({ name: site.name, receiver_ip: site.receiver_ip, mqtt_device_id: site.mqtt_device_id }); }}
                        className="p-2 text-slate-400 hover:text-water transition-colors">
                        <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round">
                          <path d="M11 4H4a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-7" />
                          <path d="M18.5 2.5a2.121 2.121 0 0 1 3 3L12 15l-4 1 1-4 9.5-9.5z" />
                        </svg>
                      </button>
                      {showDeleteConfirm === site.id ? (
                        <div className="flex gap-1">
                          <button onClick={() => handleSiteDelete(site.id)}
                            className="px-2 py-1 text-xs bg-danger text-white rounded">Delete</button>
                          <button onClick={() => setShowDeleteConfirm(null)}
                            className="px-2 py-1 text-xs text-slate-400">No</button>
                        </div>
                      ) : (
                        <button onClick={() => setShowDeleteConfirm(site.id)}
                          className="p-2 text-slate-400 hover:text-danger transition-colors">
                          <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round">
                            <path d="M3 6h18M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2" />
                          </svg>
                        </button>
                      )}
                    </div>
                  </div>
                )}
              </div>
            ))}
          </div>
        )}
      </Section>

      {/* Appearance */}
      <Section title="Appearance">
        <button onClick={() => {
          const current = document.documentElement.classList.contains('light') ? 'light' : 'dark';
          const next = current === 'dark' ? 'light' : 'dark';
          document.documentElement.classList.toggle('light', next === 'light');
          localStorage.setItem('tanksync_theme', next);
        }} className="w-full flex items-center justify-between py-2">
          <div className="flex items-center gap-3">
            <div className="w-9 h-9 rounded-lg bg-slate-700/50 flex items-center justify-center flex-shrink-0">
              <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="#94A3B8" strokeWidth="2" strokeLinecap="round">
                <circle cx="12" cy="12" r="5" /><line x1="12" y1="1" x2="12" y2="3" /><line x1="12" y1="21" x2="12" y2="23" />
                <line x1="4.22" y1="4.22" x2="5.64" y2="5.64" /><line x1="18.36" y1="18.36" x2="19.78" y2="19.78" />
                <line x1="1" y1="12" x2="3" y2="12" /><line x1="21" y1="12" x2="23" y2="12" />
                <line x1="4.22" y1="19.78" x2="5.64" y2="18.36" /><line x1="18.36" y1="5.64" x2="19.78" y2="4.22" />
              </svg>
            </div>
            <div>
              <div className="text-sm font-medium text-white">Theme</div>
              <div className="text-xs text-slate-400">Switch between dark and light mode</div>
            </div>
          </div>
          <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="#64748B" strokeWidth="2" strokeLinecap="round">
            <path d="M9 18l6-6-6-6" />
          </svg>
        </button>
      </Section>

      {/* OTA section */}
      {activeSite?.receiver_ip && (
        <Section title="Firmware">
          <button onClick={() => navigate('/ota')}
            className="w-full flex items-center justify-between py-2">
            <div className="flex items-center gap-3">
              <div className="w-9 h-9 rounded-lg bg-water/10 flex items-center justify-center flex-shrink-0">
                <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="#0EA5E9" strokeWidth="2" strokeLinecap="round">
                  <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
                  <polyline points="7,10 12,15 17,10" /><line x1="12" y1="15" x2="12" y2="3" />
                </svg>
              </div>
              <div>
                <div className="text-sm font-medium text-white">OTA Updates</div>
                <div className="text-xs text-slate-400">Update receiver and transmitter firmware</div>
              </div>
            </div>
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="#64748B" strokeWidth="2" strokeLinecap="round">
              <path d="M9 18l6-6-6-6" />
            </svg>
          </button>
        </Section>
      )}

      {/* About */}
      <Section title="About">
        <div className="flex items-center gap-3 py-1">
          <div className="w-9 h-9 rounded-lg bg-slate-700/50 flex items-center justify-center flex-shrink-0">
            <svg width="18" height="18" viewBox="0 0 40 40" fill="none">
              <path d="M20 4C20 4 8 18 8 26a12 12 0 0 0 24 0C32 18 20 4 20 4z" fill="#0EA5E9" opacity="0.8" />
            </svg>
          </div>
          <div className="text-xs text-slate-400 space-y-0.5">
            <div className="text-sm text-white font-medium">TankSync v1.0.0</div>
            <div>LoRa Water Tank Monitoring</div>
          </div>
        </div>
      </Section>

      {/* Logout */}
      <button onClick={() => { logout(); navigate('/login'); }}
        className="w-full mt-6 py-3 rounded-xl border border-danger/50 text-danger text-sm font-medium
          hover:bg-danger/10 transition-all">
        Sign Out
      </button>
    </div>
  );
}

function Section({ title, action, children }) {
  return (
    <div className="mb-6">
      <div className="flex items-center justify-between mb-2 px-1">
        <h3 className="text-xs font-semibold text-slate-500 uppercase tracking-wider">{title}</h3>
        {action}
      </div>
      <div className="bg-surface rounded-xl px-4 py-2">
        {children}
      </div>
    </div>
  );
}

function ToggleSwitch({ on }) {
  return (
    <div className={`w-12 h-7 rounded-full transition-colors duration-200 relative ${on ? 'bg-water' : 'bg-slate-600'}`}>
      <div className={`absolute top-0.5 w-6 h-6 rounded-full bg-white shadow transition-transform duration-200 ${on ? 'translate-x-5' : 'translate-x-0.5'}`} />
    </div>
  );
}
