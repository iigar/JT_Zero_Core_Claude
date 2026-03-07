import React from 'react';
import { LineChart, Line, ResponsiveContainer, YAxis } from 'recharts';

function MiniChart({ data, dataKey, color = '#00F0FF', height = 32 }) {
  if (!data || data.length < 2) return <div style={{ height }} className="bg-black/30 rounded-sm" />;
  return (
    <ResponsiveContainer width="100%" height={height}>
      <LineChart data={data}>
        <Line type="monotone" dataKey={dataKey} stroke={color} strokeWidth={1.5} dot={false} />
        <YAxis domain={['auto', 'auto']} hide />
      </LineChart>
    </ResponsiveContainer>
  );
}

function TelemetryRow({ label, value, unit, color }) {
  return (
    <div className="flex justify-between items-center border-b border-[#1E293B]/30 py-0.5 last:border-0">
      <span className="text-[10px] text-slate-600 uppercase">{label}</span>
      <span className={`text-xs font-bold tabular-nums ${color || 'text-[#00F0FF]'}`}>
        {value}
        {unit && <span className="text-[9px] text-slate-600 ml-0.5">{unit}</span>}
      </span>
    </div>
  );
}

export default function SensorPanels({ state, history }) {
  return (
    <div className="grid grid-cols-2 gap-2" data-testid="sensor-panels">
      {/* IMU Panel */}
      <div className="panel-glass p-3 relative corner-bracket" data-testid="imu-panel">
        <h3 className="text-[10px] uppercase tracking-widest text-slate-500 mb-2 font-semibold">IMU</h3>
        <div className="grid grid-cols-2 gap-x-4">
          <div>
            <span className="text-[8px] text-slate-700 uppercase">Gyroscope (rad/s)</span>
            <TelemetryRow label="X" value={state?.imu?.gyro_x?.toFixed(4)} />
            <TelemetryRow label="Y" value={state?.imu?.gyro_y?.toFixed(4)} />
            <TelemetryRow label="Z" value={state?.imu?.gyro_z?.toFixed(4)} />
          </div>
          <div>
            <span className="text-[8px] text-slate-700 uppercase">Accel (m/s2)</span>
            <TelemetryRow label="X" value={state?.imu?.acc_x?.toFixed(3)} />
            <TelemetryRow label="Y" value={state?.imu?.acc_y?.toFixed(3)} />
            <TelemetryRow label="Z" value={state?.imu?.acc_z?.toFixed(3)} />
          </div>
        </div>
        <div className="mt-2">
          <MiniChart data={history} dataKey="imu_gyro_z" color="#7dd3fc" />
        </div>
      </div>

      {/* Barometer Panel */}
      <div className="panel-glass p-3 relative corner-bracket" data-testid="baro-panel">
        <h3 className="text-[10px] uppercase tracking-widest text-slate-500 mb-2 font-semibold">Barometer</h3>
        <TelemetryRow label="PRESS" value={state?.baro?.pressure?.toFixed(2)} unit="hPa" />
        <TelemetryRow label="ALT" value={state?.baro?.altitude?.toFixed(2)} unit="m" />
        <TelemetryRow label="TEMP" value={state?.baro?.temperature?.toFixed(1)} unit="C" />
        <div className="mt-2">
          <MiniChart data={history} dataKey="baro_pressure" color="#10B981" />
        </div>
      </div>

      {/* GPS Panel */}
      <div className="panel-glass p-3 relative corner-bracket" data-testid="gps-panel">
        <h3 className="text-[10px] uppercase tracking-widest text-slate-500 mb-2 font-semibold">GPS</h3>
        <TelemetryRow label="LAT" value={state?.gps?.lat?.toFixed(6)} unit="deg" />
        <TelemetryRow label="LON" value={state?.gps?.lon?.toFixed(6)} unit="deg" />
        <TelemetryRow label="ALT" value={state?.gps?.alt?.toFixed(1)} unit="m" />
        <TelemetryRow label="SPD" value={state?.gps?.speed?.toFixed(1)} unit="m/s" />
        <div className="flex justify-between mt-1">
          <span className="text-[9px] text-slate-600">SAT: <span className="text-emerald-400 font-bold">{state?.gps?.satellites || 0}</span></span>
          <span className="text-[9px] text-slate-600">FIX: <span className="text-emerald-400 font-bold">{state?.gps?.fix_type || 0}D</span></span>
        </div>
      </div>

      {/* Rangefinder + Flow Panel */}
      <div className="panel-glass p-3 relative corner-bracket" data-testid="range-flow-panel">
        <div className="mb-2">
          <h3 className="text-[10px] uppercase tracking-widest text-slate-500 mb-1 font-semibold">Rangefinder</h3>
          <TelemetryRow label="DIST" value={state?.rangefinder?.distance?.toFixed(2)} unit="m" />
          <TelemetryRow label="QUAL" value={`${((state?.rangefinder?.signal_quality || 0) * 100).toFixed(0)}%`} />
        </div>
        <div>
          <h3 className="text-[10px] uppercase tracking-widest text-slate-500 mb-1 font-semibold">Optical Flow</h3>
          <TelemetryRow label="X" value={state?.optical_flow?.flow_x?.toFixed(3)} unit="rad/s" />
          <TelemetryRow label="Y" value={state?.optical_flow?.flow_y?.toFixed(3)} unit="rad/s" />
          <TelemetryRow label="QUAL" value={state?.optical_flow?.quality || 0} />
        </div>
      </div>
    </div>
  );
}
