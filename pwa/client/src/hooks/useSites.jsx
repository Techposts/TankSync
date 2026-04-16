// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

import { useState, useEffect, useCallback, createContext, useContext } from 'react';
import { api } from '../utils/api.js';

const SitesContext = createContext(null);

export function SitesProvider({ children }) {
  const [sites, setSites] = useState([]);
  const [loading, setLoading] = useState(true);
  const [activeSiteId, setActiveSiteId] = useState(() => {
    return parseInt(localStorage.getItem('tanksync_active_site') || '0') || null;
  });

  const refresh = useCallback(async () => {
    try {
      const data = await api.get('/api/sites');
      setSites(data);
      // Auto-select first site if none selected
      if (!activeSiteId && data.length > 0) {
        setActiveSiteId(data[0].id);
      }
    } catch {
      setSites([]);
    } finally {
      setLoading(false);
    }
  }, [activeSiteId]);

  useEffect(() => { refresh(); }, []);

  useEffect(() => {
    if (activeSiteId) localStorage.setItem('tanksync_active_site', activeSiteId.toString());
  }, [activeSiteId]);

  const activeSite = sites.find(s => s.id === activeSiteId) || sites[0] || null;

  const addSite = async (siteData) => {
    const newSite = await api.post('/api/sites', siteData);
    await refresh();
    setActiveSiteId(newSite.id);
    return newSite;
  };

  const updateSite = async (id, updates) => {
    await api.put(`/api/sites/${id}`, updates);
    await refresh();
  };

  const deleteSite = async (id) => {
    await api.delete(`/api/sites/${id}`);
    if (activeSiteId === id) setActiveSiteId(null);
    await refresh();
  };

  const addDevice = async (siteId, deviceData) => {
    const result = await api.post(`/api/sites/${siteId}/devices`, deviceData);
    await refresh();
    return result;
  };

  const updateDevice = async (deviceId, updates) => {
    const result = await api.put(`/api/devices/${deviceId}`, updates);
    await refresh();
    return result;
  };

  const deleteDevice = async (deviceId) => {
    await api.delete(`/api/devices/${deviceId}`);
    await refresh();
  };

  return (
    <SitesContext.Provider value={{
      sites, loading, activeSite, activeSiteId, setActiveSiteId,
      refresh, addSite, updateSite, deleteSite,
      addDevice, updateDevice, deleteDevice
    }}>
      {children}
    </SitesContext.Provider>
  );
}

export function useSites() {
  const ctx = useContext(SitesContext);
  if (!ctx) throw new Error('useSites must be inside SitesProvider');
  return ctx;
}
