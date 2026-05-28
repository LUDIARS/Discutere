import Database from "better-sqlite3";
import fs from "node:fs";
import path from "node:path";

import { applyMigrations } from "./schema.js";

export class KuzuClient {
  private db: Database.Database;

  constructor(dbPath = process.env.DISCATIER_KUZU_PATH || "./data/discatier.kuzu") {
    const abs = path.resolve(dbPath);
    fs.mkdirSync(path.dirname(abs), { recursive: true });
    this.db = new Database(abs);
    this.db.pragma("journal_mode = WAL");
    this.db.pragma("foreign_keys = ON");
    applyMigrations(this.db);
  }

  get raw(): Database.Database {
    return this.db;
  }

  close(): void {
    this.db.close();
  }
}
