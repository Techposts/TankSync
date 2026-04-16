// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (c) 2025-2026 Ravi Singh (Techposts)

import { useMemo } from 'react';

/**
 * 3D-style water tank with realistic wave animation.
 * Uses layered SVG with gradients, reflections, and animated wave paths
 * for a premium, Apple-like feel.
 */
export default function TankVisualization({ percent = 0, size = 'lg', state = 'online', showLabel = true }) {
  const pct = Math.max(0, Math.min(100, percent));
  const isActive = state === 'online' || state === 'stale';

  // Dimensions
  const dims = size === 'sm'
    ? { w: 90, h: 130, tankW: 60, tankH: 90, rx: 14 }
    : size === 'md'
    ? { w: 140, h: 190, tankW: 90, tankH: 140, rx: 18 }
    : { w: 200, h: 280, tankW: 130, tankH: 200, rx: 24 };

  const { w, h, tankW, tankH, rx } = dims;
  const tankX = (w - tankW) / 2;
  const tankY = h - tankH - 20; // room for legs + label
  const waterH = (pct / 100) * (tankH - 8); // 8px padding inside tank
  const waterY = tankY + tankH - 4 - waterH; // 4px bottom padding

  // Color scheme based on level
  const colors = useMemo(() => {
    if (!isActive) return {
      water1: '#334155', water2: '#1E293B', water3: '#475569',
      highlight: '#475569', glow: 'rgba(71,85,105,0.2)'
    };
    if (pct <= 15) return {
      water1: '#EF4444', water2: '#B91C1C', water3: '#F87171',
      highlight: '#FCA5A5', glow: 'rgba(239,68,68,0.3)'
    };
    if (pct <= 35) return {
      water1: '#F59E0B', water2: '#B45309', water3: '#FBBF24',
      highlight: '#FDE68A', glow: 'rgba(245,158,11,0.3)'
    };
    return {
      water1: '#0EA5E9', water2: '#0369A1', water3: '#38BDF8',
      highlight: '#7DD3FC', glow: 'rgba(14,165,233,0.3)'
    };
  }, [pct, isActive]);

  const waveAmp = size === 'sm' ? 3 : size === 'md' ? 4 : 6;
  const uid = `tank-${size}-${Math.random().toString(36).slice(2, 6)}`;

  return (
    <div className="relative inline-flex flex-col items-center">
      <svg width={w} height={h} viewBox={`0 0 ${w} ${h}`}>
        <defs>
          {/* Tank body gradient — metallic silver look */}
          <linearGradient id={`${uid}-tankBody`} x1="0" y1="0" x2="1" y2="0">
            <stop offset="0%" stopColor="#1E293B" />
            <stop offset="15%" stopColor="#334155" />
            <stop offset="50%" stopColor="#475569" />
            <stop offset="85%" stopColor="#334155" />
            <stop offset="100%" stopColor="#1E293B" />
          </linearGradient>

          {/* Tank inner shadow */}
          <linearGradient id={`${uid}-tankInner`} x1="0" y1="0" x2="1" y2="0">
            <stop offset="0%" stopColor="#020617" />
            <stop offset="20%" stopColor="#0F172A" />
            <stop offset="80%" stopColor="#0F172A" />
            <stop offset="100%" stopColor="#020617" />
          </linearGradient>

          {/* Water gradient — deep to light (3D depth) */}
          <linearGradient id={`${uid}-water`} x1="0" y1="0" x2="0" y2="1">
            <stop offset="0%" stopColor={colors.water3} stopOpacity="0.9" />
            <stop offset="40%" stopColor={colors.water1} stopOpacity="0.95" />
            <stop offset="100%" stopColor={colors.water2} stopOpacity="1" />
          </linearGradient>

          {/* Water horizontal 3D curve */}
          <linearGradient id={`${uid}-waterH`} x1="0" y1="0" x2="1" y2="0">
            <stop offset="0%" stopColor={colors.water2} stopOpacity="0.8" />
            <stop offset="30%" stopColor={colors.water1} stopOpacity="0.5" />
            <stop offset="50%" stopColor={colors.water3} stopOpacity="0.3" />
            <stop offset="70%" stopColor={colors.water1} stopOpacity="0.5" />
            <stop offset="100%" stopColor={colors.water2} stopOpacity="0.8" />
          </linearGradient>

          {/* Glass reflection */}
          <linearGradient id={`${uid}-glassL`} x1="0" y1="0" x2="1" y2="0">
            <stop offset="0%" stopColor="white" stopOpacity="0.12" />
            <stop offset="100%" stopColor="white" stopOpacity="0" />
          </linearGradient>
          <linearGradient id={`${uid}-glassR`} x1="1" y1="0" x2="0" y2="0">
            <stop offset="0%" stopColor="white" stopOpacity="0.06" />
            <stop offset="100%" stopColor="white" stopOpacity="0" />
          </linearGradient>

          {/* Water surface glow */}
          <radialGradient id={`${uid}-surfaceGlow`} cx="0.5" cy="0" r="0.8">
            <stop offset="0%" stopColor={colors.highlight} stopOpacity="0.5" />
            <stop offset="100%" stopColor={colors.water1} stopOpacity="0" />
          </radialGradient>

          {/* Clip to tank interior */}
          <clipPath id={`${uid}-clip`}>
            <rect x={tankX + 3} y={tankY + 3} width={tankW - 6} height={tankH - 6} rx={rx - 3} />
          </clipPath>

          {/* Drop shadow filter */}
          <filter id={`${uid}-shadow`} x="-10%" y="-5%" width="120%" height="115%">
            <feDropShadow dx="0" dy="4" stdDeviation="6" floodColor={colors.glow} floodOpacity="0.8" />
          </filter>

          {/* Inner glow for water */}
          <filter id={`${uid}-innerGlow`}>
            <feGaussianBlur in="SourceGraphic" stdDeviation="2" result="blur" />
            <feComposite in="SourceGraphic" in2="blur" operator="over" />
          </filter>
        </defs>

        {/* === TANK CAP === */}
        <rect
          x={w / 2 - tankW * 0.2} y={tankY - 8}
          width={tankW * 0.4} height={12}
          rx={4}
          fill={`url(#${uid}-tankBody)`}
          stroke="#64748B" strokeWidth="0.5"
        />
        {/* Cap highlight */}
        <rect
          x={w / 2 - tankW * 0.15} y={tankY - 7}
          width={tankW * 0.3} height={4}
          rx={2}
          fill="white" opacity="0.08"
        />

        {/* === TANK BODY (outer shell — 3D metallic) === */}
        <rect
          x={tankX} y={tankY} width={tankW} height={tankH} rx={rx}
          fill={`url(#${uid}-tankBody)`}
          stroke="#64748B" strokeWidth="1"
          filter={`url(#${uid}-shadow)`}
        />

        {/* === TANK INTERIOR (dark) === */}
        <rect
          x={tankX + 3} y={tankY + 3}
          width={tankW - 6} height={tankH - 6}
          rx={rx - 3}
          fill={`url(#${uid}-tankInner)`}
        />

        {/* === WATER FILL (clipped to interior) === */}
        <g clipPath={`url(#${uid}-clip)`}>
          {pct > 0 && (
            <>
              {/* Main water body */}
              <rect
                x={tankX + 3} y={waterY + waveAmp}
                width={tankW - 6} height={waterH + 10}
                fill={`url(#${uid}-water)`}
              />

              {/* Horizontal 3D shading overlay on water */}
              <rect
                x={tankX + 3} y={waterY + waveAmp}
                width={tankW - 6} height={waterH + 10}
                fill={`url(#${uid}-waterH)`}
              />

              {/* === WAVE ANIMATIONS === */}
              {isActive && (
                <>
                  {/* Wave layer 1 — front wave */}
                  <path
                    d={generateWavePath(tankX + 3, waterY, tankW - 6, waveAmp, 0)}
                    fill={colors.water3}
                    opacity="0.5"
                    className="wave-1"
                  >
                    <animateTransform
                      attributeName="transform" type="translate"
                      values={`0,0; ${-tankW * 0.5},0; 0,0`}
                      dur="7s" repeatCount="indefinite"
                    />
                  </path>

                  {/* Wave layer 2 — back wave (offset) */}
                  <path
                    d={generateWavePath(tankX + 3, waterY + 2, tankW - 6, waveAmp * 0.7, Math.PI)}
                    fill={colors.water1}
                    opacity="0.35"
                    className="wave-2"
                  >
                    <animateTransform
                      attributeName="transform" type="translate"
                      values={`0,0; ${tankW * 0.3},0; 0,0`}
                      dur="5s" repeatCount="indefinite"
                    />
                  </path>

                  {/* Wave layer 3 — subtle ripple */}
                  <path
                    d={generateWavePath(tankX + 3, waterY + 1, tankW - 6, waveAmp * 0.4, Math.PI * 0.5)}
                    fill={colors.highlight}
                    opacity="0.15"
                    className="wave-3"
                  >
                    <animateTransform
                      attributeName="transform" type="translate"
                      values={`0,0; ${-tankW * 0.25},0; 0,0`}
                      dur="11s" repeatCount="indefinite"
                    />
                  </path>

                  {/* Surface glow — light reflection on water surface */}
                  <ellipse
                    cx={w / 2} cy={waterY + 2}
                    rx={tankW * 0.35} ry={waveAmp + 3}
                    fill={`url(#${uid}-surfaceGlow)`}
                  >
                    <animate
                      attributeName="rx"
                      values={`${tankW * 0.3};${tankW * 0.4};${tankW * 0.3}`}
                      dur="4s" repeatCount="indefinite"
                    />
                  </ellipse>
                </>
              )}

              {/* Bubbles (only when level is changing / active) */}
              {isActive && pct > 5 && pct < 95 && (
                <g opacity="0.4">
                  <Bubble cx={tankX + tankW * 0.3} startY={tankY + tankH - 10} endY={waterY + 5} r={2} dur="3s" delay="0s" />
                  <Bubble cx={tankX + tankW * 0.6} startY={tankY + tankH - 15} endY={waterY + 8} r={1.5} dur="4s" delay="1s" />
                  <Bubble cx={tankX + tankW * 0.45} startY={tankY + tankH - 8} endY={waterY + 3} r={2.5} dur="5s" delay="2s" />
                </g>
              )}
            </>
          )}

          {/* Tick marks on interior wall */}
          {[25, 50, 75].map(tick => {
            const y = tankY + tankH - 4 - (tick / 100) * (tankH - 8);
            return (
              <g key={tick} opacity="0.25">
                <line x1={tankX + 5} y1={y} x2={tankX + 12} y2={y} stroke="#94A3B8" strokeWidth="0.8" />
                {size !== 'sm' && (
                  <text x={tankX + 15} y={y + 3} fill="#64748B" fontSize="8" fontFamily="JetBrains Mono">
                    {tick}
                  </text>
                )}
              </g>
            );
          })}
        </g>

        {/* === GLASS REFLECTIONS (on top of everything) === */}
        {/* Left highlight streak */}
        <rect
          x={tankX + 5} y={tankY + 8}
          width={6} height={tankH - 16}
          rx={3}
          fill={`url(#${uid}-glassL)`}
        />
        {/* Right subtle reflection */}
        <rect
          x={tankX + tankW - 14} y={tankY + 12}
          width={5} height={tankH - 24}
          rx={2.5}
          fill={`url(#${uid}-glassR)`}
        />

        {/* === TANK OUTLINE (crisp border on top) === */}
        <rect
          x={tankX} y={tankY} width={tankW} height={tankH} rx={rx}
          fill="none" stroke="#475569" strokeWidth="1.5"
        />

        {/* === TANK LEGS === */}
        <rect x={tankX + 10} y={tankY + tankH} width={8} height={12} rx={2}
          fill="#334155" stroke="#475569" strokeWidth="0.5" />
        <rect x={tankX + tankW - 18} y={tankY + tankH} width={8} height={12} rx={2}
          fill="#334155" stroke="#475569" strokeWidth="0.5" />
        {/* Leg shadow */}
        <ellipse cx={tankX + 14} cy={tankY + tankH + 13} rx={8} ry={2} fill="#000" opacity="0.15" />
        <ellipse cx={tankX + tankW - 14} cy={tankY + tankH + 13} rx={8} ry={2} fill="#000" opacity="0.15" />

        {/* === PERCENTAGE TEXT === */}
        {showLabel && size !== 'sm' && (
          <>
            {/* Text shadow for readability */}
            <text
              x={w / 2} y={tankY + tankH / 2 + (size === 'lg' ? 12 : 8)}
              textAnchor="middle"
              fill="#000" opacity="0.4"
              fontSize={size === 'lg' ? 40 : 28}
              fontWeight="800"
              fontFamily="JetBrains Mono, monospace"
            >
              {pct > 0 || isActive ? Math.round(pct) : '--'}
              <tspan fontSize={size === 'lg' ? 20 : 14} dy="-4">%</tspan>
            </text>
            {/* Actual text */}
            <text
              x={w / 2} y={tankY + tankH / 2 + (size === 'lg' ? 11 : 7)}
              textAnchor="middle"
              fill="#FFFFFF"
              fontSize={size === 'lg' ? 40 : 28}
              fontWeight="800"
              fontFamily="JetBrains Mono, monospace"
            >
              {pct > 0 || isActive ? Math.round(pct) : '--'}
              <tspan fontSize={size === 'lg' ? 20 : 14} fill="#94A3B8" dy="-4">%</tspan>
            </text>
          </>
        )}
      </svg>

      {/* Status indicator below */}
      {showLabel && (
        <div className="mt-1 flex items-center gap-1.5">
          <div className={`w-2 h-2 rounded-full ${
            state === 'online' ? 'bg-success pulse-live' :
            state === 'stale' ? 'bg-warning' :
            'bg-slate-500'
          }`} />
          <span className="text-xs text-slate-400 capitalize">{state}</span>
        </div>
      )}
    </div>
  );
}

