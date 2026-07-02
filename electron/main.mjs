/**
 * Discutere デスクトップアプリ (Electron) のメインプロセス。
 *
 * 方式: 既存の Node サーバ (dist/index.js = Hono HTTP + Discord Gateway) を
 * Electron main プロセス内でそのまま import して起動し、BrowserWindow で
 * トップページ (http://127.0.0.1:<port>/) を開く「ランチャー + 内蔵ブラウザ」構成。
 * サーバ本体は無改変 (Web/常駐運用と同一コード)。詳細は docs/desktop-app.md。
 *
 * データ配置: 書き込みは全て userData (Windows: %APPDATA%/Discutere) 配下。
 * サーバは cwd 相対 (./data/*.db, ./discutere.config.json) でパス解決するため、
 * 起動前に process.chdir(userData) し、リポ同梱の読み取りデータ
 * (data/games, data/affects, worker-home, config example) を初回のみシードする。
 */

import { app, BrowserWindow, Menu, dialog, shell } from "electron";
import { appendFileSync, cpSync, existsSync, mkdirSync, readFileSync } from "node:fs";
import { join } from "node:path";
import { pathToFileURL } from "node:url";

// ── 単一インスタンス (2 重起動はポート衝突するので既存ウィンドウへフォーカス) ──
const gotLock = app.requestSingleInstanceLock();
if (!gotLock) {
  app.quit();
}

const appRoot = app.getAppPath(); // dev = リポルート / packaged = resources/app
// シード元: packaged は extraResources (resources/seed)、dev はリポルート直下。
const seedRoot = app.isPackaged ? join(process.resourcesPath, "seed") : appRoot;

let mainWindow = null;
let logFile = null;
let startupDone = false;

/** console 出力を userData/logs のファイルにも tee する (packaged では stdout が見えないため)。 */
function setupLogging(userDataDir) {
  mkdirSync(join(userDataDir, "logs"), { recursive: true });
  logFile = join(userDataDir, "logs", "discutere-desktop.log");
  const tee = (orig) => (...args) => {
    orig(...args);
    try {
      const line = args
        .map((a) => (typeof a === "string" ? a : a instanceof Error ? (a.stack ?? a.message) : JSON.stringify(a)))
        .join(" ");
      appendFileSync(logFile, `${new Date().toISOString()} ${line}\n`);
    } catch {
      /* ログ書き込み失敗でアプリを止めない */
    }
  };
  console.log = tee(console.log.bind(console));
  console.warn = tee(console.warn.bind(console));
  console.error = tee(console.error.bind(console));
}

/**
 * 初回起動シード: リポ同梱の読み取りデータを userData へコピーする (既存なら触らない)。
 * サーバが cwd 相対で読む data/games (メカニクス md)・data/affects・worker-home と、
 * config の雛形 (discutere.config.example.json → discutere.config.json)。
 */
function seedUserData(userDataDir) {
  mkdirSync(join(userDataDir, "data"), { recursive: true });
  const pairs = [
    [join(seedRoot, "data", "games"), join(userDataDir, "data", "games")],
    [join(seedRoot, "data", "affects"), join(userDataDir, "data", "affects")],
    [join(seedRoot, "worker-home"), join(userDataDir, "worker-home")],
    [join(seedRoot, "discutere.config.example.json"), join(userDataDir, "discutere.config.json")],
  ];
  for (const [from, to] of pairs) {
    if (!existsSync(to) && existsSync(from)) cpSync(from, to, { recursive: true });
  }
}

/** config ファイル + env から HTTP bind 先を解決する (src/config.ts の既定と揃える)。 */
function resolveServerAddress(configPath) {
  let port = 3110;
  let host = "127.0.0.1";
  try {
    const cfg = JSON.parse(readFileSync(configPath, "utf8"));
    if (typeof cfg?.server?.port === "number") port = cfg.server.port;
    if (typeof cfg?.server?.host === "string" && cfg.server.host) host = cfg.server.host;
  } catch {
    /* config 無し/壊れは src/config.ts 側の既定に任せる */
  }
  if (process.env.BACKEND_PORT) port = Number(process.env.BACKEND_PORT) || port;
  if (process.env.BACKEND_HOST) host = process.env.BACKEND_HOST;
  // 0.0.0.0 等の全 IF bind でもウィンドウからは loopback で繋ぐ。
  const displayHost = host === "0.0.0.0" || host === "::" ? "127.0.0.1" : host;
  return { port, baseUrl: `http://${displayHost}:${port}` };
}

/** サーバ本体 (dist/index.js) を main プロセスに import して起動する。 */
async function startServer() {
  const entry = join(appRoot, "dist", "index.js");
  if (!existsSync(entry)) {
    throw new Error(`サーバ本体が見つかりません: ${entry}\n(開発時は先に \`npm run build\` を実行)`);
  }
  await import(pathToFileURL(entry).href);
}

