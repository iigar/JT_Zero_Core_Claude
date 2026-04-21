import React from 'react';
import { Activity, Wifi, WifiOff, Cpu, Zap, Clock } from 'lucide-react';

const BATTERY_COLOR = (pct) => {
  if (pct > 50) return { text: 'text-emerald-400', fill: '#34d399', glow: 'rgba(52,211,153,0.5)' };
  if (pct > 20) return { text: 'text-amber-400',   fill: '#fbbf24', glow: 'rgba(251,191,36,0.5)' };
  return             { text: 'text-red-500',        fill: '#ef4444', glow: 'rgba(239,68,68,0.6)' };
};

const MODE_COLOR = {
  IDLE:      'text-slate-400',
  ARMED:     'text-amber-400 glow-text-warn',
  TAKEOFF:   'text-cyan-400 glow-text',
  HOVER:     'text-emerald-400',
  NAVIGATE:  'text-blue-400',
  LAND:      'text-amber-400 glow-text-warn',
  RTL:       'text-orange-400',
  EMERGENCY: 'text-red-500 glow-text-danger animate-pulse',
};

export default function Header({ state, connected, runtimeMode }) {
  const pct = state?.battery_percent ?? 0;
  const bat = BATTERY_COLOR(pct);
  const isEmergency = state?.flight_mode === 'EMERGENCY';

  return (
    <header
      data-testid="header-bar"
      className={`flex items-center justify-between px-4 py-0 bg-[#080A0F] border-b border-[#1E293B] shrink-0 relative overflow-hidden ${isEmergency ? 'emergency-flash' : ''}`}
      style={{ height: '42px', minHeight: '42px' }}
    >
      {/* Subtle top accent line */}
      <div
        className="absolute top-0 left-0 right-0 h-px pointer-events-none"
        style={{ background: 'linear-gradient(90deg, transparent, rgba(0,240,255,0.3) 30%, rgba(0,240,255,0.6) 50%, rgba(0,240,255,0.3) 70%, transparent)' }}
      />

      {/* LEFT: Logo + mode + link */}
      <div className="flex items-center gap-4 h-full">

        {/* Logo */}
        <div className="flex items-center gap-2 relative">
          <div className="relative">
            <Zap className="w-4 h-4 text-[#00F0FF]" style={{ filter: 'drop-shadow(0 0 4px rgba(0,240,255,0.7))' }} />
          </div>
          <div className="flex items-baseline gap-1.5">
            <span
              className="text-[13px] font-bold tracking-[0.2em] text-[#00F0FF] uppercase"
              style={{ textShadow: '0 0 12px rgba(0,240,255,0.6), 0 0 24px rgba(0,240,255,0.2)', fontFamily: "'Share Tech Mono', monospace" }}
            >
              JT-ZERO
            </span>
            <span className="text-[9px] text-slate-600 tracking-widest">v1.0</span>
          </div>
        </div>

        {/* Native badge — hidden on mobile */}
        {runtimeMode === 'native' && (
          <div className="hidden lg:block text-[9px] font-bold px-2 py-0.5 border border-emerald-500/30 text-emerald-300 uppercase tracking-wider"
               style={{ background: 'rgba(52,211,153,0.07)', borderRadius: '3px' }}>
            C++ NATIVE
          </div>
        )}

        <div className="hidden lg:block w-px h-4 bg-[#1E293B]" />

        {/* Flight mode */}
        <div className="flex items-center gap-2" data-testid="flight-mode-badge">
          <div
            className="w-1.5 h-1.5 rounded-full"
            style={{
              background: state?.armed ? '#fbbf24' : '#334155',
              boxShadow: state?.armed ? '0 0 6px rgba(251,191,36,0.7)' : 'none',
            }}
          />
          <span
            className={`text-[11px] font-bold uppercase tracking-[0.15em] readout ${MODE_COLOR[state?.flight_mode] || 'text-slate-400'}`}
          >
            {state?.flight_mode || 'N/A'}
          </span>
        </div>

        <div className="w-px h-4 bg-[#1E293B]" />

        <div className="hidden lg:block w-px h-4 bg-[#1E293B]" />

        {/* Connection */}
        <div className="flex items-center gap-1.5">
          {connected ? (
            <>
              <Wifi className="w-3 h-3 text-emerald-400" />
              <span className="text-[9px] uppercase tracking-wider text-emerald-400">LINK</span>
            </>
          ) : (
            <>
              <WifiOff className="w-3 h-3 text-red-400 animate-pulse" />
              <span className="text-[9px] uppercase tracking-wider text-red-400">NO LINK</span>
            </>
          )}
        </div>
      </div>

      {/* RIGHT: Telemetry strip */}
      <div className="flex items-center gap-5 h-full">

        {/* Heartbeat — hidden on mobile */}
        <div className="hidden lg:flex items-center gap-1.5" data-testid="heartbeat-indicator">
          <Activity
            className={`w-3 h-3 ${connected ? 'text-emerald-400' : 'text-slate-700'}`}
            style={connected ? { filter: 'drop-shadow(0 0 3px rgba(52,211,153,0.7))' } : {}}
          />
          <span className="text-[9px] uppercase tracking-wider text-slate-500">HB</span>
        </div>

        {/* CPU temp — hidden on mobile */}
        <div className="hidden lg:flex items-center gap-1.5">
          <Cpu className="w-3 h-3 text-slate-500" />
          <span className="readout text-[11px] text-slate-300 tabular-nums">
            {state?.cpu_temp?.toFixed(1) ?? '—'}
            <span className="text-slate-600 text-[9px] ml-0.5">°C</span>
          </span>
        </div>

        {/* Uptime — hidden on mobile */}
        <div className="hidden lg:flex items-center gap-1.5">
          <Clock className="w-3 h-3 text-slate-500" />
          <span className="readout text-[11px] text-slate-300 tabular-nums">
            T+{state?.uptime_sec ?? 0}
            <span className="text-slate-600 text-[9px] ml-0.5">s</span>
          </span>
        </div>

        {/* Battery */}
        <div className="flex items-center gap-2">
          {/* Bar meter */}
          <div
            className={`relative flex items-center ${bat.text}`}
            title={`${pct.toFixed(0)}%`}
          >
            <div
              className="battery-bar"
              style={{ color: bat.fill }}
            >
              <div
                className="battery-bar-fill"
                style={{
                  width: `${Math.max(0, Math.min(100, pct))}%`,
                  background: bat.fill,
                  boxShadow: pct < 20 ? `0 0 4px ${bat.glow}` : 'none',
                }}
              />
            </div>
          </div>

          <div className="flex flex-col items-start leading-none gap-0.5">
            <span className={`readout text-[11px] tabular-nums font-medium ${bat.text}`}>
              {state?.battery_voltage?.toFixed(1) ?? '—'}
              <span className="text-[9px] ml-0.5 opacity-60">V</span>
            </span>
            <span className="readout text-[9px] tabular-nums text-slate-500">
              {pct.toFixed(0)}%
            </span>
          </div>
        </div>
      </div>

      {/* Bottom fade line */}
      <div
        className="absolute bottom-0 left-0 right-0 h-px pointer-events-none"
        style={{ background: 'linear-gradient(90deg, transparent, #1E293B 20%, #1E293B 80%, transparent)' }}
      />
    </header>
  );
}
