import { useState, useEffect, useRef } from 'react';
import { useNavigate, useSearchParams } from 'react-router-dom';
import { useAuth } from '../hooks/useAuth.jsx';

const TURNSTILE_SITE_KEY = import.meta.env.VITE_TURNSTILE_SITE_KEY || '';

export default function Login() {
  const [mode, setMode] = useState('login'); // 'login' | 'register' | 'verify'
  const [email, setEmail] = useState('');
  const [password, setPassword] = useState('');
  const [name, setName] = useState('');
  const [code, setCode] = useState('');
  const [error, setError] = useState('');
  const [info, setInfo] = useState('');
  const [loading, setLoading] = useState(false);
  const [resendCooldown, setResendCooldown] = useState(0);
  const turnstileRef = useRef(null);
  const turnstileWidgetId = useRef(null);
  const { login, register, verifyEmail, resendCode, needsVerification, user } = useAuth();
  const navigate = useNavigate();
  const [searchParams] = useSearchParams();
  const redirectTo = searchParams.get('redirect') || '/';

  // If logged in and verified, redirect (run once, not on every render)
  useEffect(() => {
    if (user && !needsVerification) {
      navigate(redirectTo, { replace: true });
    } else if (user && needsVerification && mode !== 'verify') {
      setEmail(user.email);
      setMode('verify');
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [user?.id, needsVerification]);

  // Load Turnstile script for registration
  useEffect(() => {
    if (!TURNSTILE_SITE_KEY || mode !== 'register') return;
    if (document.getElementById('cf-turnstile-script')) return;

    const script = document.createElement('script');
    script.id = 'cf-turnstile-script';
    script.src = 'https://challenges.cloudflare.com/turnstile/v0/api.js';
    script.async = true;
    script.onload = () => renderTurnstile();
    document.head.appendChild(script);
    return () => {};
  }, [mode]);

  useEffect(() => {
    if (mode === 'register' && TURNSTILE_SITE_KEY && window.turnstile && turnstileRef.current) {
      renderTurnstile();
    }
  }, [mode]);

  function renderTurnstile() {
    if (!window.turnstile || !turnstileRef.current) return;
    if (turnstileWidgetId.current) window.turnstile.remove(turnstileWidgetId.current);
    turnstileWidgetId.current = window.turnstile.render(turnstileRef.current, {
      sitekey: TURNSTILE_SITE_KEY,
      theme: 'dark',
      callback: () => {},
    });
  }

  function getTurnstileToken() {
    if (!TURNSTILE_SITE_KEY || !window.turnstile || turnstileWidgetId.current == null) return '';
    return window.turnstile.getResponse(turnstileWidgetId.current) || '';
  }

  // Resend cooldown timer
  useEffect(() => {
    if (resendCooldown <= 0) return;
    const t = setTimeout(() => setResendCooldown(c => c - 1), 1000);
    return () => clearTimeout(t);
  }, [resendCooldown]);

  const handleSubmit = async (e) => {
    e.preventDefault();
    setError('');
    setInfo('');
    setLoading(true);
    try {
      if (mode === 'register') {
        const turnstileToken = getTurnstileToken();
        if (TURNSTILE_SITE_KEY && !turnstileToken) {
          setError('Please complete the captcha');
          setLoading(false);
          return;
        }
        await register(email, password, name, turnstileToken);
        setMode('verify');
        setInfo('Verification code sent to your email');
      } else if (mode === 'verify') {
        await verifyEmail(email, code);
        navigate(redirectTo);
      } else {
        const data = await login(email, password);
        if (data.needsVerification) {
          setMode('verify');
          setInfo('Please verify your email to continue');
        } else {
          navigate(redirectTo);
        }
      }
    } catch (err) {
      setError(err.message);
    } finally {
      setLoading(false);
    }
  };

  const handleResend = async () => {
    setError('');
    try {
      await resendCode(email);
      setInfo('New code sent to your email');
      setResendCooldown(60);
    } catch (err) {
      setError(err.message);
    }
  };

  return (
    <div className="fixed inset-0 flex flex-col items-center justify-center px-6 bg-slate-950 overflow-auto">
      {/* Logo */}
      <div className="mb-10 text-center">
        <div className="inline-flex items-center justify-center w-20 h-20 rounded-2xl bg-water/10 mb-4">
          <svg width="40" height="40" viewBox="0 0 40 40" fill="none">
            <path d="M20 4C20 4 8 18 8 26a12 12 0 0 0 24 0C32 18 20 4 20 4z"
              fill="#0EA5E9" opacity="0.8" />
            <path d="M20 8C20 8 12 18 12 24a8 8 0 0 0 16 0C28 18 20 8 20 8z"
              fill="#38BDF8" opacity="0.6" />
          </svg>
        </div>
        <h1 className="text-3xl font-bold text-white tracking-tight">TankSync</h1>
        <p className="text-slate-400 mt-1 text-sm">
          {mode === 'verify' ? 'Verify your email' : 'Smart Water Monitoring'}
        </p>
      </div>

      {/* Form card */}
      <div className="w-full max-w-sm">
        <form onSubmit={handleSubmit} className="space-y-4">
          {mode === 'verify' ? (
            <>
              <p className="text-sm text-slate-400 text-center">
                Enter the 6-digit code sent to <span className="text-white font-medium">{email}</span>
              </p>
              <input
                type="text"
                inputMode="numeric"
                maxLength={6}
                value={code}
                onChange={e => setCode(e.target.value.replace(/\D/g, ''))}
                required
                autoFocus
                className="w-full px-4 py-4 rounded-xl bg-surface border border-slate-700 text-white text-center
                  text-2xl tracking-[0.5em] font-mono focus:border-water focus:ring-1 focus:ring-water/50 outline-none transition-all"
                placeholder="000000"
              />
            </>
          ) : (
            <>
              {mode === 'register' && (
                <div>
                  <label className="block text-sm font-medium text-slate-300 mb-1.5">Name</label>
                  <input type="text" value={name} onChange={e => setName(e.target.value)}
                    className="w-full px-4 py-3 rounded-xl bg-surface border border-slate-700 text-white
                      focus:border-water focus:ring-1 focus:ring-water/50 outline-none transition-all"
                    placeholder="Your name" />
                </div>
              )}
              <div>
                <label className="block text-sm font-medium text-slate-300 mb-1.5">Email</label>
                <input type="email" value={email} onChange={e => setEmail(e.target.value)} required
                  className="w-full px-4 py-3 rounded-xl bg-surface border border-slate-700 text-white
                    focus:border-water focus:ring-1 focus:ring-water/50 outline-none transition-all"
                  placeholder="you@example.com" />
              </div>
              <div>
                <label className="block text-sm font-medium text-slate-300 mb-1.5">Password</label>
                <input type="password" value={password} onChange={e => setPassword(e.target.value)} required minLength={6}
                  className="w-full px-4 py-3 rounded-xl bg-surface border border-slate-700 text-white
                    focus:border-water focus:ring-1 focus:ring-water/50 outline-none transition-all"
                  placeholder="At least 6 characters" />
              </div>
              {mode === 'register' && TURNSTILE_SITE_KEY && (
                <div ref={turnstileRef} className="flex justify-center" />
              )}
            </>
          )}

          {error && (
            <div className="text-danger text-sm bg-danger/10 px-4 py-2.5 rounded-xl">{error}</div>
          )}
          {info && (
            <div className="text-success text-sm bg-success/10 px-4 py-2.5 rounded-xl">{info}</div>
          )}

          <button type="submit" disabled={loading}
            className="w-full py-3.5 rounded-xl bg-water text-white font-semibold text-base
              hover:bg-water-dark active:scale-[0.98] transition-all duration-150
              disabled:opacity-50 disabled:cursor-not-allowed">
            {loading ? (
              <span className="inline-flex items-center gap-2">
                <svg className="animate-spin h-4 w-4" viewBox="0 0 24 24">
                  <circle cx="12" cy="12" r="10" stroke="currentColor" strokeWidth="3" fill="none" opacity="0.3" />
                  <path d="M12 2a10 10 0 0 1 10 10" stroke="currentColor" strokeWidth="3" fill="none" strokeLinecap="round" />
                </svg>
                {mode === 'verify' ? 'Verifying...' : mode === 'register' ? 'Creating account...' : 'Signing in...'}
              </span>
            ) : (
              mode === 'verify' ? 'Verify Email' : mode === 'register' ? 'Create Account' : 'Sign In'
            )}
          </button>
        </form>

        {mode === 'verify' && (
          <div className="mt-4 text-center">
            <button onClick={handleResend} disabled={resendCooldown > 0}
              className="text-sm text-slate-400 hover:text-water transition-colors disabled:opacity-40">
              {resendCooldown > 0 ? `Resend code in ${resendCooldown}s` : 'Resend verification code'}
            </button>
          </div>
        )}

        {mode !== 'verify' && (
          <div className="mt-6 text-center">
            <button
              onClick={() => { setMode(mode === 'register' ? 'login' : 'register'); setError(''); setInfo(''); }}
              className="text-sm text-slate-400 hover:text-water transition-colors">
              {mode === 'register' ? 'Already have an account? Sign in' : "Don't have an account? Create one"}
            </button>
          </div>
        )}
      </div>
    </div>
  );
}
