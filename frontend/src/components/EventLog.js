import React, { useRef, useEffect } from 'react';

const TYPE_COLORS = {
  SYS_STARTUP: 'text-emerald-400',
  SYS_SHUTDOWN: 'text-red-400',
  SYS_HEARTBEAT: 'text-slate-600',
  SYS_ERROR: 'text-red-500',
  SYS_WARNING: 'text-amber-400',
  SYSTEM_STARTUP: 'text-emerald-400',
  SYSTEM_SHUTDOWN: 'text-red-400',
  SYSTEM_HEARTBEAT: 'text-slate-600',
  SYSTEM_ERROR: 'text-red-500',
  SYSTEM_WARNING: 'text-amber-400',
  FLIGHT_ARM: 'text-amber-300',
  FLIGHT_DISARM: 'text-slate-400',
  FLIGHT_TAKEOFF: 'text-cyan-400',
  FLIGHT_LAND: 'text-amber-400',
  FLIGHT_RTL: 'text-orange-400',
  FLIGHT_HOLD: 'text-blue-400',
  FLIGHT_ALTITUDE_REACHED: 'text-emerald-400',
  FLIGHT_OBSTACLE_DETECTED: 'text-red-400',
  CMD_USER: 'text-purple-400',
  CMD_API: 'text-purple-300',
  IMU_UPDATE: 'text-slate-600',
  SENSOR_IMU_UPDATE: 'text-slate-600',
  DEFAULT: 'text-green-400',
};

export default function EventLog({ events }) {
  const scrollRef = useRef(null);

  useEffect(() => {
    if (scrollRef.current) {
      scrollRef.current.scrollTop = scrollRef.current.scrollHeight;
    }
  }, [events]);

  const filtered = (events || []).filter(e => 
    e.type !== 'SYSTEM_HEARTBEAT' && e.type !== 'SENSOR_IMU_UPDATE'
  );

  return (
    <div className="panel-glass p-3 flex flex-col h-full relative corner-bracket" data-testid="event-log">
      <div className="flex items-center justify-between mb-2">
        <h3 className="text-[10px] uppercase tracking-widest text-slate-500 font-semibold">Event Log</h3>
        <span className="text-[9px] text-slate-700 tabular-nums">{events?.length || 0} events</span>
      </div>

      <div
        ref={scrollRef}
        className="flex-1 overflow-y-auto font-mono text-[10px] bg-black/40 border border-[#1E293B] p-2 space-y-0.5"
        style={{ minHeight: 0 }}
      >
        {filtered.length === 0 ? (
          <div className="text-slate-700 text-center py-4">Waiting for events...</div>
        ) : (
          filtered.map((ev, i) => {
            const color = TYPE_COLORS[ev.type] || TYPE_COLORS.DEFAULT;
            return (
              <div key={i} className="flex gap-2 leading-tight" data-testid={`event-row-${i}`}>
                <span className="text-slate-700 shrink-0 tabular-nums">
                  [{ev.timestamp?.toFixed(1) || '0.0'}s]
                </span>
                <span className={`shrink-0 font-bold ${color}`}>
                  {ev.type}
                </span>
                <span className="text-slate-500 truncate">
                  {ev.message}
                </span>
              </div>
            );
          })
        )}
      </div>
    </div>
  );
}
