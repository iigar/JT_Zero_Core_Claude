import React, { useState } from 'react';
import { apiCall } from '../hooks/useApi';
import { Play, Square, ArrowUp, ArrowDown, RotateCcw, Shield, AlertTriangle, Crosshair } from 'lucide-react';

const COMMANDS = [
  { cmd: 'arm', label: 'ARM', icon: Shield, color: 'border-amber-500/30 hover:bg-amber-500/10 text-amber-400' },
  { cmd: 'disarm', label: 'DISARM', icon: Square, color: 'border-slate-500/30 hover:bg-slate-500/10 text-slate-400' },
  { cmd: 'takeoff', label: 'TAKEOFF', icon: ArrowUp, color: 'border-cyan-500/30 hover:bg-cyan-500/10 text-cyan-400', param: true },
  { cmd: 'land', label: 'LAND', icon: ArrowDown, color: 'border-emerald-500/30 hover:bg-emerald-500/10 text-emerald-400' },
  { cmd: 'hold', label: 'HOLD', icon: Crosshair, color: 'border-blue-500/30 hover:bg-blue-500/10 text-blue-400' },
  { cmd: 'rtl', label: 'RTL', icon: RotateCcw, color: 'border-orange-500/30 hover:bg-orange-500/10 text-orange-400' },
  { cmd: 'emergency', label: 'E-STOP', icon: AlertTriangle, color: 'border-red-500/30 hover:bg-red-500/10 text-red-500' },
];

export default function CommandPanel() {
  const [lastResult, setLastResult] = useState(null);
  const [sending, setSending] = useState(false);

  async function handleCommand(cmd, param1 = 0) {
    setSending(true);
    try {
      const res = await apiCall('POST', '/api/command', {
        command: cmd,
        param1: param1,
        param2: 0,
      });
      setLastResult(res);
    } catch (e) {
      setLastResult({ success: false, message: e.message });
    }
    setSending(false);
  }

  return (
    <div className="panel-glass p-3 relative corner-bracket" data-testid="command-panel">
      <h3 className="text-[10px] uppercase tracking-widest text-slate-500 mb-2 font-semibold">Commands</h3>
      
      <div className="grid grid-cols-4 gap-1.5">
        {COMMANDS.map(({ cmd, label, icon: Icon, color, param }) => (
          <button
            key={cmd}
            data-testid={`cmd-${cmd}`}
            onClick={() => handleCommand(cmd, param ? 10 : 0)}
            disabled={sending}
            className={`flex flex-col items-center gap-0.5 p-2 border rounded-sm transition-all duration-200 ${color} ${
              sending ? 'opacity-50' : ''
            }`}
          >
            <Icon className="w-3.5 h-3.5" />
            <span className="text-[8px] font-bold uppercase tracking-wider">{label}</span>
          </button>
        ))}
      </div>

      {lastResult && (
        <div className={`mt-2 text-[9px] px-2 py-1 border rounded-sm ${
          lastResult.success 
            ? 'border-emerald-500/30 text-emerald-400 bg-emerald-500/5' 
            : 'border-red-500/30 text-red-400 bg-red-500/5'
        }`} data-testid="command-result">
          {lastResult.message}
        </div>
      )}
    </div>
  );
}
