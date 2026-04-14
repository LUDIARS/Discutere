import { useState } from "react";
import { useAuth } from "../contexts/AuthContext";

export function LoginPage() {
  const { loginWithPopup } = useAuth();
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState("");

  const handleLogin = async () => {
    setError("");
    setLoading(true);
    try {
      await loginWithPopup();
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : "Authentication failed";
      if (msg !== "Login popup was closed.") setError(msg);
    } finally {
      setLoading(false);
    }
  };

  return (
    <div
      style={{
        minHeight: "100dvh",
        display: "flex",
        alignItems: "center",
        justifyContent: "center",
        background: "var(--bg, #0d1117)",
        color: "var(--text, #c9d1d9)",
        padding: "1rem",
      }}
    >
      <div style={{ width: "100%", maxWidth: 420, textAlign: "center" }}>
        <h1 style={{ fontSize: "1.75rem", fontWeight: 700, marginBottom: "0.25rem" }}>
          Discutere
        </h1>
        <p style={{ color: "var(--text-muted, #8b949e)", fontSize: "0.9rem", marginBottom: "2rem" }}>
          Chat-to-Task automation
        </p>

        <button
          onClick={handleLogin}
          disabled={loading}
          style={{
            width: "100%",
            padding: "0.75rem 1rem",
            background: "var(--accent, #58a6ff)",
            color: "#000",
            border: "none",
            borderRadius: "0.5rem",
            cursor: loading ? "wait" : "pointer",
            fontWeight: 600,
            fontSize: "0.95rem",
          }}
        >
          {loading ? "ログイン処理中…" : "Cernere でログイン"}
        </button>

        {error && (
          <p style={{ color: "#f85149", marginTop: "1rem", fontSize: "0.85rem" }}>{error}</p>
        )}

        <p style={{ marginTop: "2rem", fontSize: "0.75rem", color: "var(--text-muted, #8b949e)" }}>
          認証は LUDIARS Cernere に委譲されます。
        </p>
      </div>
    </div>
  );
}
