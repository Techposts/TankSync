import { useState, useRef, useCallback } from 'react';
import { useNavigate } from 'react-router-dom';
import { useSites } from '../hooks/useSites.jsx';
import { api } from '../utils/api.js';
import jsQR from 'jsqr';
import TankVisualization from '../components/TankVisualization.jsx';

export default function SetupWizard() {
  const { sites, activeSite, addSite, addDevice } = useSites();
  const navigate = useNavigate();
  const isFirstSite = sites.length === 0;
  // Skip welcome + name steps if adding a tank to existing site
  const STEPS = isFirstSite ? ['welcome', 'connect', 'name', 'done'] : ['connect', 'done'];

  const [step, setStep] = useState(0);
  const [receiverIp, setReceiverIp] = useState('192.168.0.');
  const [mqttDeviceId, setMqttDeviceId] = useState('');
  const [siteName, setSiteName] = useState('');
  const [probeResult, setProbeResult] = useState(null);
  const [probing, setProbing] = useState(false);
  const [error, setError] = useState('');
  const [scanning, setScanning] = useState(false);
  const [scanError, setScanError] = useState('');
  const videoRef = useRef(null);
  const streamRef = useRef(null);
  const scanIntervalRef = useRef(null);

  const canvasRef = useRef(null);

  // QR code scanner using jsQR library (works on all browsers including iOS Safari)
  const startScan = useCallback(async () => {
    setScanError('');
    try {
      const stream = await navigator.mediaDevices.getUserMedia({
        video: { facingMode: 'environment', width: { ideal: 640 }, height: { ideal: 480 } }
      });
      streamRef.current = stream;
      setScanning(true);

      // Wait for DOM elements
      setTimeout(() => {
        if (!videoRef.current) return;
        videoRef.current.srcObject = stream;
        videoRef.current.play();

        const canvas = canvasRef.current;
        const ctx = canvas?.getContext('2d', { willReadFrequently: true });

        scanIntervalRef.current = setInterval(() => {
          if (!videoRef.current || videoRef.current.readyState < 2 || !ctx) return;

          canvas.width = videoRef.current.videoWidth;
          canvas.height = videoRef.current.videoHeight;
          ctx.drawImage(videoRef.current, 0, 0, canvas.width, canvas.height);

          const imageData = ctx.getImageData(0, 0, canvas.width, canvas.height);
          const code = jsQR(imageData.data, imageData.width, imageData.height, {
            inversionAttempts: 'dontInvert'
          });

          if (code && code.data) {
            const scanned = code.data.trim();
            try {
              const parsed = new URL(scanned);
              // New format: http://<ip>/claim
              if (parsed.pathname === '/claim') {
                stopScan();
                window.location.href = scanned;
                return;
              }
              // Old format: https://cloud/link?id=...&token=...&ip=...
              const id = parsed.searchParams.get('id');
              const token = parsed.searchParams.get('token');
              const ip = parsed.searchParams.get('ip');
              if (id && token && ip) {
                stopScan();
                window.location.href = `/link?id=${id}&token=${token}&ip=${ip}`;
                return;
              }
            } catch {} // Not a valid URL — keep scanning
          }
        }, 300); // Scan every 300ms
      }, 200);
    } catch {
      setScanError('Camera access denied. Allow camera permission in your device settings and try again.');
    }
  }, []);

  const stopScan = useCallback(() => {
    if (scanIntervalRef.current) clearInterval(scanIntervalRef.current);
    if (streamRef.current) streamRef.current.getTracks().forEach(t => t.stop());
    streamRef.current = null;
    setScanning(false);
  }, []);

  const isValidIp = (ip) => /^(\d{1,3}\.){3}\d{1,3}$/.test(ip) && ip.split('.').every(n => parseInt(n) <= 255);

  const probeReceiver = async () => {
    setError('');
    setProbeResult(null);

    if (!isValidIp(receiverIp)) {
      setError('Enter a valid IP address (e.g. 192.168.0.217)');
      return;
    }

    setProbing(true);
    try {
      const res = await fetch(`http://${receiverIp}/api/data`, { signal: AbortSignal.timeout(5000) });
      const data = await res.json();
      setProbeResult(data);
      // Try to get device ID from system endpoint
      try {
        const sysRes = await fetch(`http://${receiverIp}/api/system`, { signal: AbortSignal.timeout(3000) });
        const sys = await sysRes.json();
        if (sys.version) setMqttDeviceId(sys.device_id || '');
      } catch {}
    } catch {
      setError('Could not reach receiver. Check the IP and make sure you are on the same network.');
    } finally {
      setProbing(false);
    }
  };

  const finishSetup = async () => {
    if (!probeResult?.tanks?.length) {
      setError('No tanks detected. Go back and connect to your receiver first.');
      return;
    }

    try {
      let targetSiteId;

      if (isFirstSite) {
        // Create a new site
        const site = await addSite({
          name: siteName || 'My Home',
          receiver_ip: receiverIp,
          mqtt_device_id: mqttDeviceId || null
        });
        targetSiteId = site.id;
      } else {
        // Add to existing active site
        targetSiteId = activeSite.id;
      }

      // Add discovered devices
      for (const tank of probeResult.tanks) {
        await addDevice(targetSiteId, {
          lora_address: tank.address,
          name: tank.name || `Tank ${tank.address}`,
        });
      }

      // Advance to done step
      setStep(STEPS.length - 1);
    } catch (err) {
      setError(err.message);
    }
  };

  const currentStep = STEPS[step];

  return (
    <div className="min-h-screen bg-slate-950 flex flex-col items-center justify-center px-6 relative">
      {/* Close button — skip wizard and go to dashboard */}
      <button onClick={() => window.location.href = '/'}
        className="absolute top-4 right-4 w-10 h-10 flex items-center justify-center rounded-full
          text-slate-400 hover:text-white hover:bg-slate-800 transition-all"
        style={{ top: 'calc(var(--safe-area-top, 0px) + 16px)' }}>
        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round">
          <line x1="18" y1="6" x2="6" y2="18" /><line x1="6" y1="6" x2="18" y2="18" />
        </svg>
      </button>

      {/* Progress dots */}
      <div className="flex gap-2 mb-8">
        {STEPS.map((_, i) => (
          <div key={i} className={`h-1.5 rounded-full transition-all duration-500 ${
            i === step ? 'w-8 bg-water' : i < step ? 'w-4 bg-water/50' : 'w-4 bg-slate-700'
          }`} />
        ))}
      </div>

      <div className="w-full max-w-sm">
        {/* Welcome (first-time only) */}
        {currentStep === 'welcome' && (
          <div className="text-center animate-in fade-in">
            <TankVisualization percent={72} state="online" />
            <h2 className="text-2xl font-bold text-white mt-6 mb-2">Welcome to TankSync</h2>
            <p className="text-slate-400 mb-8">Monitor your water tanks from anywhere. Let's get you set up.</p>
            <button onClick={() => setStep(step + 1)}
              className="w-full py-3.5 rounded-xl bg-water text-white font-semibold
                hover:bg-water-dark active:scale-[0.98] transition-all">
              Set Up My Tank
            </button>
          </div>
        )}

        {/* Connect */}
        {currentStep === 'connect' && (
          <div className="animate-in fade-in">
            <h2 className="text-2xl font-bold text-white mb-2">
              {isFirstSite ? 'Find Your Receiver' : 'Add a Tank'}
            </h2>
            <p className="text-slate-400 mb-6 text-sm">
              {isFirstSite
                ? 'Scan the QR code from your receiver\'s web UI, or enter its IP address manually.'
                : `Adding to "${activeSite?.name}". Connect to your receiver to discover tanks.`}
            </p>

            {/* QR Scanner */}
            {scanning ? (
              <div className="mb-4">
                <div className="relative rounded-xl overflow-hidden bg-black aspect-[4/3] mb-3">
                  <video ref={videoRef} className="w-full h-full object-cover" playsInline muted />
                  <canvas ref={canvasRef} className="hidden" />
                  {/* Scan overlay crosshair */}
                  <div className="absolute inset-0 flex items-center justify-center">
                    <div className="w-48 h-48 border-2 border-water/60 rounded-2xl" />
                  </div>
                  <div className="absolute bottom-2 left-0 right-0 text-center text-xs text-white/70">
                    Point at QR code on receiver screen
                  </div>
                </div>
                <button onClick={stopScan}
                  className="w-full py-2.5 rounded-xl border border-slate-600 text-slate-300 text-sm font-medium">
                  Cancel Scan
                </button>
              </div>
            ) : (
              <button onClick={startScan}
                className="w-full py-3 rounded-xl bg-water/10 border border-water/30 text-water font-medium mb-4
                  flex items-center justify-center gap-2 active:scale-[0.98] transition-all">
                <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round">
                  <rect x="3" y="3" width="7" height="7" /><rect x="14" y="3" width="7" height="7" />
                  <rect x="3" y="14" width="7" height="7" /><rect x="14" y="14" width="7" height="7" />
                </svg>
                Scan QR Code
              </button>
            )}
            {scanError && (
              <div className="text-warning text-xs mb-3 bg-warning/10 px-3 py-2 rounded-lg">{scanError}</div>
            )}

            <div className="text-center text-xs text-slate-500 mb-4">— or enter IP manually —</div>

            <div className="space-y-4">
              <input
                type="text"
                value={receiverIp}
                onChange={e => setReceiverIp(e.target.value)}
                placeholder="192.168.0.217"
                className="w-full px-4 py-3 rounded-xl bg-surface border border-slate-700 text-white font-mono
                  focus:border-water focus:ring-1 focus:ring-water/50 outline-none transition-all"
              />

              <button onClick={probeReceiver} disabled={probing}
                className="w-full py-3 rounded-xl bg-surface border border-slate-600 text-white font-medium
                  hover:bg-surface-elevated active:scale-[0.98] transition-all disabled:opacity-50">
                {probing ? (
                  <span className="inline-flex items-center gap-2">
                    <svg className="animate-spin h-4 w-4" viewBox="0 0 24 24"><circle cx="12" cy="12" r="10" stroke="currentColor" strokeWidth="3" fill="none" opacity="0.3" /><path d="M12 2a10 10 0 0 1 10 10" stroke="currentColor" strokeWidth="3" fill="none" strokeLinecap="round" /></svg>
                    Searching...
                  </span>
                ) : 'Connect to Receiver'}
              </button>

              {probeResult && (
                <div className="bg-success/10 border border-success/30 rounded-xl p-4 text-center">
                  <div className="text-success font-semibold mb-1">Receiver Found!</div>
                  <div className="text-slate-300 text-sm">
                    {probeResult.tanks?.length || 0} tank{probeResult.tanks?.length !== 1 ? 's' : ''} detected
                    {probeResult.tanks?.[0] && ` — ${probeResult.tanks[0].name} at ${probeResult.tanks[0].water_pct}%`}
                  </div>
                </div>
              )}

              {error && (
                <div className="bg-danger/10 border border-danger/30 rounded-xl p-4 text-danger text-sm">
                  {error}
                </div>
              )}
            </div>

            <div className="flex gap-3 mt-6">
              <button onClick={() => step > 0 ? setStep(step - 1) : navigate('/')}
                className="flex-1 py-3 rounded-xl border border-slate-700 text-slate-300 font-medium
                  hover:bg-surface transition-all">
                {step > 0 ? 'Back' : 'Cancel'}
              </button>
              {isFirstSite ? (
                <button onClick={() => setStep(step + 1)} disabled={!probeResult}
                  className="flex-1 py-3 rounded-xl bg-water text-white font-semibold
                    hover:bg-water-dark active:scale-[0.98] transition-all disabled:opacity-40 disabled:cursor-not-allowed">
                  Next
                </button>
              ) : (
                <button onClick={finishSetup} disabled={!probeResult}
                  className="flex-1 py-3 rounded-xl bg-water text-white font-semibold
                    hover:bg-water-dark active:scale-[0.98] transition-all disabled:opacity-40 disabled:cursor-not-allowed">
                  Add Tank
                </button>
              )}
            </div>
            {!probeResult && !error && (
              <p className="text-xs text-slate-500 text-center mt-3">
                Connect to your receiver first to continue
              </p>
            )}
          </div>
        )}

        {/* Name (first-time only) */}
        {currentStep === 'name' && (
          <div className="animate-in fade-in">
            <h2 className="text-2xl font-bold text-white mb-2">Name Your Location</h2>
            <p className="text-slate-400 mb-6 text-sm">Give this location a name so you can identify it later (e.g. Home, Farm, Office).</p>

            <input
              type="text"
              value={siteName}
              onChange={e => setSiteName(e.target.value)}
              placeholder="Home, Farm, etc."
              className="w-full px-4 py-3 rounded-xl bg-surface border border-slate-700 text-white
                focus:border-water focus:ring-1 focus:ring-water/50 outline-none transition-all mb-4"
              autoFocus
            />

            {/* MQTT Device ID (optional, for pros) */}
            <details className="mb-6">
              <summary className="text-sm text-slate-500 cursor-pointer hover:text-slate-300">
                Advanced: MQTT Device ID
              </summary>
              <input
                type="text"
                value={mqttDeviceId}
                onChange={e => setMqttDeviceId(e.target.value)}
                placeholder="e.g. a1b2c3d4e5f6"
                className="w-full mt-2 px-4 py-3 rounded-xl bg-surface border border-slate-700 text-white font-mono text-sm
                  focus:border-water focus:ring-1 focus:ring-water/50 outline-none transition-all"
              />
            </details>

            {error && (
              <div className="bg-danger/10 border border-danger/30 rounded-xl p-4 text-danger text-sm mb-4">
                {error}
              </div>
            )}

            <div className="flex gap-3">
              <button onClick={() => setStep(step - 1)}
                className="flex-1 py-3 rounded-xl border border-slate-700 text-slate-300 font-medium
                  hover:bg-surface transition-all">
                Back
              </button>
              <button onClick={finishSetup}
                className="flex-1 py-3 rounded-xl bg-water text-white font-semibold
                  hover:bg-water-dark active:scale-[0.98] transition-all">
                Finish Setup
              </button>
            </div>
          </div>
        )}

        {/* Done */}
        {currentStep === 'done' && (
          <div className="text-center animate-in fade-in">
            <div className="inline-flex items-center justify-center w-20 h-20 rounded-full bg-success/10 mb-6">
              <svg width="40" height="40" viewBox="0 0 24 24" fill="none" stroke="#22C55E" strokeWidth="2.5" strokeLinecap="round">
                <path d="M20 6L9 17l-5-5" />
              </svg>
            </div>
            <h2 className="text-2xl font-bold text-white mb-2">All Set!</h2>
            <p className="text-slate-400 mb-8">Your tank monitoring is ready. You'll see live data on the dashboard.</p>
            <button onClick={() => window.location.href = '/'}
              className="w-full py-3.5 rounded-xl bg-water text-white font-semibold
                hover:bg-water-dark active:scale-[0.98] transition-all">
              Go to Dashboard
            </button>
          </div>
        )}
      </div>
    </div>
  );
}
