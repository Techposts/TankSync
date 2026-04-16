// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

import { useState, useEffect } from 'react';
import { useNavigate } from 'react-router-dom';
import { api } from '../utils/api.js';
import { useSites } from '../hooks/useSites.jsx';

export default function OTA() {
  const navigate = useNavigate();
  const { activeSite } = useSites();
  const [otaState, setOtaState] = useState(null);
  const [systemInfo, setSystemInfo] = useState(null);
  const [uploading, setUploading] = useState(false);
  const [uploadProgress, setUploadProgress] = useState(0);
  const [error, setError] = useState('');
  const [success, setSuccess] = useState('');

  useEffect(() => {
    if (!activeSite?.id) return;
    loadState();
  }, [activeSite?.id]);

  const loadState = async () => {
    try {
      const [ota, sys] = await Promise.all([
        api.get(`/api/sites/${activeSite.id}/proxy/ota/state`),
        api.get(`/api/sites/${activeSite.id}/proxy/system`)
      ]);
      setOtaState(ota);
      setSystemInfo(sys);
    } catch { setError('Could not reach receiver'); }
  };

  const handleUpload = async (e, type) => {
    const file = e.target.files?.[0];
    if (!file) return;
    if (!file.name.endsWith('.bin')) {
      setError('Please select a .bin firmware file');
      return;
    }

    setUploading(true);
    setError('');
    setSuccess('');
    setUploadProgress(0);

    try {
      const endpoint = type === 'receiver'
        ? `http://${activeSite.receiver_ip}/api/ota/upload`
        : `http://${activeSite.receiver_ip}/api/ota/upload_tx`;

      const xhr = new XMLHttpRequest();
      xhr.open('POST', endpoint);
      xhr.setRequestHeader('Content-Type', 'application/octet-stream');

      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) setUploadProgress(Math.round((e.loaded / e.total) * 100));
      };

      await new Promise((resolve, reject) => {
        xhr.onload = () => {
          if (xhr.status >= 200 && xhr.status < 300) resolve();
          else reject(new Error(xhr.responseText || 'Upload failed'));
        };
        xhr.onerror = () => reject(new Error('Network error'));
        xhr.send(file);
      });

      setSuccess(type === 'receiver'
        ? 'Receiver firmware uploaded! Device will reboot.'
        : 'Transmitter firmware staged. Deploy via LoRa OTA on next wake.');

      // Refresh after a delay (receiver reboots)
      if (type === 'receiver') {
        setTimeout(() => loadState(), 10000);
      } else {
        loadState();
      }
    } catch (err) {
      setError(err.message);
    } finally {
      setUploading(false);
      setUploadProgress(0);
    }
  };

  if (!activeSite?.receiver_ip) {
    return (
      <div className="px-4 pt-2 pb-24">
        <button onClick={() => navigate(-1)}
          className="flex items-center gap-1 text-water text-sm font-medium mb-4 -ml-1">
          <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round"><path d="M15 18l-6-6 6-6" /></svg>
          Back
        </button>
        <div className="text-center py-20 text-slate-400">
          No receiver IP configured for this site.
        </div>
      </div>
    );
  }

  return (
    <div className="px-4 pt-2 pb-24">
      <button onClick={() => navigate(-1)}
        className="flex items-center gap-1 text-water text-sm font-medium mb-4 -ml-1">
        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round"><path d="M15 18l-6-6 6-6" /></svg>
        Back
      </button>

      <h1 className="text-2xl font-bold text-white mb-6">Firmware Updates</h1>

      {/* System info */}
      {systemInfo && (
        <div className="bg-surface rounded-xl p-4 mb-4">
          <h3 className="text-xs font-semibold text-slate-500 uppercase tracking-wider mb-3">Current System</h3>
          <div className="grid grid-cols-2 gap-3 text-sm">
            <div><span className="text-slate-400">Version:</span> <span className="text-white font-mono">{systemInfo.version}</span></div>
            <div><span className="text-slate-400">IP:</span> <span className="text-white font-mono">{systemInfo.ip}</span></div>
            <div><span className="text-slate-400">Free Heap:</span> <span className="text-white font-mono">{systemInfo.free_heap ? `${Math.round(systemInfo.free_heap / 1024)}KB` : '--'}</span></div>
            <div><span className="text-slate-400">Uptime:</span> <span className="text-white font-mono">{systemInfo.uptime_s ? formatUptime(systemInfo.uptime_s) : '--'}</span></div>
          </div>
        </div>
      )}

      {error && (
        <div className="bg-danger/10 border border-danger/30 rounded-xl p-4 text-danger text-sm mb-4">
          {error}
        </div>
      )}
      {success && (
        <div className="bg-success/10 border border-success/30 rounded-xl p-4 text-success text-sm mb-4">
          {success}
        </div>
      )}

      {/* Upload progress */}
      {uploading && (
        <div className="bg-surface rounded-xl p-4 mb-4">
          <div className="flex items-center justify-between mb-2">
            <span className="text-sm text-white">Uploading firmware...</span>
            <span className="text-sm font-mono text-water">{uploadProgress}%</span>
          </div>
          <div className="h-2 bg-slate-700 rounded-full overflow-hidden">
            <div className="h-full bg-water rounded-full transition-all duration-300"
              style={{ width: `${uploadProgress}%` }} />
          </div>
        </div>
      )}

      {/* Receiver OTA */}
      <div className="bg-surface rounded-xl p-4 mb-4">
        <h3 className="text-sm font-semibold text-white mb-1">Receiver Firmware</h3>
        <p className="text-xs text-slate-400 mb-3">Upload a .bin file to update the receiver. It will reboot automatically.</p>
        <label className="block">
          <input type="file" accept=".bin" onChange={e => handleUpload(e, 'receiver')} disabled={uploading}
            className="hidden" />
          <span className="inline-flex items-center gap-2 px-4 py-2.5 rounded-xl bg-water/10 text-water text-sm font-medium
            cursor-pointer hover:bg-water/20 transition-all">
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round">
              <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
              <polyline points="17,8 12,3 7,8" />
              <line x1="12" y1="3" x2="12" y2="15" />
            </svg>
            Upload Receiver .bin
          </span>
        </label>
      </div>

      {/* Transmitter OTA */}
      <div className="bg-surface rounded-xl p-4 mb-4">
        <h3 className="text-sm font-semibold text-white mb-1">Transmitter Firmware</h3>
        <p className="text-xs text-slate-400 mb-3">Stage a .bin file on the receiver. It will be pushed to transmitters over LoRa on next wake.</p>
        <label className="block">
          <input type="file" accept=".bin" onChange={e => handleUpload(e, 'transmitter')} disabled={uploading}
            className="hidden" />
          <span className="inline-flex items-center gap-2 px-4 py-2.5 rounded-xl bg-water/10 text-water text-sm font-medium
            cursor-pointer hover:bg-water/20 transition-all">
            <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round">
              <path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4" />
              <polyline points="17,8 12,3 7,8" />
              <line x1="12" y1="3" x2="12" y2="15" />
            </svg>
            Upload Transmitter .bin
          </span>
        </label>
        {otaState?.tx_staged_version && (
          <div className="mt-2 text-xs text-slate-400">
            Staged: <span className="text-water font-mono">{otaState.tx_staged_version}</span>
          </div>
        )}
      </div>
    </div>
  );
}

function formatUptime(seconds) {
  const d = Math.floor(seconds / 86400);
  const h = Math.floor((seconds % 86400) / 3600);
  const m = Math.floor((seconds % 3600) / 60);
  if (d > 0) return `${d}d ${h}h`;
  if (h > 0) return `${h}h ${m}m`;
  return `${m}m`;
}
