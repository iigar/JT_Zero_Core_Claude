import React from 'react';
import { Gauge, Thermometer, MapPin, Radar, Eye, Server } from 'lucide-react';

function SensorStatus({ name, icon: Icon, healthy, data }) {
  return (
    <div
      data-testid={`sensor-status-${name.toLowerCase()}`}
      className="flex items-center gap-2"
    >
      <div className={`w-2 h-2 rounded-full ${
        healthy 
          ? 'bg-emerald-500 shadow-[0_0_6px_rgba(16,185,129,0.5)]' 
          : 'bg-red-500 animate-pulse'
      }`} />
      <Icon className="w-3.5 h-3.5 text-slate-500" />
      <span className="text-[10px] font-bold text-slate-300 uppercase tracking-wider">{name}</span>
    </div>
  );
}

export default function Sidebar({ state, threads, engines }) {
  const sensors = [
    { name: 'IMU', icon: Server, healthy: state?.imu?.valid },
    { name: 'BARO', icon: Thermometer, healthy: state?.baro?.valid },
    { name: 'GPS', icon: MapPin, healthy: state?.gps?.valid && state?.gps?.fix_type >= 2 },
    { name: 'RANGE', icon: Radar, healthy: state?.rangefinder?.valid },
    { name: 'FLOW', icon: Eye, healthy: state?.optical_flow?.valid },
  ];

  return (
    <aside
      data-testid="sidebar"
      className="flex flex-col gap-3 p-3 bg-[#0A0C10] border-r border-[#1E293B] overflow-y-auto"
    >
      {/* System */}
      <div>
        <h3 className="text-[10px] uppercase tracking-widest text-slate-600 mb-2 font-semibold">System</h3>
        <div className="space-y-1.5">
          <DataRow label="CPU" value={`${state?.cpu_usage?.toFixed(1) || 0}%`} />
          <DataRow label="RAM" value={`${state?.ram_usage_mb?.toFixed(0) || 0}MB`} />
          <DataRow label="TEMP" value={`${state?.cpu_temp?.toFixed(1) || 0}C`} />
          <DataRow label="EVENTS" value={state?.event_count || 0} />
        </div>
      </div>

      <div className="border-t border-[#1E293B]" />

      {/* Sensors */}
      <div>
        <h3 className="text-[10px] uppercase tracking-widest text-slate-600 mb-2 font-semibold">Sensors</h3>
        <div className="space-y-2">
          {sensors.map(s => (
            <SensorStatus key={s.name} {...s} />
          ))}
        </div>
      </div>

      <div className="border-t border-[#1E293B]" />

      {/* Threads */}
      <div>
        <h3 className="text-[10px] uppercase tracking-widest text-slate-600 mb-2 font-semibold">Threads</h3>
        <div className="space-y-1">
          {threads?.slice(0, 5).map((t, i) => (
            <div key={i} className="flex items-center gap-1.5">
              <div className={`w-1.5 h-1.5 rounded-full ${
                t.running ? 'bg-emerald-500' : 'bg-slate-700'
              }`} />
              <span className="text-[9px] text-slate-500 w-12 truncate">{t.name?.replace('T'+i+'_', '')}</span>
              <span className="text-[9px] text-cyan-400 tabular-nums ml-auto">{t.actual_hz?.toFixed(0)}Hz</span>
            </div>
          ))}
        </div>
      </div>

      <div className="border-t border-[#1E293B]" />

      {/* Engines */}
      <div>
        <h3 className="text-[10px] uppercase tracking-widest text-slate-600 mb-2 font-semibold">Engines</h3>
        <div className="space-y-1.5">
          <DataRow label="EVT.Q" value={engines?.events?.pending || 0} />
          <DataRow label="REFLEX" value={`${engines?.reflexes?.total_fires || 0} fires`} />
          <DataRow label="RULES" value={`${engines?.rules?.total_evaluations || 0} eval`} />
          <DataRow label="MEM" value={`${((engines?.memory?.usage_bytes || 0) / 1024).toFixed(0)}KB`} />
        </div>
      </div>
    </aside>
  );
}

function DataRow({ label, value }) {
  return (
    <div className="flex justify-between items-center">
      <span className="text-[10px] text-slate-600 uppercase">{label}</span>
      <span className="text-[11px] text-[#00F0FF] font-bold tabular-nums">{value}</span>
    </div>
  );
}
