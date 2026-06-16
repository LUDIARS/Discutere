/**
 * Slack Socket Mode 接続 (公開 URL 不要・常時 WebSocket)。
 *
 * apps.connections.open で wss URL を取得し WebSocket を張る。受信した
 * events_api / interactive は **即 ack** (envelope_id を返す) してから
 * ハンドラを呼ぶ (Slack は ack が 3 秒以内でないと再送する)。
 *
 * WS の close/error 時は数秒バックオフで再接続する (stop() まで無限)。
 * 依存は ws パッケージと web-api.ts のみ (@slack/* SDK は使わない)。
 */

import WebSocket from "ws";
import { appsConnectionsOpen } from "./web-api.js";

/** Socket Mode から渡ってくる受信メッセージ (最小型)。 */
interface SocketEnvelope {
  type?: string;
  envelope_id?: string;
  payload?: unknown;
}

export interface SocketModeDeps {
  /** app-level token (xapp-)。 */
  appToken: string;
  /** events_api の payload.event を受ける (message 等)。 */
  onEvent: (event: Record<string, unknown>) => void | Promise<void>;
  /** interactive (block_actions 等) の payload を受ける。 */
  onInteractive: (payload: Record<string, unknown>) => void | Promise<void>;
  log?: (msg: string) => void;
  warn?: (msg: string) => void;
}

/** 再接続のバックオフ (ms)。close/error が続くほど伸ばす (上限あり)。 */
const RECONNECT_BASE_MS = 2_000;
const RECONNECT_MAX_MS = 30_000;

/**
 * Socket Mode 接続を開始する。stop() で再接続を止めて close する。
 */
export function startSocketMode(deps: SocketModeDeps): { stop(): void } {
  const log = deps.log ?? ((m) => console.log(`  [slack-socket] ${m}`));
  const warn = deps.warn ?? ((m) => console.warn(`  [slack-socket/warn] ${m}`));

  let ws: WebSocket | null = null;
  let stopped = false;
  let attempt = 0;
  let reconnectTimer: NodeJS.Timeout | null = null;

  /** 受信 envelope に envelope_id があれば即 ack を返す (最優先)。 */
  function ack(envelope: SocketEnvelope): void {
    if (!envelope.envelope_id || !ws || ws.readyState !== WebSocket.OPEN) return;
    try {
      ws.send(JSON.stringify({ envelope_id: envelope.envelope_id }));
    } catch (err) {
      warn(`ack 送信失敗: ${(err as Error).message}`);
    }
  }

  /** payload が文字列なら JSON.parse、object ならそのまま (安全に Record 化)。 */
  function asPayload(raw: unknown): Record<string, unknown> | null {
    if (raw && typeof raw === "object") return raw as Record<string, unknown>;
    if (typeof raw === "string") {
      try {
        const parsed = JSON.parse(raw);
        return parsed && typeof parsed === "object" ? (parsed as Record<string, unknown>) : null;
      } catch {
        return null;
      }
    }
    return null;
  }

  function handleMessage(data: WebSocket.RawData): void {
    let envelope: SocketEnvelope;
    try {
      envelope = JSON.parse(data.toString()) as SocketEnvelope;
    } catch {
      return; // 非 JSON は無視
    }

    switch (envelope.type) {
      case "hello":
        log("hello (接続確立)");
        return;
      case "disconnect":
        log("disconnect 受信 → 再接続");
        // 現 WS を閉じる。close ハンドラが再接続をスケジュールする。
        try {
          ws?.close();
        } catch {
          /* ignore */
        }
        return;
      case "events_api": {
        ack(envelope); // ack 最優先
        const payload = asPayload(envelope.payload);
        const event = payload?.event;
        if (event && typeof event === "object") {
          void Promise.resolve(deps.onEvent(event as Record<string, unknown>)).catch((err) =>
            warn(`onEvent 例外: ${(err as Error).message}`)
          );
        }
        return;
      }
      case "interactive": {
        ack(envelope);
        const payload = asPayload(envelope.payload);
        if (payload) {
          void Promise.resolve(deps.onInteractive(payload)).catch((err) =>
            warn(`onInteractive 例外: ${(err as Error).message}`)
          );
        }
        return;
      }
      case "slash_commands":
        ack(envelope); // 今回は未使用 (ack のみ)
        return;
      default:
        return;
    }
  }

  function scheduleReconnect(): void {
    if (stopped) return;
    if (reconnectTimer) return; // 二重スケジュール防止
    const delay = Math.min(RECONNECT_BASE_MS * 2 ** attempt, RECONNECT_MAX_MS);
    attempt += 1;
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      void connect();
    }, delay);
  }

  async function connect(): Promise<void> {
    if (stopped) return;
    let url: string;
    try {
      url = await appsConnectionsOpen(deps.appToken);
    } catch (err) {
      warn(`接続 URL 取得失敗: ${(err as Error).message} (再試行)`);
      scheduleReconnect();
      return;
    }
    if (stopped) return;

    const sock = new WebSocket(url);
    ws = sock;

    sock.on("open", () => {
      attempt = 0; // 成功でバックオフをリセット
      log("WebSocket 接続");
    });
    sock.on("message", (data) => handleMessage(data));
    sock.on("error", (err) => {
      warn(`WebSocket error: ${(err as Error).message}`);
    });
    sock.on("close", () => {
      if (ws === sock) ws = null;
      if (!stopped) {
        warn("WebSocket close → 再接続をスケジュール");
        scheduleReconnect();
      }
    });
  }

  void connect();

  return {
    stop(): void {
      stopped = true;
      if (reconnectTimer) {
        clearTimeout(reconnectTimer);
        reconnectTimer = null;
      }
      try {
        ws?.close();
      } catch {
        /* ignore */
      }
      ws = null;
    },
  };
}
