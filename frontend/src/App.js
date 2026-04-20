import React, { useState, useCallback, useRef, useEffect } from 'react';
import Header from './components/Header';
import SensorPanels from './components/SensorPanels';
import DronePanel from './components/DronePanel';
import Drone3DPanel from './components/Drone3DPanel';
import EventLog from './components/EventLog';
import CommandPanel from './components/CommandPanel';
import TelemetryCharts from './components/TelemetryCharts';
import CameraPanel from './components/CameraPanel';
import ThermalPanel from './components/ThermalPanel';
import MAVLinkDiagPanel from './components/MAVLinkDiagPanel';
import MAVLinkPanel from './components/MAVLinkPanel';
import FlightLogPanel from './components/FlightLogPanel';
import PerformancePanel from './components/PerformancePanel';
import SimulatorPanel from './components/SimulatorPanel';
import DocumentationTab from './components/DocumentationTab';
import SettingsTab from './components/SettingsTab';
import { useWebSocket } from './hooks/useApi';
import { LayoutDashboard, LineChart, Camera, Radio, ScrollText, FileText, Settings } from 'lucide-react';

const TABS = [
  { id: 'dashboard', label: 'Dashboard', icon: LayoutDashboard },
  { id: 'telemetry', label: 'Telemetry', icon: LineChart },
  { id: 'camera', label: 'Camera / VO', icon: Camera },
  { id: 'mavlink', label: 'MAVLink', icon: Radio },
  { id: 'events', label: 'Events', icon: ScrollText },
  { id: 'docs', label: 'Docs', icon: FileText },
  { id: 'settings', label: 'Settings', icon: Settings },
];

