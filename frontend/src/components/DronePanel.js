import React from 'react';
import { LineChart, Line, ResponsiveContainer, YAxis, CartesianGrid } from 'recharts';
import { Compass, ArrowUp, Navigation } from 'lucide-react';

function AttitudeIndicator({ roll, pitch }) {
  const r = roll || 0;
  const p = pitch || 0;
  const clampPitch = Math.max(-45, Math.min(45, p));

  return (
    <div className="relative w-full aspect-square max-w-[120px] mx-auto">
      <svg viewBox="0 0 100 100" className="w-full h-full">
        <defs>
          <clipPath id="ai-clip">
            <circle cx="50" cy="50" r="45" />
          </clipPath>
        </defs>
        <circle cx="50" cy="50" r="46" fill="none" stroke="#1E293B" strokeWidth="1" />
        <g clipPath="url(#ai-clip)">
          <g transform={`rotate(${-r}, 50, 50) translate(0, ${clampPitch * 0.8})`}>
            <rect x="0" y="0" width="100" height="50" fill="#0369a1" />
            <rect x="0" y="50" width="100" height="50" fill="#854d0e" />
            <line x1="10" y1="50" x2="90" y2="50" stroke="white" strokeWidth="1" />
            <line x1="25" y1="40" x2="75" y2="40" stroke="white" strokeWidth="0.3" opacity="0.5" />
            <line x1="25" y1="60" x2="75" y2="60" stroke="white" strokeWidth="0.3" opacity="0.5" />
          </g>
        </g>
        {/* Aircraft symbol */}
        <line x1="25" y1="50" x2="42" y2="50" stroke="#00F0FF" strokeWidth="2" />
        <line x1="58" y1="50" x2="75" y2="50" stroke="#00F0FF" strokeWidth="2" />
        <circle cx="50" cy="50" r="3" fill="none" stroke="#00F0FF" strokeWidth="1.5" />
      </svg>
    </div>
  );
}

export default function DronePanel({ state, history }) {
  return (
    <div className="panel-glass p-3 relative corner-bracket" data-testid="drone-panel">
      <h3 className="text-[10px] uppercase tracking-widest text-slate-500 mb-3 font-semibold">
        Drone Telemetry
      </h3>

      <div className="grid grid-cols-3 gap-3">
        {/* Attitude */}
        <div className="flex flex-col items-center">
          <AttitudeIndicator roll={state?.roll} pitch={state?.pitch} />
          <div className="mt-1 text-center">
            <div className="text-[9px] text-slate-600">ATTITUDE</div>
          </div>
        </div>

        {/* Data */}
        <div className="space-y-1">
          <Row label="ROLL" value={`${(state?.roll || 0).toFixed(2)}`} unit="deg" />
          <Row label="PITCH" value={`${(state?.pitch || 0).toFixed(2)}`} unit="deg" />
          <Row label="YAW" value={`${(state?.yaw || 0).toFixed(1)}`} unit="deg" />
          <div className="border-t border-[#1E293B]/30 my-1" />
          <Row label="ALT AGL" value={`${(state?.altitude_agl || 0).toFixed(2)}`} unit="m" />
          <Row label="GPS ALT" value={`${(state?.gps?.alt || 0).toFixed(1)}`} unit="m" />
          <Row label="GND SPD" value={`${(state?.gps?.speed || 0).toFixed(1)}`} unit="m/s" />
        </div>

        {/* Heading compass */}
        <div className="flex flex-col items-center">
          <div className="relative w-[80px] h-[80px]">
            <svg viewBox="0 0 100 100" className="w-full h-full">
              <circle cx="50" cy="50" r="45" fill="none" stroke="#1E293B" strokeWidth="1" />
              <g transform={`rotate(${-(state?.yaw || 0)}, 50, 50)`}>
                <text x="50" y="12" textAnchor="middle" fill="#EF4444" fontSize="8" fontWeight="bold">N</text>
                <text x="90" y="53" textAnchor="middle" fill="#64748B" fontSize="7">E</text>
                <text x="50" y="96" textAnchor="middle" fill="#64748B" fontSize="7">S</text>
                <text x="10" y="53" textAnchor="middle" fill="#64748B" fontSize="7">W</text>
              </g>
              <polygon points="50,20 46,38 54,38" fill="#00F0FF" />
              <polygon points="50,80 46,62 54,62" fill="#1E293B" />
            </svg>
          </div>
          <div className="text-[9px] text-slate-600 mt-1">HDG {(state?.yaw || 0).toFixed(0)}°</div>
        </div>
      </div>

      {/* Altitude chart */}
      <div className="mt-3">
        <span className="text-[8px] text-slate-700 uppercase">Altitude History</span>
        <div className="h-[40px] mt-1">
          {history && history.length > 2 ? (
            <ResponsiveContainer width="100%" height="100%">
              <LineChart data={history.slice(-60)}>
                <CartesianGrid stroke="#1E293B" strokeDasharray="3 3" />
                <YAxis domain={['auto', 'auto']} hide />
                <Line type="monotone" dataKey="altitude" stroke="#00F0FF" strokeWidth={1.5} dot={false} />
              </LineChart>
            </ResponsiveContainer>
          ) : (
            <div className="h-full bg-black/30 rounded-sm flex items-center justify-center">
              <span className="text-[9px] text-slate-700">Waiting for data...</span>
            </div>
          )}
        </div>
      </div>
    </div>
  );
}

function Row({ label, value, unit }) {
  return (
    <div className="flex justify-between items-center">
      <span className="text-[10px] text-slate-600 uppercase">{label}</span>
      <span className="text-[11px] text-[#00F0FF] font-bold tabular-nums">
        {value}
        {unit && <span className="text-[9px] text-slate-600 ml-0.5">{unit}</span>}
      </span>
    </div>
  );
}
