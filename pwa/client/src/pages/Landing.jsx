// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

import { useEffect, useRef } from 'react';
import { useNavigate } from 'react-router-dom';

export default function Landing() {
  const navigate = useNavigate();

  // Landing page is always dark — temporarily disable light theme if active
  useEffect(() => {
    const html = document.documentElement;
    const wasLight = html.classList.contains('light');
    if (wasLight) html.classList.remove('light');
    return () => { if (wasLight) html.classList.add('light'); };
  }, []);

  return (
    <div className="bg-slate-950 text-white overflow-x-hidden">
      <Nav onLogin={() => navigate('/login')} />
      <Hero onGetStarted={() => navigate('/login')} />
      <WaveDivider flip />
      <Problem />
      <WaveDivider />
      <Solution />
      <WaveDivider flip />
      <HowItWorks />
      <WaveDivider />
      <Features />
      <WaveDivider flip />
      <TechSpecs />
      <WaveDivider />
      <Pricing onGetStarted={() => navigate('/login')} />
      <WaveDivider flip />
      <Hardware />
      <Footer />
    </div>
  );
}

// ─── SVG ICONS ─────────────────────────────────────────────────────────────
const icons = {
  droplet: (
    <svg width="32" height="32" viewBox="0 0 24 24" fill="none" stroke="#38BDF8" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
      <path d="M12 2.69l5.66 5.66a8 8 0 1 1-11.31 0z" />
    </svg>
  ),
  zap: (
    <svg width="32" height="32" viewBox="0 0 24 24" fill="none" stroke="#38BDF8" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
      <polygon points="13 2 3 14 12 14 11 22 21 10 12 10 13 2" />
    </svg>
  ),
  home: (
    <svg width="32" height="32" viewBox="0 0 24 24" fill="none" stroke="#38BDF8" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
      <path d="M3 9l9-7 9 7v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" />
      <polyline points="9 22 9 12 15 12 15 22" />
    </svg>
  ),
  radio: (
    <svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="#38BDF8" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
      <path d="M16.72 11.06A10.94 10.94 0 0 1 19 17.94" />
      <path d="M7.28 11.06A10.94 10.94 0 0 0 5 17.94" />
      <path d="M14.34 13.5A6.97 6.97 0 0 1 16 17.94" />
      <path d="M9.66 13.5A6.97 6.97 0 0 0 8 17.94" />
      <circle cx="12" cy="18" r="1" />
      <path d="M12 2v10" />
    </svg>
  ),
  battery: (
    <svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="#38BDF8" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
      <rect x="1" y="6" width="18" height="12" rx="2" ry="2" />
      <line x1="23" y1="10" x2="23" y2="14" />
      <rect x="4" y="9" width="6" height="6" rx="1" fill="#38BDF8" opacity="0.3" />
    </svg>
  ),
  phone: (
    <svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="#38BDF8" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
      <rect x="5" y="2" width="14" height="20" rx="2" ry="2" />
      <line x1="12" y1="18" x2="12.01" y2="18" />
    </svg>
  ),
  bell: (
    <svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="#38BDF8" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
      <path d="M18 8A6 6 0 0 0 6 8c0 7-3 9-3 9h18s-3-2-3-9" />
      <path d="M13.73 21a2 2 0 0 1-3.46 0" />
    </svg>
  ),
  chart: (
    <svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="#38BDF8" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
      <line x1="18" y1="20" x2="18" y2="10" />
      <line x1="12" y1="20" x2="12" y2="4" />
      <line x1="6" y1="20" x2="6" y2="14" />
    </svg>
  ),
  homeAssistant: (
    <svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="#38BDF8" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
      <path d="M3 9l9-7 9 7v11a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z" />
      <circle cx="12" cy="14" r="3" />
      <path d="M12 11v-1" />
      <path d="M14.6 12.5l.8-.5" />
      <path d="M14.6 15.5l.8.5" />
      <path d="M12 17v1" />
      <path d="M9.4 15.5l-.8.5" />
      <path d="M9.4 12.5l-.8-.5" />
    </svg>
  ),
  shield: (
    <svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="#38BDF8" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
      <path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z" />
      <path d="M9 12l2 2 4-4" />
    </svg>
  ),
  layers: (
    <svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="#38BDF8" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
      <polygon points="12 2 2 7 12 12 22 7 12 2" />
      <polyline points="2 17 12 22 22 17" />
      <polyline points="2 12 12 17 22 12" />
    </svg>
  ),
  refresh: (
    <svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="#38BDF8" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round">
      <path d="M21 2v6h-6" />
      <path d="M3 12a9 9 0 0 1 15-6.7L21 8" />
      <path d="M3 22v-6h6" />
      <path d="M21 12a9 9 0 0 1-15 6.7L3 16" />
    </svg>
  ),
};