function App() {
  const [activeTab, setActiveTab] = useState('dashboard');
  const [state, setState] = useState(null);
  const [threads, setThreads] = useState([]);
  const [engines, setEngines] = useState({});
  const [events, setEvents] = useState([]);
  const [camera, setCamera] = useState(null);
  const [mavlink, setMavlink] = useState(null);
  const [performance, setPerformance] = useState(null);
  const [features, setFeatures] = useState([]);
  const [runtimeMode, setRuntimeMode] = useState('simulator');
  const [sensorModes, setSensorModes] = useState({});
  const [systemMetrics, setSystemMetrics] = useState(null);
  const [cameras, setCameras] = useState([]);
  const historyRef = useRef([]);
  const [history, setHistory] = useState([]);
  const [voTrail, setVoTrail] = useState([]);

  // Throttled update: accumulate data in refs, flush to state at ~5Hz
  const pendingRef = useRef(null);
  const eventsAccRef = useRef([]);
  const flushTimerRef = useRef(null);
  const FLUSH_INTERVAL = 200; // ms — 5Hz UI updates instead of 10Hz

  const flushPending = useCallback(() => {
    const d = pendingRef.current;
    if (!d) return;
    pendingRef.current = null;

    setState(d.state);
    setThreads(d.threads || []);
    setEngines(d.engines || {});
    if (d.camera) setCamera(d.camera);
    if (d.mavlink) setMavlink(d.mavlink);
    if (d.performance) setPerformance(d.performance);
    if (d.features) setFeatures(d.features);
    if (d.runtime_mode) setRuntimeMode(d.runtime_mode);
    if (d.sensor_modes) setSensorModes(d.sensor_modes);
    if (d.system_metrics) setSystemMetrics(d.system_metrics);
    if (d.cameras) setCameras(d.cameras);

    // Flush accumulated events
    if (eventsAccRef.current.length > 0) {
      const newEvents = eventsAccRef.current;
      eventsAccRef.current = [];
      setEvents(prev => {
        const combined = [...prev, ...newEvents];
        const unique = combined.filter((e, i, arr) =>
          i === arr.findIndex(x => x.timestamp === e.timestamp && x.type === e.type && x.message === e.message)
        );
        return unique.slice(-500);
      });
    }

    // Flush history (single copy, not per-message)
    setHistory([...historyRef.current]);
  }, []);

  const handleMessage = useCallback((data) => {
    if (data.type === 'telemetry') {
      // Store latest payload (overwrite previous if not yet flushed)
      pendingRef.current = data;

      // Accumulate events (don't lose between flushes)
      if (data.recent_events) {
        eventsAccRef.current = [...eventsAccRef.current, ...data.recent_events];
      }

      // Accumulate history in ref (no React re-render)
      if (data.state) {
        const s = data.state;
        const record = {
          timestamp: s.uptime_sec,
          roll: s.roll, pitch: s.pitch, yaw: s.yaw,
          altitude: s.altitude_agl,
          battery_voltage: s.battery_voltage,
          cpu_usage: s.cpu_usage,
          imu_gyro_x: s.imu?.gyro_x, imu_gyro_y: s.imu?.gyro_y, imu_gyro_z: s.imu?.gyro_z,
          imu_acc_x: s.imu?.acc_x, imu_acc_y: s.imu?.acc_y, imu_acc_z: s.imu?.acc_z,
          baro_pressure: s.baro?.pressure,
          baro_temp: s.baro?.temperature,
          range_distance: s.rangefinder?.distance,
          flow_x: s.optical_flow?.flow_x, flow_y: s.optical_flow?.flow_y,
        };
        historyRef.current = [...historyRef.current.slice(-200), record];
      }
    }
  }, []);

  // Throttle: flush to React state at fixed interval
  useEffect(() => {
    flushTimerRef.current = setInterval(flushPending, FLUSH_INTERVAL);
    return () => clearInterval(flushTimerRef.current);
  }, [flushPending]);

  const { connected } = useWebSocket('/api/ws/telemetry', handleMessage);

  // Periodic fetch of VO trail for 3D visualization (every 2s)
  useEffect(() => {
    const fetchTrail = async () => {
      try {
        const res = await fetch((process.env.REACT_APP_BACKEND_URL || '') + '/api/vo/trail');
        if (res.ok) {
          const data = await res.json();
          setVoTrail(data);
        }
      } catch {}
    };
    fetchTrail();
    const interval = setInterval(fetchTrail, 2000);
    return () => clearInterval(interval);
  }, []);

  return (
    <div className="h-screen flex flex-col bg-[#050505] overflow-hidden hud-lines sweep-overlay" data-testid="app-root">
      <Header state={state} connected={connected} runtimeMode={runtimeMode} />

      {/* Tab Navigation */}
      <nav className="flex items-center gap-0 bg-[#0A0C10] border-b border-[#1E293B] px-1 sm:px-2 shrink-0 overflow-x-auto" data-testid="tab-nav">
        {TABS.map(({ id, label, icon: Icon }) => (
          <button
            key={id}
            data-testid={`tab-${id}`}
            onClick={() => setActiveTab(id)}
            className={`flex items-center gap-1 sm:gap-1.5 px-2.5 sm:px-3 py-2.5 sm:py-2 text-[10px] font-semibold uppercase tracking-wider transition-all border-b-2 shrink-0 ${
              activeTab === id
                ? 'text-[#00F0FF] border-[#00F0FF] bg-[#00F0FF]/5 tab-active'
                : 'text-slate-500 border-transparent hover:text-slate-300 hover:border-slate-600'
            }`}
            style={activeTab === id ? { textShadow: '0 0 8px rgba(0,240,255,0.5)', boxShadow: '0 2px 0 -1px rgba(0,240,255,0.25)' } : {}}
          >
            <Icon className="w-4 h-4 sm:w-3.5 sm:h-3.5" />
            <span className="hidden sm:inline">{label}</span>
          </button>
        ))}

        {/* Status pills right-aligned — hidden on mobile */}
        <div className="ml-auto hidden sm:flex items-center gap-2 pr-2">
          <StatusPill label="THREADS" value={`${threads?.filter(t => t.running).length || 0}/8`} ok={threads?.filter(t => t.running).length >= 7} />
          <StatusPill label="MODE" value={runtimeMode === 'native' ? 'C++' : 'PY'} ok={runtimeMode === 'native'} />
          <StatusPill label="EVT" value={events.length} ok />
        </div>
      </nav>

      {/* Tab Content */}
      <main className="flex-1 min-h-0 overflow-y-auto sm:overflow-hidden">
        {activeTab === 'dashboard' && (
          <DashboardTab state={state} history={history} threads={threads} engines={engines} camera={camera} mavlink={mavlink} performance={performance} systemMetrics={systemMetrics} runtimeMode={runtimeMode} events={events} features={features} sensorModes={sensorModes} cameras={cameras} voTrail={voTrail} />
        )}
        {activeTab === 'telemetry' && (
          <TelemetryTab state={state} history={history} performance={performance} systemMetrics={systemMetrics} runtimeMode={runtimeMode} threads={threads} sensorModes={sensorModes} />
        )}
        {activeTab === 'camera' && (
          <CameraTab camera={camera} features={features} cameras={cameras} />
        )}
        {activeTab === 'mavlink' && (
          <MavlinkTab mavlink={mavlink} />
        )}
        {activeTab === 'events' && (
          <div className="h-full p-3">
            <EventLog events={events} fullPage />
          </div>
        )}
        {activeTab === 'docs' && (
          <DocumentationTab />
        )}
        {activeTab === 'settings' && (
          <SettingsTab state={state} threads={threads} engines={engines} runtimeMode={runtimeMode} mavlink={mavlink} sensorModes={sensorModes} camera={camera} />
        )}
      </main>

      {/* Scanline overlay */}
      <div className="fixed inset-0 pointer-events-none z-50" style={{
        background: 'repeating-linear-gradient(0deg, transparent, transparent 1px, rgba(0,0,0,0.03) 1px, rgba(0,0,0,0.03) 2px)',
      }} />
    </div>
  );
}

