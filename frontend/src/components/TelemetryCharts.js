import React from 'react';
import { LineChart, Line, ResponsiveContainer, YAxis, CartesianGrid, XAxis, Tooltip } from 'recharts';

const CustomTooltip = ({ active, payload }) => {
  if (active && payload && payload.length) {
    return (
      <div className="bg-[#0A0C10] border border-[#1E293B] px-2 py-1 text-[10px]">
        {payload.map((p, i) => (
          <div key={i} className="flex gap-2">
            <span style={{ color: p.stroke }}>{p.dataKey}:</span>
            <span className="text-slate-300 tabular-nums">{p.value?.toFixed(4)}</span>
          </div>
        ))}
      </div>
    );
  }
  return null;
};

export default function TelemetryCharts({ history }) {
  const data = (history || []).slice(-100);

  if (data.length < 2) {
    return (
      <div className="panel-glass p-3 flex items-center justify-center h-full" data-testid="telemetry-charts">
        <span className="text-[10px] text-slate-700">Collecting telemetry data...</span>
      </div>
    );
  }

  return (
    <div className="panel-glass p-3 relative corner-bracket" data-testid="telemetry-charts">
      <h3 className="text-[10px] uppercase tracking-widest text-slate-500 mb-2 font-semibold">Telemetry Charts</h3>
      
      <div className="space-y-3">
        {/* Attitude */}
        <div>
          <span className="text-[8px] text-slate-700 uppercase">Attitude (Roll / Pitch)</span>
          <div className="h-[60px]">
            <ResponsiveContainer width="100%" height="100%">
              <LineChart data={data}>
                <CartesianGrid stroke="#1E293B" strokeDasharray="3 3" />
                <YAxis domain={['auto', 'auto']} hide />
                <Tooltip content={<CustomTooltip />} />
                <Line type="monotone" dataKey="roll" stroke="#00F0FF" strokeWidth={1.5} dot={false} />
                <Line type="monotone" dataKey="pitch" stroke="#7dd3fc" strokeWidth={1} dot={false} />
              </LineChart>
            </ResponsiveContainer>
          </div>
        </div>

        {/* IMU Gyro */}
        <div>
          <span className="text-[8px] text-slate-700 uppercase">IMU Gyroscope</span>
          <div className="h-[60px]">
            <ResponsiveContainer width="100%" height="100%">
              <LineChart data={data}>
                <CartesianGrid stroke="#1E293B" strokeDasharray="3 3" />
                <YAxis domain={['auto', 'auto']} hide />
                <Tooltip content={<CustomTooltip />} />
                <Line type="monotone" dataKey="imu_gyro_x" stroke="#EF4444" strokeWidth={1} dot={false} />
                <Line type="monotone" dataKey="imu_gyro_y" stroke="#10B981" strokeWidth={1} dot={false} />
                <Line type="monotone" dataKey="imu_gyro_z" stroke="#00F0FF" strokeWidth={1} dot={false} />
              </LineChart>
            </ResponsiveContainer>
          </div>
        </div>

        {/* Battery + CPU */}
        <div>
          <span className="text-[8px] text-slate-700 uppercase">System (Battery V / CPU %)</span>
          <div className="h-[50px]">
            <ResponsiveContainer width="100%" height="100%">
              <LineChart data={data}>
                <CartesianGrid stroke="#1E293B" strokeDasharray="3 3" />
                <YAxis domain={['auto', 'auto']} hide />
                <Tooltip content={<CustomTooltip />} />
                <Line type="monotone" dataKey="battery_voltage" stroke="#F59E0B" strokeWidth={1.5} dot={false} />
                <Line type="monotone" dataKey="cpu_usage" stroke="#64748B" strokeWidth={1} dot={false} />
              </LineChart>
            </ResponsiveContainer>
          </div>
        </div>
      </div>
    </div>
  );
}
