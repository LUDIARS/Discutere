/**
 * Discutere — 外部サービスリレーインターフェース
 *
 * 外部 PM サービスへのタスク生成/更新をリレーする。
 * アダプタパターンで疎結合を実現。
 */

interface ExternalRelayAdapter {
  createTask(task: Record<string, unknown>): Promise<{ externalTaskId: string }>;
  updateTask(externalTaskId: string, updates: Record<string, unknown>): Promise<void>;
}

let currentAdapter: ExternalRelayAdapter | null = null;

export function registerPmRelayAdapter(adapter: ExternalRelayAdapter): void {
  currentAdapter = adapter;
  console.log("[discutere:relay] External relay adapter registered");
}

export function hasPmRelay(): boolean {
  return currentAdapter !== null;
}

export async function relayTaskToPm(
  task: Record<string, unknown>
): Promise<{ externalTaskId: string } | null> {
  if (!currentAdapter) {
    console.log(`[discutere:relay] No adapter — skipping relay for task "${task.title}"`);
    return null;
  }
  try {
    const result = await currentAdapter.createTask(task);
    console.log(`[discutere:relay] Task "${task.title}" → external (externalTaskId: ${result.externalTaskId})`);
    return result;
  } catch (err) {
    console.error(`[discutere:relay] Relay error:`, err);
    return null;
  }
}

export async function relayTaskUpdateToPm(
  externalTaskId: string,
  updates: Record<string, unknown>
): Promise<boolean> {
  if (!currentAdapter) return false;
  try {
    await currentAdapter.updateTask(externalTaskId, updates);
    return true;
  } catch (err) {
    console.error(`[discutere:relay] Update relay error:`, err);
    return false;
  }
}