// ─── WAVE DIVIDER ──────────────────────────────────────────────────────────
function WaveDivider({ flip = false }) {
  return (
    <div className={`relative w-full h-16 sm:h-24 overflow-hidden ${flip ? 'rotate-180' : ''}`}>
      <svg className="absolute bottom-0 w-full h-full" viewBox="0 0 1440 100" preserveAspectRatio="none" fill="none">
        <path d="M0,40 C360,100 1080,0 1440,60 L1440,100 L0,100Z" fill="#0EA5E9" opacity="0.05" />
        <path d="M0,60 C480,10 960,90 1440,40 L1440,100 L0,100Z" fill="#0EA5E9" opacity="0.03" />
      </svg>
    </div>
  );
}

// ─── FLOATING BUBBLES ──────────────────────────────────────────────────────
function Bubbles() {
  return (
    <div className="absolute inset-0 overflow-hidden pointer-events-none" aria-hidden="true">
      {[...Array(12)].map((_, i) => (
        <div
          key={i}
          className="landing-bubble absolute rounded-full"
          style={{
            width: `${6 + (i % 5) * 4}px`,
            height: `${6 + (i % 5) * 4}px`,
            left: `${5 + (i * 8) % 90}%`,
            bottom: `-${10 + (i * 7) % 20}%`,
            animationDelay: `${i * 1.2}s`,
            animationDuration: `${8 + (i % 4) * 3}s`,
          }}
        />
      ))}
    </div>
  );
}

// ─── WATER SURFACE ─────────────────────────────────────────────────────────
function WaterSurface({ className = '' }) {
  return (
    <div className={`absolute left-0 right-0 overflow-hidden pointer-events-none ${className}`} aria-hidden="true">
      <svg className="w-full landing-water-surface" viewBox="0 0 1440 60" preserveAspectRatio="none">
        <path className="landing-wave-1" d="M0,30 C240,10 480,50 720,30 C960,10 1200,50 1440,30 L1440,60 L0,60Z" fill="#0EA5E9" opacity="0.06" />
        <path className="landing-wave-2" d="M0,35 C300,55 600,15 900,35 C1200,55 1350,20 1440,35 L1440,60 L0,60Z" fill="#38BDF8" opacity="0.04" />
      </svg>
    </div>
  );
}

// ─── ANIMATIONS ─────────────────────────────────────────────────────────────
function FadeIn({ children, className = '', delay = 0 }) {
  const ref = useRef(null);
  useEffect(() => {
    const el = ref.current;
    if (!el) return;
    const observer = new IntersectionObserver(([entry]) => {
      if (entry.isIntersecting) {
        el.style.transitionDelay = `${delay}ms`;
        el.classList.add('opacity-100', 'translate-y-0');
        el.classList.remove('opacity-0', 'translate-y-8');
      }
    }, { threshold: 0.1 });
    observer.observe(el);
    return () => observer.disconnect();
  }, [delay]);
  return (
    <div ref={ref} className={`opacity-0 translate-y-8 transition-all duration-700 ease-out ${className}`}>
      {children}
    </div>
  );
}

// ─── NAV ────────────────────────────────────────────────────────────────────
function Nav({ onLogin }) {
  return (
    <nav className="fixed top-0 left-0 right-0 z-50 backdrop-blur-xl bg-slate-950/80 border-b border-slate-800/50">
      <div className="max-w-6xl mx-auto px-6 h-16 flex items-center justify-between">
        <div className="flex items-center gap-2">
          <svg width="28" height="28" viewBox="0 0 40 40" fill="none">
            <path d="M20 4C20 4 8 18 8 26a12 12 0 0 0 24 0C32 18 20 4 20 4z" fill="#0EA5E9" />
            <path d="M20 8C20 8 12 18 12 24a8 8 0 0 0 16 0C28 18 20 8 20 8z" fill="#38BDF8" opacity="0.5" />
          </svg>
          <span className="font-bold text-lg tracking-tight">Tank<span className="text-sky-400">Sync</span></span>
        </div>
        <div className="flex items-center gap-6">
          <a href="#features" className="text-sm text-slate-400 hover:text-white transition-colors hidden sm:block">Features</a>
          <a href="#pricing" className="text-sm text-slate-400 hover:text-white transition-colors hidden sm:block">Pricing</a>
          <a href="#hardware" className="text-sm text-slate-400 hover:text-white transition-colors hidden sm:block">Hardware</a>
          <button onClick={onLogin}
            className="text-sm font-medium bg-sky-500 hover:bg-sky-400 text-white px-4 py-2 rounded-lg transition-all active:scale-95">
            Login
          </button>
        </div>
      </div>
    </nav>
  );
}

