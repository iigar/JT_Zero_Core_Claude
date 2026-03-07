import React from 'react';
import { Activity, Wifi, WifiOff, Cpu, Zap, Clock } from 'lucide-react';

export default function Header({ state, connected }) {
  const modeColor = {
    IDLE: 'text-slate-400',
    ARMED: 'text-amber-400 glow-text-warn',
    TAKEOFF: 'text-cyan-400 glow-text',
    HOVER: 'text-emerald-400',
    NAVIGATE: 'text-blue-400',
    LAND: 'text-amber-400 glow-text-warn',
    RTL: 'text-orange-400',
    EMERGENCY: 'text-red-500 glow-text-danger animate-pulse',
  };

  return (
    <header
      data-testid="header-bar"
      className="flex items-center justify-between px-4 py-2 bg-[#0A0C10] border-b border-[#1E293B] shrink-0"
    >
      <div className="flex items-center gap-4">
        <div className="flex items-center gap-2">
          <Zap className="w-5 h-5 text-[#00F0FF]" />
          <span className="text-sm font-bold tracking-widest text-[#00F0FF] uppercase glow-text">
            JT-Zero
          </span>
          <span className="text-[10px] text-slate-600 ml-1">v1.0</span>
        </div>

        <div className="h-4 w-px bg-[#1E293B]" />

        <div className="flex items-center gap-2" data-testid="flight-mode-badge">
          <div className={`w-2 h-2 rounded-full ${
            state?.armed 
              ? 'bg-amber-400 shadow-[0_0_8px_rgba(251,191,36,0.5)]' 
              : 'bg-slate-600'
          }`} />
          <span className={`text-xs font-bold uppercase tracking-wider ${
            modeColor[state?.flight_mode] || 'text-slate-400'
          }`}>
            {state?.flight_mode || 'N/A'}
          </span>
        </div>

        <div className="h-4 w-px bg-[#1E293B]" />

        <div className="flex items-center gap-1">
          {connected ? (
            <Wifi className="w-3.5 h-3.5 text-emerald-400" />
          ) : (
            <WifiOff className="w-3.5 h-3.5 text-red-400 animate-pulse" />
          )}
          <span className={`text-[10px] uppercase tracking-wider ${
            connected ? 'text-emerald-400' : 'text-red-400'
          }`}>
            {connected ? 'LINK' : 'NO LINK'}
          </span>
        </div>
      </div>

      <div className="flex items-center gap-6 text-[10px] text-slate-500 uppercase tracking-wider">
        <div className="flex items-center gap-1.5" data-testid="heartbeat-indicator">
          <Activity className={`w-3 h-3 ${connected ? 'text-emerald-400 animate-pulse' : 'text-slate-700'}`} />
          <span>HB</span>
        </div>

        <div className="flex items-center gap-1.5">
          <Cpu className="w-3 h-3" />
          <span className="tabular-nums">{state?.cpu_temp?.toFixed(1) || '0.0'}C</span>
        </div>

        <div className="flex items-center gap-1.5">
          <Clock className="w-3 h-3" />
          <span className="tabular-nums">T+{state?.uptime_sec || 0}s</span>
        </div>

        <div className="flex items-center gap-1">
          <div className={`w-1.5 h-3 rounded-sm ${
            (state?.battery_percent || 0) > 50 ? 'bg-emerald-400' :
            (state?.battery_percent || 0) > 20 ? 'bg-amber-400' : 'bg-red-500 animate-pulse'
          }`} />
          <span className="tabular-nums">
            {state?.battery_voltage?.toFixed(1) || '0.0'}V
            <span className="text-slate-600 ml-1">
              ({state?.battery_percent?.toFixed(0) || '0'}%)
            </span>
          </span>
        </div>
      </div>
    </header>
  );
}
