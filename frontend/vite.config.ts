import { defineConfig, loadEnv } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), "");
  const allowedHosts = env.VITE_ALLOWED_HOSTS
    ? env.VITE_ALLOWED_HOSTS.split(",").map((s) => s.trim()).filter(Boolean)
    : undefined;
  const port = parseInt(env.FRONTEND_PORT || "5174", 10);
  const backendPort = parseInt(env.BACKEND_PORT || "3100", 10);

  return {
    plugins: [react()],
    server: {
      port,
      allowedHosts,
      proxy: {
        "/api": `http://localhost:${backendPort}`,
      },
    },
  };
});