// ─── HERO ───────────────────────────────────────────────────────────────────
function Hero({ onGetStarted }) {
  return (
    <section className="min-h-screen flex flex-col items-center justify-center text-center px-6 pt-20 relative overflow-hidden">
      {/* Background gradient */}
      <div className="absolute inset-0 bg-gradient-to-b from-sky-500/10 via-transparent to-transparent" />
      <div className="absolute top-1/3 left-1/2 -translate-x-1/2 w-[600px] h-[600px] bg-sky-500/5 rounded-full blur-3xl" />

      {/* Floating bubbles */}
      <Bubbles />

      {/* Water surface at bottom */}
      <WaterSurface className="bottom-0 h-20" />

      <FadeIn>
        <div className="inline-flex items-center gap-2 bg-sky-500/10 border border-sky-500/20 rounded-full px-4 py-1.5 mb-8">
          <div className="w-2 h-2 rounded-full bg-emerald-400 animate-pulse" />
          <span className="text-xs text-sky-300 font-medium">Now monitoring 1,000+ tanks worldwide</span>
        </div>
      </FadeIn>

      <FadeIn delay={100}>
        <h1 className="text-5xl sm:text-7xl font-bold tracking-tight max-w-4xl leading-[1.1] mb-6">
          Never run out of
          <span className="bg-gradient-to-r from-sky-400 to-cyan-300 bg-clip-text text-transparent"> water </span>
          again
        </h1>
      </FadeIn>

      <FadeIn delay={200}>
        <p className="text-lg sm:text-xl text-slate-400 max-w-2xl mb-10 leading-relaxed">
          Smart wireless monitoring for your water tanks. Know your water level from anywhere,
          get alerts before it runs out, and save up to 30% water with usage insights.
        </p>
      </FadeIn>

      <FadeIn delay={300}>
        <div className="flex flex-col sm:flex-row gap-4">
          <button onClick={onGetStarted}
            className="px-8 py-4 bg-sky-500 hover:bg-sky-400 text-white font-semibold rounded-xl text-lg transition-all active:scale-95 shadow-lg shadow-sky-500/25">
            Get Started Free
          </button>
          <a href="#how-it-works"
            className="px-8 py-4 border border-slate-700 hover:border-slate-500 text-slate-300 font-medium rounded-xl text-lg transition-all">
            See How It Works
          </a>
        </div>
      </FadeIn>

      <FadeIn delay={500} className="mt-16 w-full max-w-4xl">
        <div className="relative mx-auto">
          <div className="bg-slate-900 rounded-2xl border border-slate-800 p-4 shadow-2xl shadow-sky-500/10">
            <div className="bg-slate-800 rounded-xl aspect-[16/9] flex items-center justify-center">
              <div className="text-center">
                <svg width="64" height="64" viewBox="0 0 40 40" fill="none" className="mx-auto mb-4 opacity-30">
                  <path d="M20 4C20 4 8 18 8 26a12 12 0 0 0 24 0C32 18 20 4 20 4z" fill="#0EA5E9" />
                </svg>
                <p className="text-slate-500 text-sm">Dashboard screenshot placeholder</p>
                <p className="text-slate-600 text-xs mt-1">Replace with: app screenshot showing tank at 72% — 1920x1080</p>
              </div>
            </div>
          </div>
        </div>
      </FadeIn>
    </section>
  );
}

