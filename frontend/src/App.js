import React, { useState, useCallback, useRef } from 'react';
import Header from './components/Header';
import Sidebar from './components/Sidebar';
import SensorPanels from './components/SensorPanels';
import DronePanel from './components/DronePanel';
import EventLog from './components/EventLog';
import CommandPanel from './components/CommandPanel';
import TelemetryCharts from './components/TelemetryCharts';
import { useWebSocket } from './hooks/useApi';

function App() {
  const [state, setState] = useState(null);
  const [threads, setThreads] = useState([]);
  const [engines, setEngines] = useState({});
  const [events, setEvents] = useState([]);
  const historyRef = useRef([]);
  const [history, setHistory] = useState([]);

  const handleMessage = useCallback((data) => {
    if (data.type === 'telemetry') {
      setState(data.state);
      setThreads(data.threads || []);
      setEngines(data.engines || {});
      if (data.recent_events) {
        setEvents(prev => {
          const combined = [...prev, ...data.recent_events];
          const unique = combined.filter((e, i, arr) => 
            i === arr.findIndex(x => x.timestamp === e.timestamp && x.type === e.type && x.message === e.message)
          );
          return unique.slice(-200);
        });
      }
      // Build history for charts
      if (data.state) {
        const s = data.state;
        const record = {
          timestamp: s.uptime_sec,
          roll: s.roll,
          pitch: s.pitch,
          yaw: s.yaw,
          altitude: s.altitude_agl,
          battery_voltage: s.battery_voltage,
          cpu_usage: s.cpu_usage,
          imu_gyro_x: s.imu?.gyro_x,
          imu_gyro_y: s.imu?.gyro_y,
          imu_gyro_z: s.imu?.gyro_z,
          baro_pressure: s.baro?.pressure,
          range_distance: s.rangefinder?.distance,
          flow_x: s.optical_flow?.flow_x,
          flow_y: s.optical_flow?.flow_y,
        };
        historyRef.current = [...historyRef.current.slice(-200), record];
        setHistory([...historyRef.current]);
      }
    }
  }, []);

  const { connected } = useWebSocket('/api/ws/telemetry', handleMessage);

  return (
    <div className="h-screen flex flex-col bg-[#050505] overflow-hidden" data-testid="app-root">
      <Header state={state} connected={connected} />
      
      <div className="flex-1 flex overflow-hidden">
        {/* Sidebar */}
        <div className="w-44 shrink-0">
          <Sidebar state={state} threads={threads} engines={engines} />
        </div>

        {/* Main Content */}
        <main className="flex-1 flex flex-col gap-2 p-2 overflow-y-auto">
          {/* Top: Drone + Sensors */}
          <div className="grid grid-cols-12 gap-2">
            <div className="col-span-5">
              <DronePanel state={state} history={history} />
            </div>
            <div className="col-span-7">
              <SensorPanels state={state} history={history} />
            </div>
          </div>

          {/* Bottom: Charts + Events + Commands */}
          <div className="grid grid-cols-12 gap-2 flex-1" style={{ minHeight: '220px' }}>
            <div className="col-span-5">
              <TelemetryCharts history={history} />
            </div>
            <div className="col-span-4 flex flex-col">
              <EventLog events={events} />
            </div>
            <div className="col-span-3 flex flex-col gap-2">
              <CommandPanel />
              {/* C++ Runtime info */}
              <div className="panel-glass p-3 flex-1 relative corner-bracket" data-testid="runtime-info">
                <h3 className="text-[10px] uppercase tracking-widest text-slate-500 mb-2 font-semibold">Runtime</h3>
                <div className="space-y-1">
                  <InfoRow label="ENGINE" value="JT-Zero v1.0" />
                  <InfoRow label="TARGET" value="Pi Zero 2 W" />
                  <InfoRow label="MODE" value="SIMULATOR" color="text-amber-400" />
                  <InfoRow label="LANG" value="C++17" />
                  <InfoRow label="THREADS" value="8 (5 active)" />
                  <InfoRow label="CORE" value="Lock-free SPSC" />
                  <InfoRow label="REFRESH" value="10 Hz WS" />
                </div>
              </div>
            </div>
          </div>
        </main>
      </div>

      {/* Scanline overlay */}
      <div 
        className="fixed inset-0 pointer-events-none z-50"
        style={{
          background: 'repeating-linear-gradient(0deg, transparent, transparent 1px, rgba(0,0,0,0.03) 1px, rgba(0,0,0,0.03) 2px)',
        }}
      />
    </div>
  );
}

function InfoRow({ label, value, color }) {
  return (
    <div className="flex justify-between items-center">
      <span className="text-[9px] text-slate-600 uppercase">{label}</span>
      <span className={`text-[10px] font-bold tabular-nums ${color || 'text-slate-400'}`}>{value}</span>
    </div>
  );
}

export default App;