/** Generate a smooth wave path with sinusoidal curves */
function generateWavePath(x, y, width, amplitude, phaseOffset = 0) {
  const segments = 8;
  const segW = (width * 2) / segments; // double width for seamless looping
  let d = `M${x - width * 0.5},${y}`;

  for (let i = 0; i <= segments; i++) {
    const px = x - width * 0.5 + i * segW;
    const py = y + Math.sin((i / segments) * Math.PI * 4 + phaseOffset) * amplitude;
    if (i === 0) {
      d = `M${px},${py}`;
    } else {
      const cpx = px - segW / 2;
      const prevPy = y + Math.sin(((i - 1) / segments) * Math.PI * 4 + phaseOffset) * amplitude;
      d += ` C${cpx},${prevPy} ${cpx},${py} ${px},${py}`;
    }
  }

  // Close path downward
  d += ` L${x + width * 1.5},${y + amplitude + 200} L${x - width * 0.5},${y + amplitude + 200} Z`;
  return d;
}

/** Animated bubble rising through water */
function Bubble({ cx, startY, endY, r, dur, delay }) {
  return (
    <circle cx={cx} cy={startY} r={r} fill="white" opacity="0">
      <animate attributeName="cy" values={`${startY};${endY}`} dur={dur} begin={delay} repeatCount="indefinite" />
      <animate attributeName="opacity" values="0;0.5;0.3;0" dur={dur} begin={delay} repeatCount="indefinite" />
      <animate attributeName="r" values={`${r};${r * 1.3};${r * 0.8}`} dur={dur} begin={delay} repeatCount="indefinite" />
    </circle>
  );
}