// ─── PROBLEM ────────────────────────────────────────────────────────────────
function Problem() {
  const stats = [
    {
      value: '135L',
      label: 'water wasted daily per household due to overflow',
      icon: icons.droplet,
    },
    {
      value: '23%',
      label: 'of water pumps fail from running dry',
      icon: icons.zap,
    },
    {
      value: '2x',
      label: 'daily roof climbs to check water level',
      icon: icons.home,
    },
  ];

  return (
    <section className="py-24 px-6 relative">
      <div className="max-w-6xl mx-auto">
        <FadeIn>
          <p className="text-sky-400 font-medium text-sm tracking-wider uppercase mb-4 text-center">The Problem</p>
          <h2 className="text-3xl sm:text-5xl font-bold text-center max-w-3xl mx-auto mb-6">
            You can't manage what you can't measure
          </h2>
          <p className="text-slate-400 text-center max-w-2xl mx-auto mb-16 text-lg">
            Most households have zero visibility into their water storage. The result?
            Overflows, dry pumps, and wasted water — every single day.
          </p>
        </FadeIn>

        <div className="grid sm:grid-cols-3 gap-8">
          {stats.map((stat, i) => (
            <FadeIn key={i} delay={i * 150}>
              <div className="bg-slate-900/50 border border-slate-800 rounded-2xl p-8 text-center hover:border-sky-500/30 transition-all">
                <div className="w-14 h-14 rounded-xl bg-sky-500/10 flex items-center justify-center mx-auto mb-4">
                  {stat.icon}
                </div>
                <div className="text-4xl font-bold text-white mb-2">{stat.value}</div>
                <p className="text-slate-400 text-sm">{stat.label}</p>
              </div>
            </FadeIn>
          ))}
        </div>
      </div>
    </section>
  );
}

// ─── SOLUTION ───────────────────────────────────────────────────────────────
function Solution() {
  return (
    <section className="py-24 px-6 bg-gradient-to-b from-slate-950 via-slate-900 to-slate-950 relative">
      <WaterSurface className="top-0 h-16 opacity-50" />
      <div className="max-w-6xl mx-auto grid md:grid-cols-2 gap-16 items-center">
        <FadeIn>
          <p className="text-sky-400 font-medium text-sm tracking-wider uppercase mb-4">The Solution</p>
          <h2 className="text-3xl sm:text-4xl font-bold mb-6">
            Your water tanks, always in your pocket
          </h2>
          <p className="text-slate-400 text-lg mb-8 leading-relaxed">
            TankSync uses long-range LoRa radio to wirelessly monitor your water tanks
            from up to 5 kilometers away. No WiFi needed between the sensor and receiver —
            it works through walls, across buildings, even between properties.
          </p>
          <div className="space-y-4">
            {[
              'Real-time water level on your phone',
              'Instant alerts when water is low or tank overflows',
              'Battery-powered sensor lasts 6+ months',
              'Works with Home Assistant for smart home integration',
            ].map((item, i) => (
              <div key={i} className="flex items-start gap-3">
                <div className="w-6 h-6 rounded-full bg-sky-500/20 flex items-center justify-center flex-shrink-0 mt-0.5">
                  <svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="#0EA5E9" strokeWidth="3" strokeLinecap="round">
                    <path d="M20 6L9 17l-5-5" />
                  </svg>
                </div>
                <span className="text-slate-300">{item}</span>
              </div>
            ))}
          </div>
        </FadeIn>

        <FadeIn delay={200}>
          <div className="bg-slate-800 rounded-2xl aspect-square flex items-center justify-center border border-slate-700">
            <div className="text-center p-8">
              <p className="text-slate-500 text-sm">Product photo placeholder</p>
              <p className="text-slate-600 text-xs mt-1">Replace with: receiver + transmitter clean shot — 800x800</p>
            </div>
          </div>
        </FadeIn>
      </div>
    </section>
  );
}

