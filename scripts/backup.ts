/**
 * 学習データを今すぐ S3 にバックアップする (npm run backup).
 *
 * 自動 (月次スケジューラ) とは独立に、手動で 1 回アーカイブを push する。
 * 必要 config: backup.bucket (+ AWS 認証情報 / region / storageClass)。
 */

import { getConfig } from "../src/config.js";
import { runBackup } from "../src/backup/runner.js";

const config = getConfig();
const result = await runBackup(config);

if (result.ok) {
  console.log(`✅ uploaded s3://${result.bucket}/${result.key}`);
  console.log(`   ${result.bytes} bytes / storageClass=${result.storageClass}`);
  console.log(`   included: ${result.included?.join(", ")}`);
  if (result.skipped && result.skipped.length > 0) {
    console.log(`   skipped (not found): ${result.skipped.join(", ")}`);
  }
} else {
  console.error(`❌ backup failed: ${result.error}`);
  process.exit(1);
}
