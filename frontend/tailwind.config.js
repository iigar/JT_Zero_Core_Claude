/** @type {import('tailwindcss').Config} */
module.exports = {
  content: ["./src/**/*.{js,jsx,ts,tsx}"],
  theme: {
    extend: {
      colors: {
        void: '#050505',
        panel: '#0A0C10',
        'panel-border': '#1E293B',
        'grid-line': '#1f2937',
        cyan: {
          400: '#00F0FF',
          300: '#00F0FF',
          500: '#00D4E0',
        }
      },
      fontFamily: {
        mono: ['JetBrains Mono', 'monospace'],
      },
    },
  },
  plugins: [],
}
