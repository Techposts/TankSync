// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

import { useState, useEffect } from 'react';
import { useNavigate, useSearchParams } from 'react-router-dom';
import { useAuth } from '../hooks/useAuth.jsx';
import { api } from '../utils/api.js';

/**
 * QR Code Claim Page — handles /link?id=...&token=...&ip=...
 *
 * Architecture: The phone (on LAN) talks directly to the receiver to verify
 * the token and discover devices. The cloud server only creates DB records
 * and MQTT credentials — it never needs LAN access to the receiver.
 */
export default function LinkDevice() {
  const [searchParams] = useSearchParams();
  const { user, loading: authLoading } = useAuth();
  const navigate = useNavigate();

  const deviceId = searchParams.get('id');
  const token = searchParams.get('token');
  const receiverIp = searchParams.get('ip');
  const tanksParam = searchParams.get('tanks');

  const [status, setStatus] = useState('linking');
  const [message, setMessage] = useState('');
  const [step, setStep] = useState('');
  const [mqttCreds, setMqttCreds] = useState(null);
  const [mqttAutoConfigured, setMqttAutoConfigured] = useState(false);

  useEffect(() => {
    if (!deviceId || !token || !receiverIp) setStatus('missing_params');
  }, [deviceId, token, receiverIp]);

  useEffect(() => {
    if (authLoading) return;
    if (!user) {
      const returnUrl = `/link?id=${deviceId}&token=${token}&ip=${receiverIp}`;
      navigate(`/login?redirect=${encodeURIComponent(returnUrl)}`);
      return;
    }
    if (status !== 'linking' || !deviceId || !token || !receiverIp) return;
    claimDevice();
  }, [user, authLoading, status]);

  const claimDevice = async () => {
    try {
      // Tanks come from the receiver's /claim redirect (embedded in URL)
      setStep('Setting up cloud connection...');
      let tanks = [];
      if (tanksParam) {
        try { tanks = JSON.parse(tanksParam); } catch {}
      }
      const transmitters = tanks; // same data, has min_dist/max_dist/capacity
      const result = await api.post('/api/link/claim', {
        device_id: deviceId,
        receiver_ip: receiverIp,
        tanks,
        transmitters,
      });

      // Step 3: Try to push MQTT config to receiver (may fail due to mixed content)
      if (result.mqtt) {
        setStep('Configuring receiver...');
        try {
          await fetch(`http://${receiverIp}/api/mqtt`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
              host: result.mqtt.mqtt_host,
              port: result.mqtt.mqtt_port,
              user: result.mqtt.mqtt_username,
              pass: result.mqtt.mqtt_password,
              enabled: true,
              ha_discovery: false,
              use_tls: true,
            }),
            signal: AbortSignal.timeout(5000),
          });
        } catch {
          // Mixed content block or not on LAN — show manual instructions
        }
      }

      // Step 3: Try to push MQTT config to receiver directly
      let mqttPushed = false;
      if (result.mqtt && receiverIp) {
        setStep('Configuring receiver...');
        try {
          const pushResp = await fetch(`http://${receiverIp}/api/mqtt`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({
              host: result.mqtt.mqtt_host,
              port: result.mqtt.mqtt_port,
              user: result.mqtt.mqtt_username,
              pass: result.mqtt.mqtt_password,
              enabled: true, ha_discovery: false, use_tls: true,
            }),
            signal: AbortSignal.timeout(5000),
          });
          mqttPushed = pushResp.ok;
        } catch {} // May fail due to mixed content — handled below
      }

      setStatus('success');
      setMqttCreds(result.mqtt);
      setMqttAutoConfigured(mqttPushed);
      if (result.already_linked) {
        setMessage('This device is already linked to your account.');
      } else {
        setMessage(`Linked successfully! ${result.device_count || 0} tank${result.device_count !== 1 ? 's' : ''} discovered.`);
      }
    } catch (err) {
      setStatus('error');
      setMessage(err.message || 'Failed to link device');
    }
  };

  if (status === 'missing_params') {
    return (
      <div className="min-h-[100dvh] bg-slate-950 flex flex-col items-center justify-center px-6 text-center">
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

  if (status === 'linking') {
    return (
      <div className="min-h-[100dvh] bg-slate-950 flex flex-col items-center justify-center px-6 text-center">
        <div className="w-16 h-16 border-4 border-water/30 border-t-water rounded-full animate-spin mb-6" />
        <h2 className="text-xl font-bold text-white mb-2">Linking Device</h2>
        <p className="text-slate-400 text-sm">{step || 'Connecting...'}</p>
        <p className="text-slate-500 text-xs mt-2 font-mono">{deviceId}</p>
      </div>
    );
  }

  if (status === 'success') {
    return (
      <div className="min-h-[100dvh] bg-slate-950 flex flex-col items-center justify-center px-6 text-center">
        <div className="w-20 h-20 rounded-full bg-success/10 flex items-center justify-center mb-6">
          <svg width="40" height="40" viewBox="0 0 24 24" fill="none" stroke="#22C55E" strokeWidth="2.5" strokeLinecap="round">
            <path d="M20 6L9 17l-5-5" />
          </svg>
        </div>
        <h2 className="text-2xl font-bold text-white mb-2">Device Linked!</h2>
        <p className="text-slate-400 text-sm mb-4">{message}</p>

        {mqttAutoConfigured ? (
          <div className="bg-success/10 border border-success/30 rounded-xl px-4 py-3 mb-6 text-sm text-success">
            MQTT auto-configured. Your receiver will connect to the cloud automatically.
          </div>
        ) : mqttCreds ? (
          <div className="bg-warning/10 border border-warning/30 rounded-xl px-4 py-3 mb-4 text-sm text-left">
            <p className="text-warning font-medium mb-2">Configure MQTT on your receiver:</p>
            <p className="text-slate-300 text-xs font-mono mb-1">Host: {mqttCreds.mqtt_host}</p>
            <p className="text-slate-300 text-xs font-mono mb-1">Port: {mqttCreds.mqtt_port}</p>
            <p className="text-slate-300 text-xs font-mono mb-1">User: {mqttCreds.mqtt_username}</p>
            <p className="text-slate-300 text-xs font-mono mb-1">Pass: {mqttCreds.mqtt_password}</p>
            <p className="text-slate-300 text-xs font-mono mb-2">TLS: Enabled</p>
            <a href={`http://${receiverIp}`} target="_blank" rel="noopener"
              className="inline-block text-water text-xs underline">Open Receiver Web UI</a>
          </div>
        ) : null}

        <button onClick={() => window.location.href = '/'}
          className="w-full max-w-xs py-3.5 rounded-xl bg-water text-white font-semibold
            hover:bg-water-dark active:scale-[0.98] transition-all">
          Go to Dashboard
        </button>
      </div>
    );
  }

  return (
    <div className="min-h-[100dvh] bg-slate-950 flex flex-col items-center justify-center px-6 text-center">
      <ErrorIcon />
      <h2 className="text-xl font-bold text-white mt-4 mb-2">Linking Failed</h2>
      <p className="text-danger text-sm mb-6">{message}</p>
      <div className="flex gap-3 w-full max-w-xs">
        <button onClick={() => { setStatus('linking'); setStep(''); claimDevice(); }}
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