/* ═══════════════════════════════════════════════════════════ */
/* Dashboard Tab                                              */
/* ═══════════════════════════════════════════════════════════ */

function DashboardTab({ state, history, threads, engines, camera, mavlink, performance, systemMetrics, runtimeMode, events, features, sensorModes, cameras, voTrail }) {
  const secondaryCam = cameras?.find(c => c.slot === 'SECONDARY');
  return (
    <div className="sm:h-full flex flex-col sm:flex-row sm:overflow-hidden">
      {/* Compact sidebar — hidden on mobile */}
      <aside className="hidden sm:flex flex-col w-36 shrink-0 bg-[#0A0C10] border-r border-[#1E293B] p-2 overflow-y-auto">
        <Section title="System">
          <DataRow label="CPU" value={`${systemMetrics?.cpu?.total_percent ?? state?.cpu_usage?.toFixed(1) ?? 0}%`} />
          <DataRow label="RAM" value={`${systemMetrics?.memory?.used_mb ?? state?.ram_usage_mb?.toFixed(0) ?? 0}MB`} />
          <DataRow label="TEMP" value={`${systemMetrics?.temperature ?? state?.cpu_temp?.toFixed(1) ?? 0}C`} />
        </Section>
        <Section title="Sensors">
          {['IMU', 'BARO', 'GPS', 'RANGE', 'FLOW'].map(s => {
            const valid = s === 'IMU' ? state?.imu?.valid :
              s === 'BARO' ? state?.baro?.valid :
              s === 'GPS' ? state?.gps?.valid :
              s === 'RANGE' ? state?.rangefinder?.valid :
              state?.optical_flow?.valid;
            return (
              <div key={s} className="flex items-center gap-1.5">
                <div className={`w-1.5 h-1.5 rounded-full ${valid ? 'bg-emerald-500' : 'bg-red-500'}`} />
                <span className="text-[9px] text-slate-400 uppercase">{s}</span>
              </div>
            );
          })}
        </Section>
        <Section title="Cameras">
          {cameras?.map((cam, i) => (
            <div key={i} className="flex items-center gap-1.5">
              <div className={`w-1.5 h-1.5 rounded-full ${cam.camera_open ? 'bg-emerald-500' : 'bg-slate-700'}`} />
              <span className="text-[8px] text-slate-400 truncate">{cam.label || cam.slot}</span>
            </div>
          )) || (
            <div className="flex items-center gap-1.5">
              <div className="w-1.5 h-1.5 rounded-full bg-emerald-500" />
              <span className="text-[8px] text-slate-400">Primary</span>
            </div>
          )}
          {/* VO Source indicator */}
          <div className="flex items-center gap-1.5 mt-1 pt-1 border-t border-slate-800">
            <div className={`w-1.5 h-1.5 rounded-full ${camera?.vo_source === 'THERMAL_FALLBACK' ? 'bg-amber-500 animate-pulse' : 'bg-cyan-500'}`} />
            <span className="text-[8px] text-slate-400">VO:</span>
            <span className={`text-[8px] font-bold ${camera?.vo_source === 'THERMAL_FALLBACK' ? 'text-amber-400' : 'text-cyan-400'}`} data-testid="sidebar-vo-source">
              {camera?.vo_source === 'THERMAL_FALLBACK' ? 'THERMAL' : 'CSI'}
            </span>
          </div>
        </Section>
        <Section title="Threads">
          {threads?.map((t, i) => (
            <div key={i} className="flex items-center gap-1">
              <div className={`w-1.5 h-1.5 rounded-full ${t.running ? 'bg-emerald-500' : 'bg-slate-700'}`} />
              <span className="text-[8px] text-slate-500 truncate flex-1">{t.name?.split('_')[1] || t.name}</span>
              <span className="text-[8px] text-cyan-400 tabular-nums">{t.actual_hz?.toFixed(0)}</span>
            </div>
          ))}
        </Section>
        <Section title="Engines">
          <DataRow label="EVT" value={engines?.events?.pending || 0} />
          <DataRow label="REFLEX" value={`${engines?.reflexes?.total_fires || 0}`} />
          <DataRow label="RULES" value={`${engines?.rules?.total_evaluations || 0}`} />
          <DataRow label="MEM" value={`${((engines?.memory?.usage_bytes || 0) / 1024).toFixed(0)}K`} />
        </Section>
      </aside>

      {/* Main grid */}
      <div className="flex-1 flex flex-col gap-2 p-2 overflow-y-auto">
        {/* Row 1: 3D + Telemetry + Sensors */}
        <div className="grid grid-cols-1 sm:grid-cols-12 gap-2 shrink-0 sm:h-[240px]">
          <div className="sm:col-span-3 h-48 sm:h-auto overflow-hidden"><Drone3DPanel state={state} voTrail={voTrail} /></div>
          <div className="sm:col-span-3 h-48 sm:h-auto overflow-hidden"><DronePanel state={state} history={history} /></div>
          <div className="sm:col-span-6 h-48 sm:h-auto overflow-hidden"><SensorPanels state={state} history={history} sensorModes={sensorModes} /></div>
        </div>
        {/* Row 2: Camera + MAVLink + Performance */}
        <div className="grid grid-cols-1 sm:grid-cols-12 gap-2 shrink-0 sm:h-[220px]">
          <div className="sm:col-span-4 h-48 sm:h-auto overflow-hidden"><CameraPanel camera={camera} features={features} /></div>
          <div className="sm:col-span-4 h-48 sm:h-auto overflow-hidden"><MAVLinkPanel mavlink={mavlink} /></div>
          <div className="sm:col-span-4 h-48 sm:h-auto overflow-hidden"><PerformancePanel performance={performance} systemMetrics={systemMetrics} runtimeMode={runtimeMode} /></div>
        </div>
        {/* Row 3: Mini event log */}
        <div className="shrink-0 overflow-hidden h-[150px]">
          <EventLog events={events} />
        </div>
      </div>
    </div>
  );
}

