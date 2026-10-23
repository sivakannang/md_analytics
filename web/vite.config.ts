// Dev server: proxies /api to mda-gateway so the browser sees one origin and the
// SameSite=Strict, HttpOnly session cookie works without CORS (ADR-0015).
import react from '@vitejs/plugin-react';
import { defineConfig } from 'vite';

const gateway = process.env.MDA_GATEWAY_URL ?? 'http://127.0.0.1:8090';

export default defineConfig({
  plugins: [react()],
  server: {
    host: '127.0.0.1',
    port: Number(process.env.PORT ?? 5173),
    strictPort: true,
    // ws: the live price stream; xfwd: pass the browser's address (X-Forwarded-For), which
    // the gateway trusts only from a loopback peer like this proxy (ADR-0019).
    proxy: { '/api': { target: gateway, ws: true, xfwd: true } },
  },
});