// ─── HOW IT WORKS ───────────────────────────────────────────────────────────
function HowItWorks() {
  const steps = [
    {
      num: '01',
      title: 'Install the Sensor',
      desc: 'Mount the waterproof ultrasonic sensor on your tank. The transmitter sits nearby, powered by a rechargeable battery.',
      placeholder: 'Transmitter mounted on tank — 600x400',
    },
    {
      num: '02',
      title: 'Connect the Receiver',
      desc: 'Plug in the receiver near your WiFi router. It picks up LoRa signals from up to 5km away and connects to the cloud.',
      placeholder: 'Receiver with OLED display showing levels — 600x400',
    },
    {
      num: '03',
      title: 'Monitor From Anywhere',
      desc: 'Open the TankSync app on your phone. See real-time levels, get push alerts, and track water usage history.',
      placeholder: 'Phone showing TankSync dashboard — 600x400',
    },
  ];

  return (
    <section id="how-it-works" className="py-24 px-6">
      <div className="max-w-6xl mx-auto">
        <FadeIn>
          <p className="text-sky-400 font-medium text-sm tracking-wider uppercase mb-4 text-center">How It Works</p>
          <h2 className="text-3xl sm:text-5xl font-bold text-center mb-16">Three steps to peace of mind</h2>
        </FadeIn>

        <div className="space-y-24">
          {steps.map((step, i) => (
            <div key={i} className={`grid md:grid-cols-2 gap-12 items-center`}>
              <FadeIn className={i % 2 === 1 ? 'md:order-2' : ''}>
                <div className="text-sky-500 font-mono text-6xl font-bold opacity-20 mb-4">{step.num}</div>
                <h3 className="text-2xl font-bold mb-4">{step.title}</h3>
                <p className="text-slate-400 text-lg leading-relaxed">{step.desc}</p>
              </FadeIn>
              <FadeIn delay={200} className={i % 2 === 1 ? 'md:order-1' : ''}>
                <div className="bg-slate-900 rounded-2xl aspect-[3/2] flex items-center justify-center border border-slate-800">
                  <p className="text-slate-600 text-xs text-center p-4">{step.placeholder}</p>
                </div>
              </FadeIn>
            </div>
          ))}
        </div>
      </div>
    </section>
  );
}

// ─── FEATURES ───────────────────────────────────────────────────────────────
function Features() {
  const features = [
    { icon: icons.radio, title: 'Long Range LoRa', desc: 'Up to 5km line-of-sight. Works through walls and across buildings.' },
    { icon: icons.battery, title: 'Low Power', desc: '6+ months battery life with deep sleep. Solar charging optional.' },
    { icon: icons.phone, title: 'Real-Time Dashboard', desc: 'See water levels, battery, signal strength — all from your phone.' },
    { icon: icons.bell, title: 'Smart Alerts', desc: 'Low water, overflow risk, battery low, device offline — push to your phone.' },
    { icon: icons.chart, title: 'Usage History', desc: 'Track water consumption patterns. Know when to schedule delivery.' },
    { icon: icons.homeAssistant, title: 'Home Assistant', desc: 'Native MQTT integration. Auto-discovery for seamless smart home setup.' },
    { icon: icons.shield, title: 'Secure by Design', desc: 'MQTT over TLS, per-user credentials, encrypted cloud connection.' },
    { icon: icons.layers, title: 'Multi-Tank', desc: 'Monitor up to 10 tanks from one receiver. Perfect for farms and buildings.' },
    { icon: icons.refresh, title: 'OTA Updates', desc: 'Update firmware wirelessly. New features delivered over the air.' },
  ];

  return (
    <section id="features" className="py-24 px-6 bg-gradient-to-b from-slate-950 via-slate-900 to-slate-950 relative">
      <WaterSurface className="bottom-0 h-16 opacity-40" />
      <div className="max-w-6xl mx-auto">
        <FadeIn>
          <p className="text-sky-400 font-medium text-sm tracking-wider uppercase mb-4 text-center">Features</p>
          <h2 className="text-3xl sm:text-5xl font-bold text-center mb-16">Everything you need. Nothing you don't.</h2>
        </FadeIn>

        <div className="grid sm:grid-cols-2 lg:grid-cols-3 gap-6">
          {features.map((f, i) => (
            <FadeIn key={i} delay={i * 80}>
              <div className="bg-slate-900/50 border border-slate-800 rounded-2xl p-6 hover:border-sky-500/30 hover:bg-slate-900 transition-all group">
                <div className="w-12 h-12 rounded-xl bg-sky-500/10 flex items-center justify-center mb-4 group-hover:bg-sky-500/20 transition-colors">
                  {f.icon}
                </div>
                <h3 className="font-semibold text-lg mb-2">{f.title}</h3>
                <p className="text-slate-400 text-sm leading-relaxed">{f.desc}</p>
              </div>
            </FadeIn>
          ))}
        </div>
      </div>
    </section>
  );
}

