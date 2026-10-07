import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";
import network from "./config/network.json";
const backend = `http://127.0.0.1:${network.httpPort}`;
export default defineConfig({
  plugins: [react()],
  build: { target: "es2020", sourcemap: false },
  server: {
    port: 5177,
    strictPort: true,
    proxy: {
      "/api": {
        target: backend,
        changeOrigin: true,
        configure(proxy) {
          proxy.on("proxyReq", (outgoing, incoming) => {
            // Translate only a same-origin development request; keep hostile origins intact.
            if (incoming.headers.origin === `http://${incoming.headers.host}`)
              outgoing.setHeader("Origin", backend);
          });
        },
      },
    },
  },
});
