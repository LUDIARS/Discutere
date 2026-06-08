/**
 * 学習データバックアップの実行 + 月次スケジューラ.
 *
 * - runBackup(): tar.gz 化 → S3 push → state 更新。手動 (slash / npm script) と
 *   自動 (scheduler) の両方から呼ぶ単一エントリ。
 * - startBackupScheduler(): config.backup.enabled なら、起動時に overdue 判定で 1 回 +
 *   以後 24h tick で intervalDays (既定 30 = 月次) 経過ごとに runBackup。
 *
 * 「使うのは S3 のアーカイバ」要件: storageClass=GLACIER 系で安価に長期保管。
 */

import { mkdirSync, readFileSync, writeFileSync, rmSync, existsSync } from "node:fs";
import path from "node:path";
import os from "node:os";

import type { DiscutereConfig } from "../config.js";
import { resolveActiveKgPath } from "../core/kg-registry.js";
import { createArchive } from "./archive.js";
import { uploadFileToS3 } from "./s3.js";

export interface BackupResult {
  ok: boolean;
  key?: string;
  bucket?: string;
  bytes?: number;
  storageClass?: string;
  included?: string[];
  skipped?: string[];
  error?: string;
}

interface BackupState {
  lastRunAt?: number;
  lastKey?: string;
}

const DAY_MS = 24 * 60 * 60 * 1000;

/** バックアップ対象の実ファイル群を config から導出 */
function resolveTargets(config: DiscutereConfig): string[] {
  const dbPath = process.env.DATABASE_PATH ?? "./data/discutere.db";
  return [
    // active KG (§12) をバックアップ対象に。 未解決時は従来既定。
    resolveActiveKgPath(config) ?? "./data/discatier.kuzu",
    config.personaEngine.dbPath,
    dbPath,
  ];
}

function readState(stateFile: string): BackupState {
  if (!existsSync(stateFile)) return {};
  try {
    return JSON.parse(readFileSync(stateFile, "utf8")) as BackupState;
  } catch {
    return {};
  }
}

function writeState(stateFile: string, state: BackupState): void {
  mkdirSync(path.dirname(stateFile), { recursive: true });
  writeFileSync(stateFile, JSON.stringify(state, null, 2), "utf8");
}

/** タイムスタンプ付き S3 key を組み立てる (prefix/workspace/discutere-backup-YYYYMMDD-HHmmss.tar.gz) */
function buildKey(config: DiscutereConfig): string {
  const d = new Date();
  const pad = (n: number) => String(n).padStart(2, "0");
  const ts =
    `${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}` +
    `-${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}`;
  const prefix = config.backup.prefix.endsWith("/") ? config.backup.prefix : config.backup.prefix + "/";
  return `${prefix}${config.workspace}/discutere-backup-${ts}.tar.gz`;
}

/**
 * バックアップを 1 回実行する。bucket 未設定なら ok=false を返す (throw しない)。
 */
export async function runBackup(config: DiscutereConfig): Promise<BackupResult> {
  if (!config.backup.bucket) {
    return { ok: false, error: "backup.bucket が未設定です (S3 バックアップ不可)" };
  }

  const tmpDir = path.join(os.tmpdir(), "discutere-backup");
  mkdirSync(tmpDir, { recursive: true });
  const tmpFile = path.join(tmpDir, `backup-${process.pid}-${Date.now()}.tar.gz`);

  try {
    const archive = await createArchive({ targets: resolveTargets(config), outFile: tmpFile });
    const key = buildKey(config);
    const up = await uploadFileToS3({
      bucket: config.backup.bucket,
      key,
      filePath: tmpFile,
      region: config.backup.region,
      storageClass: config.backup.storageClass,
      endpoint: config.backup.endpoint,
      accessKeyId: config.backup.accessKeyId,
      secretAccessKey: config.backup.secretAccessKey,
    });
    writeState(config.backup.stateFile, { lastRunAt: Date.now(), lastKey: key });
    return {
      ok: true,
      key: up.key,
      bucket: up.bucket,
      bytes: up.bytes,
      storageClass: up.storageClass,
      included: archive.included,
      skipped: archive.skipped,
    };
  } catch (err) {
    return { ok: false, error: (err as Error).message };
  } finally {
    try {
      rmSync(tmpFile, { force: true });
    } catch {
      /* ignore */
    }
  }
}

export interface BackupSchedulerHandle {
  stop(): void;
  /** 手動トリガ (slash / API から共有) */
  trigger(): Promise<BackupResult>;
}

/**
 * 月次 (intervalDays) 自動バックアップを開始する。
 * enabled=false / bucket 未設定なら trigger だけ生きた no-op スケジューラを返す。
 */
export function startBackupScheduler(config: DiscutereConfig): BackupSchedulerHandle {
  const trigger = () => runBackup(config);

  if (!config.backup.enabled || !config.backup.bucket) {
    console.log(
      `  backup: scheduler skipped (enabled=${config.backup.enabled}, bucket=${config.backup.bucket ? "set" : "unset"})`
    );
    return { stop() {}, trigger };
  }

  const intervalMs = Math.max(1, config.backup.intervalDays) * DAY_MS;

  const maybeRun = async () => {
    const state = readState(config.backup.stateFile);
    const elapsed = state.lastRunAt ? Date.now() - state.lastRunAt : Infinity;
    if (elapsed < intervalMs) return;
    console.log(`  backup: running scheduled backup (elapsed=${Math.round(elapsed / DAY_MS)}d)`);
    const r = await runBackup(config);
    if (r.ok) console.log(`  backup: uploaded s3://${r.bucket}/${r.key} (${r.bytes} bytes, ${r.storageClass})`);
    else console.warn(`  backup: scheduled backup failed: ${r.error}`);
  };

  // 起動直後の overdue チェック (即時には走らせず軽く遅延)
  const kickoff = setTimeout(() => void maybeRun(), 30_000);
  // 以後 24h 毎に overdue 判定
  const daily = setInterval(() => void maybeRun(), DAY_MS);
  console.log(
    `  backup: scheduler attached (every ${config.backup.intervalDays}d → s3://${config.backup.bucket}/${config.backup.prefix})`
  );

  return {
    stop() {
      clearTimeout(kickoff);
      clearInterval(daily);
    },
    trigger,
  };
}