// ─── TECH SPECS ─────────────────────────────────────────────────────────────
function TechSpecs() {
  return (
    <section className="py-24 px-6">
      <div className="max-w-6xl mx-auto">
        <FadeIn>
          <p className="text-sky-400 font-medium text-sm tracking-wider uppercase mb-4 text-center">Tech Specs</p>
          <h2 className="text-3xl sm:text-4xl font-bold text-center mb-16">Built for reliability</h2>
        </FadeIn>

        <div className="grid md:grid-cols-2 gap-8">
          <FadeIn>
            <div className="bg-slate-900/50 border border-slate-800 rounded-2xl p-8">
              <h3 className="font-bold text-lg mb-6 text-sky-400">Transmitter (Tank Sensor)</h3>
              <div className="space-y-3 text-sm">
                {[
                  ['MCU', 'ESP32-C3 (RISC-V, WiFi + BLE)'],
                  ['Sensor', 'JSN-SR04T waterproof ultrasonic'],
                  ['Range', '25cm — 400cm'],
                  ['Radio', 'REYAX RYLR998 LoRa (865/915 MHz)'],
                  ['Power', '18650 LiPo + TP4056 USB-C charging'],
                  ['Battery Life', '6+ months (5-min intervals)'],
                  ['Sleep Current', '<10 uA deep sleep'],
                  ['Enclosure', 'IP65 weatherproof (3D printed)'],
                ].map(([k, v], i) => (
                  <div key={i} className="flex justify-between py-2 border-b border-slate-800 last:border-0">
                    <span className="text-slate-500">{k}</span>
                    <span className="text-white font-medium">{v}</span>
                  </div>
                ))}
              </div>
            </div>
          </FadeIn>

          <FadeIn delay={150}>
            <div className="bg-slate-900/50 border border-slate-800 rounded-2xl p-8">
              <h3 className="font-bold text-lg mb-6 text-sky-400">Receiver (Base Station)</h3>
              <div className="space-y-3 text-sm">
                {[
                  ['MCU', 'ESP32 DevKit v1 (or ESP32-C3)'],
                  ['Radio', 'REYAX RYLR998 LoRa'],
                  ['Display', 'SH1106 1.3" OLED (128x64)'],
                  ['LED', 'WS2812B RGB status indicator'],
                  ['Connectivity', 'WiFi 2.4GHz + MQTT over TLS'],
                  ['Web UI', 'Built-in config (WiFi, MQTT, LoRa, OTA)'],
                  ['LoRa Range', 'Up to 5km line-of-sight'],
                  ['Power', 'USB-C 5V (always-on)'],
                ].map(([k, v], i) => (
                  <div key={i} className="flex justify-between py-2 border-b border-slate-800 last:border-0">
                    <span className="text-slate-500">{k}</span>
                    <span className="text-white font-medium">{v}</span>
                  </div>
                ))}
              </div>
            </div>
          </FadeIn>
        </div>
      </div>
    </section>
  );
}

