/**
 * Fundamentum (`lib/fundamentum` submodule) の Discutere 側シングルトン配線。
 * SRP: on-disk Foundation の遅延初期化のみ。
 */
import { Foundation } from "fundamentum";
import { getConfig } from "../config.js";

let instance: Foundation | undefined;

export function getFundamentumStore(): Foundation {
  if (!instance) {
    const config = getConfig();
    instance = Foundation.onDisk(config.fundamentum.dataDir);
  }
  return instance;
}

/** テスト用: シングルトンを差し替え可能にする (in-memory Foundation 等)。 */
export function setFundamentumStoreForTests(store: Foundation | undefined): void {
  instance = store;
}
