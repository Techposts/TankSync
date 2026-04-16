// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

import { useState, useEffect } from 'react';
import { useNavigate, useSearchParams } from 'react-router-dom';
import { useAuth } from '../hooks/useAuth.jsx';
import { api } from '../utils/api.js';

/**
 * QR Code Claim Page — handles /link?id=...&token=...&ip=...
 *
 * Flow:
 *  1. User scans QR from receiver's web UI → opens this URL
 *  2. If not logged in → redirect to /login with return URL
 *  3. If logged in → auto-claim the device via POST /api/link/claim
 *  4. On success → redirect to dashboard
 */
export default function LinkDevice() {
  const [searchParams] = useSearchParams();
  const { user, loading: authLoading } = useAuth();
  const navigate = useNavigate();

  const deviceId = searchParams.get('id');
  const token = searchParams.get('token');
  const receiverIp = searchParams.get('ip');

  const [status, setStatus] = useState('linking'); // linking | success | error | missing_params
  const [message, setMessage] = useState('');
  const [siteId, setSiteId] = useState(null);

  // Validate params
  useEffect(() => {
    if (!deviceId || !token || !receiverIp) {
      setStatus('missing_params');
    }
  }, [deviceId, token, receiverIp]);

  // Auto-claim once authenticated
  useEffect(() => {
    if (authLoading) return;
    if (!user) {
      // Redirect to login, preserving the link URL for after login
      const returnUrl = `/link?id=${deviceId}&token=${token}&ip=${receiverIp}`;
      navigate(`/login?redirect=${encodeURIComponent(returnUrl)}`);
      return;
    }
    if (status !== 'linking' || !deviceId || !token || !receiverIp) return;

    claimDevice();
  }, [user, authLoading, status]);

  const claimDevice = async () => {
    try {
      const result = await api.post('/api/link/claim', {
        device_id: deviceId,
        token,
        receiver_ip: receiverIp
      });

      setSiteId(result.site_id);

      if (result.already_linked) {
        setStatus('success');
        setMessage('This device is already linked to your account.');
      } else {
        setStatus('success');
        setMessage(`Linked successfully! ${result.device_count || 0} tank${result.device_count !== 1 ? 's' : ''} discovered.`);
      }
    } catch (err) {
      setStatus('error');
      setMessage(err.message || 'Failed to link device');
    }
  };

  // Missing params
  if (status === 'missing_params') {
    return (
      <div className="min-h-screen bg-slate-950 flex flex-col items-center justify-center px-6 text-center">
        <ErrorIcon />
        <h2 className="text-xl font-bold text-white mt-4 mb-2">Invalid Link</h2>
        <p className="text-slate-400 text-sm mb-6">This link is missing required parameters. Please scan the QR code from your receiver's web UI again.</p>
        <button onClick={() => navigate('/setup')}
          className="px-6 py-3 rounded-xl bg-water text-white font-semibold active:scale-[0.98] transition-all">
          Manual Setup
        </button>
      </div>
    );
  }

  // Linking in progress
  if (status === 'linking') {
    return (
      <div className="min-h-screen bg-slate-950 flex flex-col items-center justify-center px-6 text-center">
        <div className="w-16 h-16 border-4 border-water/30 border-t-water rounded-full animate-spin mb-6" />
        <h2 className="text-xl font-bold text-white mb-2">Linking Device</h2>
        <p className="text-slate-400 text-sm">Verifying and connecting your TankSync receiver...</p>
        <p className="text-slate-500 text-xs mt-2 font-mono">{deviceId}</p>
      </div>
    );
  }

  // Success
  if (status === 'success') {
    return (
      <div className="min-h-screen bg-slate-950 flex flex-col items-center justify-center px-6 text-center">
        <div className="w-20 h-20 rounded-full bg-success/10 flex items-center justify-center mb-6">
          <svg width="40" height="40" viewBox="0 0 24 24" fill="none" stroke="#22C55E" strokeWidth="2.5" strokeLinecap="round">
            <path d="M20 6L9 17l-5-5" />
          </svg>
        </div>
        <h2 className="text-2xl font-bold text-white mb-2">Device Linked!</h2>
        <p className="text-slate-400 text-sm mb-8">{message}</p>
        <button onClick={() => window.location.href = '/'}
          className="w-full max-w-xs py-3.5 rounded-xl bg-water text-white font-semibold
            hover:bg-water-dark active:scale-[0.98] transition-all">
          Go to Dashboard
        </button>
      </div>
    );
  }

  // Error
  return (
    <div className="min-h-screen bg-slate-950 flex flex-col items-center justify-center px-6 text-center">
      <ErrorIcon />
      <h2 className="text-xl font-bold text-white mt-4 mb-2">Linking Failed</h2>
      <p className="text-danger text-sm mb-6">{message}</p>
      <div className="flex gap-3 w-full max-w-xs">
        <button onClick={() => { setStatus('linking'); claimDevice(); }}
          className="flex-1 py-3 rounded-xl bg-water text-white font-semibold active:scale-[0.98] transition-all">
          Try Again
        </button>
        <button onClick={() => navigate('/setup')}
          className="flex-1 py-3 rounded-xl border border-slate-700 text-slate-300 font-medium">
          Manual Setup
        </button>
      </div>
    </div>
  );
}

function ErrorIcon() {
  return (
    <div className="w-20 h-20 rounded-full bg-danger/10 flex items-center justify-center">
      <svg width="36" height="36" viewBox="0 0 24 24" fill="none" stroke="#EF4444" strokeWidth="2" strokeLinecap="round">
        <circle cx="12" cy="12" r="10" />
        <line x1="15" y1="9" x2="9" y2="15" />
        <line x1="9" y1="9" x2="15" y2="15" />
      </svg>
    </div>
  );
}