// ─── PRICING ────────────────────────────────────────────────────────────────
function Pricing({ onGetStarted }) {
  const isIndia = Intl.DateTimeFormat().resolvedOptions().timeZone?.startsWith('Asia/Calcutta') ||
                  Intl.DateTimeFormat().resolvedOptions().timeZone?.startsWith('Asia/Kolkata');

  const plans = [
    {
      name: 'Free',
      price: '0',
      period: 'forever',
      desc: 'Perfect for getting started',
      features: [
        '1 site, 2 tanks',
        '7-day history',
        'Low water alerts',
        'Real-time dashboard',
        'Home Assistant / MQTT',
      ],
      cta: 'Get Started Free',
      highlight: false,
    },
    {
      name: 'Home',
      price: isIndia ? '99' : '1.99',
      currency: isIndia ? '₹' : '$',
      period: '/month',
      annual: isIndia ? '₹799/year (save 33%)' : '$19/year (save 20%)',
      desc: 'For homes and small properties',
      features: [
        '1 site, up to 4 tanks',
        '6-month history',
        'All alert types (low, high, battery, offline)',
        'Push notifications to 3 devices',
        'Email alert backup',
        'Email support',
      ],
      cta: 'Start Free Trial',
      highlight: true,
    },
    {
      name: 'Pro',
      price: isIndia ? '599' : '9.99',
      currency: isIndia ? '₹' : '$',
      period: '/month',
      annual: isIndia ? '₹4,999/year (save 30%)' : '$99/year (save 17%)',
      desc: 'For installers and property managers',
      features: [
        'Up to 10 sites, 50 tanks',
        '1-year history',
        'Fleet dashboard',
        'Client sharing (read-only links)',
        'API access',
        'Priority support (24hr SLA)',
      ],
      cta: 'Contact Sales',
      highlight: false,
    },
  ];

  return (
    <section id="pricing" className="py-24 px-6 bg-gradient-to-b from-slate-950 via-slate-900 to-slate-950">
      <div className="max-w-6xl mx-auto">
        <FadeIn>
          <p className="text-sky-400 font-medium text-sm tracking-wider uppercase mb-4 text-center">Pricing</p>
          <h2 className="text-3xl sm:text-5xl font-bold text-center mb-4">Simple, transparent pricing</h2>
          <p className="text-slate-400 text-center mb-16 text-lg">Start free. Upgrade when you need more.</p>
        </FadeIn>

        <div className="grid md:grid-cols-3 gap-6 max-w-5xl mx-auto">
          {plans.map((plan, i) => (
            <FadeIn key={i} delay={i * 150}>
              <div className={`rounded-2xl p-8 relative ${
                plan.highlight
                  ? 'bg-gradient-to-b from-sky-500/10 to-slate-900 border-2 border-sky-500/50 shadow-lg shadow-sky-500/10'
                  : 'bg-slate-900/50 border border-slate-800'
              }`}>
                {plan.highlight && (
                  <div className="absolute -top-3 left-1/2 -translate-x-1/2 bg-sky-500 text-white text-xs font-bold px-3 py-1 rounded-full">
                    Most Popular
                  </div>
                )}
                <h3 className="text-xl font-bold mb-1">{plan.name}</h3>
                <p className="text-slate-400 text-sm mb-4">{plan.desc}</p>
                <div className="flex items-baseline gap-1 mb-1">
                  <span className="text-slate-400 text-lg">{plan.currency || ''}</span>
                  <span className="text-4xl font-bold">{plan.price}</span>
                  <span className="text-slate-400 text-sm">{plan.period}</span>
                </div>
                {plan.annual && <p className="text-emerald-400 text-xs mb-6">{plan.annual}</p>}
                {!plan.annual && <div className="mb-6" />}
                <ul className="space-y-3 mb-8">
                  {plan.features.map((f, j) => (
                    <li key={j} className="flex items-start gap-2 text-sm">
                      <svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="#0EA5E9" strokeWidth="2.5" strokeLinecap="round" className="flex-shrink-0 mt-0.5">
                        <path d="M20 6L9 17l-5-5" />
                      </svg>
                      <span className="text-slate-300">{f}</span>
                    </li>
                  ))}
                </ul>
                <button onClick={onGetStarted}
                  className={`w-full py-3 rounded-xl font-semibold text-sm transition-all active:scale-95 ${
                    plan.highlight
                      ? 'bg-sky-500 hover:bg-sky-400 text-white shadow-lg shadow-sky-500/25'
                      : 'border border-slate-700 hover:border-slate-500 text-slate-300'
                  }`}>
                  {plan.cta}
                </button>
              </div>
            </FadeIn>
          ))}
        </div>

        <FadeIn delay={500}>
          <p className="text-center text-slate-500 text-sm mt-8">
            All plans include MQTT export and Home Assistant integration. Open-source firmware — always free.
          </p>
        </FadeIn>
      </div>
    </section>
  );
}