/* ═══════════════════════════════════════════════════════════ */
/* Camera Tab (multi-camera: Primary VO + Secondary Thermal)  */
/* ═══════════════════════════════════════════════════════════ */

function CameraTab({ camera, features, cameras }) {
  const primaryCam = cameras?.find(c => c.slot === 'PRIMARY');
  const secondaryCam = cameras?.find(c => c.slot === 'SECONDARY');
  const hasThermal = !!secondaryCam;
  const [activeView, setActiveView] = useState('split');

  const primaryLabel = primaryCam?.label || 'Camera (VO)';
  const csiSensor = primaryCam?.csi_sensor;
  const isCsi = primaryCam?.camera_type === 'PI_CSI';
  const isUsbFallback = primaryCam?.camera_type === 'USB' && !isCsi;

  // VO Fallback state from camera telemetry
  const voSource = camera?.vo_source || 'CSI_PRIMARY';
  const isVOFallback = voSource === 'THERMAL_FALLBACK';
  const fallbackReason = camera?.vo_fallback_reason || '';
  const fallbackDuration = camera?.vo_fallback_duration || 0;
  const fallbackSwitches = camera?.vo_fallback_switches || 0;

  return (
    <div className="h-full flex flex-col p-3 gap-2">
      {/* VO Fallback Alert */}
      {isVOFallback && (
        <div className="shrink-0 flex items-center gap-2 px-2 py-1.5 rounded-sm bg-amber-500/10 border border-amber-500/30" data-testid="vo-fallback-alert">
          <span className="text-amber-400 text-[10px] font-bold animate-pulse">VO FALLBACK</span>
          <span className="text-amber-300/70 text-[8px]">
            Thermal camera active | {fallbackReason} | {fallbackDuration.toFixed(1)}s
          </span>
          {fallbackSwitches > 1 && (
            <span className="text-amber-500/50 text-[8px] ml-auto">switches: {fallbackSwitches}</span>
          )}
        </div>
      )}

      {/* Camera tab header with view selector */}
      <div className="flex items-center gap-2 shrink-0">
        <span className="text-[10px] font-bold text-slate-400 uppercase tracking-wider">Multi-Camera</span>
        <span className="text-[8px] px-1.5 py-0.5 rounded-sm bg-cyan-500/10 text-cyan-400 border border-cyan-500/20 font-bold">
          {cameras?.filter(c => c.camera_open).length || 1} / {cameras?.length || 1} Active
        </span>
        {/* VO Source badge */}
        <span className={`text-[8px] px-1.5 py-0.5 rounded-sm font-bold border ${
          isVOFallback
            ? 'bg-amber-500/10 text-amber-400 border-amber-500/20'
            : 'bg-emerald-500/10 text-emerald-400 border-emerald-500/20'
        }`} data-testid="vo-source-badge">
          VO: {isVOFallback ? 'THERMAL' : 'CSI'}
        </span>
        {isCsi && csiSensor && !isVOFallback && (
          <span className="text-[8px] px-1.5 py-0.5 rounded-sm bg-emerald-500/10 text-emerald-400 border border-emerald-500/20 font-bold">
            CSI: {csiSensor}
          </span>
        )}
        {isUsbFallback && (
          <span className="text-[8px] px-1.5 py-0.5 rounded-sm bg-yellow-500/10 text-yellow-400 border border-yellow-500/20 font-bold">
            USB Fallback
          </span>
        )}
        <div className="ml-auto flex items-center gap-1">
          {['split', 'primary', 'thermal'].map(view => (
            <button
              key={view}
              onClick={() => setActiveView(view)}
              className={`text-[8px] font-bold uppercase px-2 py-1 rounded-sm border transition-colors ${
                activeView === view
                  ? 'bg-cyan-500/15 text-cyan-400 border-cyan-500/30'
                  : 'text-slate-500 border-slate-700/30 hover:text-slate-400'
              }`}
              data-testid={`camera-view-${view}`}
            >
              {view === 'split' ? 'Split' : view === 'primary' ? 'VO Only' : 'Thermal'}
            </button>
          ))}
        </div>
      </div>

      {/* Camera views */}
      <div className="flex-1 min-h-0">
        {activeView === 'split' ? (
          <div className="grid grid-cols-1 sm:grid-cols-2 gap-2 h-full">
            <CameraPanel camera={camera} features={isVOFallback ? [] : features} />
            {hasThermal ? (
              <ThermalPanel secondary={secondaryCam} features={isVOFallback ? features : []} camera={isVOFallback ? camera : null} isVOActive={isVOFallback} />
            ) : (
              <div className="h-full flex items-center justify-center bg-[#080A0E] border border-[#1E293B] rounded-sm">
                <div className="text-center">
                  <p className="text-[10px] text-slate-500">No USB camera for secondary slot</p>
                  <p className="text-[8px] text-slate-600 mt-1">Connect USB thermal camera</p>
                </div>
              </div>
            )}
          </div>
        ) : activeView === 'primary' ? (
          <CameraPanel camera={camera} features={isVOFallback ? [] : features} />
        ) : (
          hasThermal ? (
            <ThermalPanel secondary={secondaryCam} features={isVOFallback ? features : []} camera={isVOFallback ? camera : null} isVOActive={isVOFallback} />
          ) : (
            <div className="h-full flex items-center justify-center bg-[#080A0E] border border-[#1E293B] rounded-sm">
              <p className="text-[10px] text-slate-500">No USB camera for secondary slot</p>
            </div>
          )
        )}
      </div>
    </div>
  );
}