/** /health が 200 を返すまでポーリングする。 */
async function waitForHealth(url, timeoutMs) {
  const deadline = Date.now() + timeoutMs;
  let lastErr = null;
  while (Date.now() < deadline) {
    try {
      const res = await fetch(url);
      if (res.ok) return;
      lastErr = new Error(`health check: HTTP ${res.status}`);
    } catch (err) {
      lastErr = err;
    }
    await new Promise((r) => setTimeout(r, 500));
  }
  throw new Error(`サーバが起動しませんでした (${url}): ${lastErr?.message ?? "timeout"}`);
}

function buildMenu(userDataDir, configPath) {
  const template = [
    ...(process.platform === "darwin" ? [{ role: "appMenu" }] : []),
    {
      label: "ファイル",
      submenu: [
        { label: "データフォルダを開く", click: () => shell.openPath(userDataDir) },
        { label: "設定ファイルを開く (要再起動)", click: () => shell.openPath(configPath) },
        { label: "ログを開く", click: () => logFile && shell.openPath(logFile) },
        { type: "separator" },
        { role: "quit", label: "終了" },
      ],
    },
    {
      label: "表示",
      submenu: [
        { role: "reload" },
        { role: "forceReload" },
        { role: "toggleDevTools" },
        { type: "separator" },
        { role: "resetZoom" },
        { role: "zoomIn" },
        { role: "zoomOut" },
        { type: "separator" },
        { role: "togglefullscreen" },
      ],
    },
  ];
  Menu.setApplicationMenu(Menu.buildFromTemplate(template));
}

function createWindow(baseUrl) {
  const win = new BrowserWindow({
    width: 1280,
    height: 860,
    title: "Discutere",
    webPreferences: { nodeIntegration: false, contextIsolation: true, sandbox: true },
  });
  const isInternal = (url) => url.startsWith(`${baseUrl}/`) || url === baseUrl;
  // 外部リンク (Discord 招待や出所 URL 等) は OS ブラウザへ。UI はアプリ内。
  win.webContents.setWindowOpenHandler(({ url }) => {
    if (!isInternal(url)) {
      void shell.openExternal(url);
      return { action: "deny" };
    }
    return { action: "allow" };
  });
  win.webContents.on("will-navigate", (e, url) => {
    if (!isInternal(url)) {
      e.preventDefault();
      void shell.openExternal(url);
    }
  });
  // サーバ起動待ちの間に出す簡易ローディング画面。
  const loading = `<!doctype html><meta charset="utf-8"><title>Discutere</title>
    <body style="font-family:sans-serif;display:flex;align-items:center;justify-content:center;height:95vh;color:#444">
    <div style="text-align:center"><h1>Discutere</h1><p>議論エンジンを起動しています…</p></div></body>`;
  void win.loadURL(`data:text/html;charset=utf-8,${encodeURIComponent(loading)}`);
  return win;
}

function fatal(title, err) {
  console.error(`[desktop] ${title}:`, err);
  if (app.isReady()) {
    dialog.showErrorBox(
      `Discutere — ${title}`,
      `${err instanceof Error ? err.message : String(err)}\n\nログ: ${logFile ?? "(未初期化)"}`
    );
  }
  app.exit(1);
}

// serve() の EADDRINUSE 等、サーバ起動中の非同期例外を拾う (起動後は握りつぶさずログのみ)。
process.on("uncaughtException", (err) => {
  if (!startupDone) fatal("起動エラー", err);
  else console.error("[desktop] uncaughtException:", err);
});
process.on("unhandledRejection", (reason) => {
  console.error("[desktop] unhandledRejection:", reason);
});

app.on("second-instance", () => {
  if (mainWindow) {
    if (mainWindow.isMinimized()) mainWindow.restore();
    mainWindow.focus();
  }
});

// サーバごと終了する (常駐させない)。常駐運用は従来の `npm start` / systemd 等で。
app.on("window-all-closed", () => app.quit());

if (gotLock) {
  app
    .whenReady()
    .then(async () => {
      const userDataDir = app.getPath("userData");
      setupLogging(userDataDir);
      console.log(`[desktop] userData=${userDataDir} appRoot=${appRoot} packaged=${app.isPackaged}`);
      seedUserData(userDataDir);

      const configPath = process.env.DISCUTERE_CONFIG ?? join(userDataDir, "discutere.config.json");
      process.env.DISCUTERE_CONFIG = configPath;
      const { baseUrl } = resolveServerAddress(configPath);

      // サーバは cwd 相対 (./data/*.db 等) でパスを解決する → 書き込み先を userData に固定。
      process.chdir(userDataDir);

      buildMenu(userDataDir, configPath);
      mainWindow = createWindow(baseUrl);
      mainWindow.on("closed", () => {
        mainWindow = null;
      });

      await startServer();
      await waitForHealth(`${baseUrl}/health`, 60_000);
      startupDone = true;
      console.log(`[desktop] server ready — opening ${baseUrl}/`);
      if (mainWindow) await mainWindow.loadURL(`${baseUrl}/`);
    })
    .catch((err) => fatal("起動エラー", err));
}
