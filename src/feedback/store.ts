/**
 * ゲーム感想の匿名収集ストア。
 *
 * Discord の「ゲーム感想」カテゴリ配下のチャンネル (チャンネル名 = ゲームタイトル) に
 * 投稿された感想を、議論にせず意見データとして蓄積する。後で分類器 / persona の
 * プロンプトに「そのゲームへの実際の人間の感想」として差し込む (議論を実データに接地)。
 *
 * 個人データ規約 (CLAUDE.md): 投稿者名・アカウント名は保存しない (匿名)。
 * 保存するのは gameTitle / 本文 / 時刻 のみ。
 *
 * spec/feature/game-feedback.md。
 */

import type Database from "better-sqlite3";

export interface GameFeedback {
  id: number;
  gameTitle: string;
  content: string;
  createdAt: number;
}

export class GameFeedbackStore {
  constructor(private readonly db: Database.Database) {
    this.db.exec(`
      CREATE TABLE IF NOT EXISTS game_feedback (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        game_title TEXT NOT NULL,
        content TEXT NOT NULL,
        created_at INTEGER NOT NULL
      );
      CREATE INDEX IF NOT EXISTS idx_game_feedback_title ON game_feedback (game_title, created_at);
    `);
  }

  /** 感想を 1 件追加 (投稿者は保存しない)。空本文は無視。 */
  add(input: { gameTitle: string; content: string; createdAt: number }): { id: number } | null {
    const gameTitle = input.gameTitle.trim();
    const content = input.content.trim();
    if (!gameTitle || !content) return null;
    const info = this.db
      .prepare("INSERT INTO game_feedback (game_title, content, created_at) VALUES (?, ?, ?)")
      .run(gameTitle, content, input.createdAt);
    return { id: Number(info.lastInsertRowid) };
  }

  /** ゲームの直近感想を新しい順で返す。 */
  recentForGame(gameTitle: string, limit = 12): GameFeedback[] {
    const rows = this.db
      .prepare(
        "SELECT id, game_title, content, created_at FROM game_feedback WHERE game_title = ? ORDER BY created_at DESC LIMIT ?"
      )
      .all(gameTitle.trim(), limit) as Array<{
      id: number;
      game_title: string;
      content: string;
      created_at: number;
    }>;
    return rows.map((r) => ({ id: r.id, gameTitle: r.game_title, content: r.content, createdAt: r.created_at }));
  }

  /** ゲームの感想件数。 */
  countForGame(gameTitle: string): number {
    const row = this.db
      .prepare("SELECT COUNT(*) AS c FROM game_feedback WHERE game_title = ?")
      .get(gameTitle.trim()) as { c: number };
    return row.c;
  }

  /** 収集済みゲームタイトル一覧 (件数付き)。 */
  listGames(): Array<{ gameTitle: string; count: number }> {
    const rows = this.db
      .prepare(
        "SELECT game_title, COUNT(*) AS c FROM game_feedback GROUP BY game_title ORDER BY c DESC"
      )
      .all() as Array<{ game_title: string; c: number }>;
    return rows.map((r) => ({ gameTitle: r.game_title, count: r.c }));
  }
}