/* ═══════════════════════════════════════════════════════════ */
/* Telemetry Tab                                              */
/* ═══════════════════════════════════════════════════════════ */

function TelemetryTab({ state, history, performance, systemMetrics, runtimeMode, threads, sensorModes }) {
  return (
    <div className="flex flex-col gap-2 p-3 overflow-y-auto">
      <div className="grid grid-cols-1 sm:grid-cols-12 gap-2 shrink-0 sm:h-[300px]">
        <div className="sm:col-span-8 h-64 sm:h-auto overflow-hidden">
          <TelemetryCharts history={history} />
        </div>
        <div className="sm:col-span-4 h-48 sm:h-auto overflow-hidden">
          <PerformancePanel performance={performance} systemMetrics={systemMetrics} runtimeMode={runtimeMode} />
        </div>
      </div>
      {/* Sensor detail grid */}
      <div className="grid grid-cols-1 sm:grid-cols-12 gap-2 shrink-0 sm:h-[220px]">
        <div className="sm:col-span-12 h-48 sm:h-auto overflow-hidden">
          <SensorPanels state={state} history={history} sensorModes={sensorModes} />
        </div>
      </div>
    </div>
  );
}

/* ═══════════════════════════════════════════════════════════ */
/* MAVLink Tab                                                */
/* ═══════════════════════════════════════════════════════════ */