// ─── HARDWARE ───────────────────────────────────────────────────────────────
function Hardware() {
  return (
    <section id="hardware" className="py-24 px-6">
      <div className="max-w-6xl mx-auto">
        <FadeIn>
          <p className="text-sky-400 font-medium text-sm tracking-wider uppercase mb-4 text-center">Hardware Kits</p>
          <h2 className="text-3xl sm:text-5xl font-bold text-center mb-4">Ready to install. No soldering required.</h2>
          <p className="text-slate-400 text-center mb-16 text-lg max-w-2xl mx-auto">
            Pre-assembled, pre-flashed, and tested. Scan the QR code on the box to connect — done in 5 minutes.
          </p>
        </FadeIn>

        <div className="grid md:grid-cols-2 gap-8 max-w-4xl mx-auto">
          <FadeIn>
            <div className="bg-slate-900/50 border border-slate-800 rounded-2xl overflow-hidden hover:border-sky-500/30 transition-all">
              <div className="bg-slate-800 aspect-[4/3] flex items-center justify-center">
                <p className="text-slate-600 text-xs">Single Tank Kit photo — 800x600</p>
              </div>
              <div className="p-6">
                <h3 className="font-bold text-xl mb-1">Single Tank Kit</h3>
                <p className="text-slate-400 text-sm mb-4">1 Receiver + 1 Transmitter + Sensor + Cables</p>
                <div className="flex items-baseline gap-2 mb-4">
                  <span className="text-3xl font-bold">₹7,499</span>
                  <span className="text-slate-500 text-sm line-through">₹8,999</span>
                </div>
                <a href="mailto:ravi@thetechposts.org?subject=TankSync Single Tank Kit Order"
                  className="block w-full py-3 bg-sky-500 hover:bg-sky-400 text-white text-center font-semibold rounded-xl transition-all active:scale-95">
                  Order Now
                </a>
              </div>
            </div>
          </FadeIn>

          <FadeIn delay={150}>
            <div className="bg-slate-900/50 border border-slate-800 rounded-2xl overflow-hidden hover:border-sky-500/30 transition-all">
              <div className="bg-slate-800 aspect-[4/3] flex items-center justify-center">
                <p className="text-slate-600 text-xs">Dual Tank Kit photo — 800x600</p>
              </div>
              <div className="p-6">
                <h3 className="font-bold text-xl mb-1">Dual Tank Kit</h3>
                <p className="text-slate-400 text-sm mb-4">1 Receiver + 2 Transmitters + Sensors + Cables</p>
                <div className="flex items-baseline gap-2 mb-4">
                  <span className="text-3xl font-bold">₹11,999</span>
                  <span className="text-slate-500 text-sm line-through">₹14,499</span>
                </div>
                <a href="mailto:ravi@thetechposts.org?subject=TankSync Dual Tank Kit Order"
                  className="block w-full py-3 bg-sky-500 hover:bg-sky-400 text-white text-center font-semibold rounded-xl transition-all active:scale-95">
                  Order Now
                </a>
              </div>
            </div>
          </FadeIn>
        </div>

        <FadeIn delay={300}>
          <div className="text-center mt-12">
            <p className="text-slate-400 text-sm mb-2">Prefer DIY? Build your own for ~₹3,500</p>
            <a href="https://github.com/Techposts/LoRa-Water-Tank-Monitor" target="_blank" rel="noopener"
              className="text-sky-400 text-sm font-medium hover:underline">
              View open-source build guide on GitHub
            </a>
          </div>
        </FadeIn>
      </div>
    </section>
  );
}

// ─── FOOTER ─────────────────────────────────────────────────────────────────
function Footer() {
  return (
    <footer className="py-16 px-6 border-t border-slate-800">
      <div className="max-w-6xl mx-auto">
        <div className="grid sm:grid-cols-3 gap-12 mb-12">
          <div>
            <div className="flex items-center gap-2 mb-4">
              <svg width="24" height="24" viewBox="0 0 40 40" fill="none">
                <path d="M20 4C20 4 8 18 8 26a12 12 0 0 0 24 0C32 18 20 4 20 4z" fill="#0EA5E9" />
              </svg>
              <span className="font-bold text-lg">Tank<span className="text-sky-400">Sync</span></span>
            </div>
            <p className="text-slate-400 text-sm leading-relaxed">
              Smart wireless water tank monitoring. Built in India, open-source at heart.
            </p>
          </div>

          <div>
            <h4 className="font-semibold mb-4">Product</h4>
            <div className="space-y-2 text-sm text-slate-400">
              <a href="#features" className="block hover:text-white transition-colors">Features</a>
              <a href="#pricing" className="block hover:text-white transition-colors">Pricing</a>
              <a href="#hardware" className="block hover:text-white transition-colors">Hardware Kits</a>
              <a href="https://github.com/Techposts/LoRa-Water-Tank-Monitor" target="_blank" rel="noopener" className="block hover:text-white transition-colors">GitHub</a>
            </div>
          </div>

          <div>
            <h4 className="font-semibold mb-4">Contact</h4>
            <div className="space-y-2 text-sm text-slate-400">
              <a href="mailto:ravi@thetechposts.org" className="block hover:text-white transition-colors">ravi@thetechposts.org</a>
              <a href="https://www.youtube.com/@ravis1ngh" target="_blank" rel="noopener" className="block hover:text-white transition-colors">YouTube</a>
              <a href="https://github.com/Techposts" target="_blank" rel="noopener" className="block hover:text-white transition-colors">GitHub</a>
            </div>
          </div>
        </div>

        <div className="border-t border-slate-800 pt-8 flex flex-col sm:flex-row items-center justify-between gap-4">
          <p className="text-slate-500 text-xs">Built by Ravi Singh. Open-source firmware, cloud-powered monitoring.</p>
          <p className="text-slate-600 text-xs">A <span className="text-slate-400">SmartGhar</span> product</p>
        </div>
      </div>
    </footer>
  );
}
