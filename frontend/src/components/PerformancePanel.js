import React from 'react';
import { Cpu, MemoryStick, Gauge, Clock, Zap, TrendingUp } from 'lucide-react';

function ProgressBar({ value, max, color = 'bg-[#00F0FF]', warn = 70, danger = 90 }) {
  const pct = Math.min(100, Math.max(0, (value / max) * 100));
  const barColor = pct > danger ? 'bg-red-500' : pct > warn ? 'bg-amber-400' : color;
  
  return (
    <div className="w-full h-1.5 bg-black/40 rounded-full overflow-hidden">
      <div
        className={`h-full ${barColor} transition-all duration-300`}
        style={{ width: `${pct}%` }}
      />
    </div>
  );
}

export default function PerformancePanel({ performance, runtimeMode }) {
  const mem = performance?.memory || {};
  const latency = performance?.latency || {};
  const throughput = performance?.throughput || {};
  const threads = performance?.threads || [];
  const totalCpu = performance?.total_cpu_percent || 0;

  return (
    <div className="panel-glass p-3 relative corner-bracket" data-testid="performance-panel">
      <div className="flex items-center justify-between mb-2">
        <div className="flex items-center gap-2">
          <Gauge className="w-3.5 h-3.5 text-slate-500" />
          <h3 className="text-[10px] uppercase tracking-widest text-slate-500 font-semibold">Performance</h3>
        </div>
        <div className={`text-[9px] font-bold uppercase px-1.5 py-0.5 rounded-sm ${
          runtimeMode === 'native' 
            ? 'bg-emerald-500/10 border border-emerald-500/30 text-emerald-400' 
            : 'bg-amber-500/10 border border-amber-500/30 text-amber-400'
        }`} data-testid="runtime-mode-badge">
          {runtimeMode === 'native' ? 'C++ NATIVE' : 'PY SIMULATOR'}
        </div>
      </div>

      {/* CPU Usage */}
      <div className="mb-2">
        <div className="flex justify-between items-center mb-0.5">
          <span className="text-[9px] text-slate-600 uppercase">CPU Total</span>
          <span className="text-[10px] text-[#00F0FF] font-bold tabular-nums">
            {totalCpu.toFixed(1)}%
          </span>
        </div>
        <ProgressBar value={totalCpu} max={100} warn={50} danger={65} />
      </div>

      {/* Thread CPU bars */}
      <div className="mb-2">
        <span className="text-[8px] text-slate-700 uppercase">Thread CPU</span>
        <div className="space-y-0.5 mt-0.5">
          {threads.slice(0, 7).map((t, i) => (
            <div key={i} className="flex items-center gap-1.5">
              <span className="text-[8px] text-slate-600 w-10 truncate">{t.name?.replace(/T\d_/, '')}</span>
              <div className="flex-1">
                <ProgressBar value={t.cpu_percent || 0} max={100} />
              </div>
              <span className="text-[8px] text-slate-500 tabular-nums w-10 text-right">
                {(t.cpu_percent || 0).toFixed(1)}%
              </span>
            </div>
          ))}
        </div>
      </div>

      {/* Memory */}
      <div className="mb-2 border-t border-[#1E293B]/30 pt-1">
        <div className="flex justify-between items-center mb-0.5">
          <span className="text-[9px] text-slate-600 uppercase">Memory</span>
          <span className="text-[10px] text-[#00F0FF] font-bold tabular-nums">
            {(mem.total_mb || 0).toFixed(2)} MB
          </span>
        </div>
        <ProgressBar value={mem.total_mb || 0} max={300} warn={200} danger={280} />
        <div className="flex gap-2 mt-1">
          <MiniStat label="Engines" value={`${((mem.engines_bytes || 0) / 1024).toFixed(0)}KB`} />
          <MiniStat label="Events" value={`${((mem.event_queue_bytes || 0) / 1024).toFixed(0)}KB`} />
          <MiniStat label="Camera" value={`${((mem.camera_bytes || 0) / 1024).toFixed(0)}KB`} />
        </div>
      </div>

      {/* Latency & Throughput */}
      <div className="border-t border-[#1E293B]/30 pt-1">
        <div className="grid grid-cols-2 gap-x-3">
          <div>
            <span className="text-[8px] text-slate-700 uppercase">Latency</span>
            <Row label="Reflex avg" value={`${(latency.reflex_avg_us || 0).toFixed(1)}us`} />
            <Row label="Fires" value={latency.reflex_fires || 0} />
          </div>
          <div>
            <span className="text-[8px] text-slate-700 uppercase">Throughput</span>
            <Row label="Events" value={throughput.events_total || 0} />
            <Row label="Dropped" value={throughput.events_dropped || 0}
                 color={(throughput.events_dropped || 0) > 0 ? 'text-red-400' : 'text-emerald-400'} />
            <Row label="Drop Rate" value={`${((throughput.drop_rate || 0) * 100).toFixed(2)}%`}
                 color={(throughput.drop_rate || 0) > 0.01 ? 'text-red-400' : 'text-emerald-400'} />
          </div>
        </div>
      </div>
    </div>
  );
}

function Row({ label, value, color }) {
  return (
    <div className="flex justify-between items-center py-0.5">
      <span className="text-[9px] text-slate-600">{label}</span>
      <span className={`text-[10px] font-bold tabular-nums ${color || 'text-[#00F0FF]'}`}>{value}</span>
    </div>
  );
}

function MiniStat({ label, value }) {
  return (
    <div className="text-[8px]">
      <span className="text-slate-600">{label}: </span>
      <span className="text-slate-400 font-bold">{value}</span>
    </div>
  );
}