function MavlinkTab({ mavlink }) {
  return (
    <div className="flex flex-col gap-2 p-3 overflow-y-auto">
      <div className="grid grid-cols-1 sm:grid-cols-12 gap-2" style={{ minHeight: '280px' }}>
        <div className="sm:col-span-5"><MAVLinkDiagPanel mavlink={mavlink} /></div>
        <div className="sm:col-span-4"><CommandPanel /></div>
        <div className="sm:col-span-3"><FlightLogPanel /></div>
      </div>
    </div>
  );
}

/* ═══════════════════════════════════════════════════════════ */
/* Shared Mini-Components                                     */
/* ═══════════════════════════════════════════════════════════ */

function Section({ title, children }) {
  return (
    <div className="mb-3">
      <h4 className="text-[8px] uppercase tracking-widest text-slate-600 mb-1.5 font-semibold">{title}</h4>
      <div className="space-y-1">{children}</div>
    </div>
  );
}

function DataRow({ label, value }) {
  return (
    <div className="flex justify-between items-center">
      <span className="text-[9px] text-slate-600 uppercase">{label}</span>
      <span className="text-[9px] text-[#00F0FF] font-bold tabular-nums">{value}</span>
    </div>
  );
}

function StatusPill({ label, value, ok }) {
  return (
    <div className={`flex items-center gap-1 px-2 py-0.5 rounded-sm text-[8px] font-bold uppercase tracking-wider border ${
      ok ? 'border-emerald-500/20 text-emerald-400 bg-emerald-500/5' : 'border-amber-500/20 text-amber-400 bg-amber-500/5'
    }`}>
      <span className="text-slate-500">{label}</span>
      <span>{value}</span>
    </div>
  );
}

export default App;
