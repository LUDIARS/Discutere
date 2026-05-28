import fs from "node:fs";
import path from "node:path";

import { randomUUID } from "node:crypto";

import type { KuzuClient } from "../db/kuzu-client.js";
import type { DomainEvent, EventPayload, EventType } from "./event-types.js";
import { applyEventProjection } from "./projection.js";

export class EventLog {
  private readonly jsonlPath: string;

  constructor(private readonly client: KuzuClient, jsonlPath = "./data/events.jsonl") {
    this.jsonlPath = path.resolve(jsonlPath);
    fs.mkdirSync(path.dirname(this.jsonlPath), { recursive: true });
    if (!fs.existsSync(this.jsonlPath)) fs.writeFileSync(this.jsonlPath, "", "utf8");
  }

  appendEvent<T extends EventPayload>(event: Omit<DomainEvent<T>, "id" | "createdAt"> & { id?: string; createdAt?: number }): DomainEvent<T> {
    const full: DomainEvent<T> = {
      id: event.id ?? randomUUID(),
      eventType: event.eventType,
      payload: event.payload,
      createdAt: event.createdAt ?? Date.now(),
    };

    const payloadJson = JSON.stringify(full.payload);
    const db = this.client.raw;
    const tx = db.transaction(() => {
      db.prepare("INSERT INTO events (id, event_type, payload_json, created_at) VALUES (?, ?, ?, ?)")
        .run(full.id, full.eventType, payloadJson, full.createdAt);
      applyEventProjection(db, full);
    });
    tx();

    fs.appendFileSync(this.jsonlPath, `${JSON.stringify(full)}\n`, "utf8");
    return full;
  }

  loadEventsSince(input: { eventId?: string; timestamp?: number } = {}): DomainEvent[] {
    const db = this.client.raw;
    if (input.eventId) {
      const rows = db
        .prepare("SELECT id, event_type, payload_json, created_at FROM events WHERE created_at >= (SELECT created_at FROM events WHERE id = ?) ORDER BY created_at ASC")
        .all(input.eventId) as Array<{ id: string; event_type: EventType; payload_json: string; created_at: number }>;
      return rows.map((r) => ({
        id: r.id,
        eventType: r.event_type,
        payload: JSON.parse(r.payload_json),
        createdAt: r.created_at,
      }));
    }

    const timestamp = input.timestamp ?? 0;
    const rows = db
      .prepare("SELECT id, event_type, payload_json, created_at FROM events WHERE created_at >= ? ORDER BY created_at ASC")
      .all(timestamp) as Array<{ id: string; event_type: EventType; payload_json: string; created_at: number }>;

    return rows.map((r) => ({
      id: r.id,
      eventType: r.event_type,
      payload: JSON.parse(r.payload_json),
      createdAt: r.created_at,
    }));
  }

  rebuildFromEventStore(): void {
    const db = this.client.raw;
    const rows = db.prepare("SELECT id, event_type, payload_json, created_at FROM events ORDER BY created_at ASC").all() as Array<{
      id: string;
      event_type: EventType;
      payload_json: string;
      created_at: number;
    }>;

    const tx = db.transaction(() => {
      db.exec("DELETE FROM people; DELETE FROM games; DELETE FROM mechanics; DELETE FROM aesthetics; DELETE FROM affects; DELETE FROM play_contexts; DELETE FROM sessions; DELETE FROM utterances; DELETE FROM reactions; DELETE FROM design_gaps; DELETE FROM hypotheses;");
      for (const row of rows) {
        applyEventProjection(db, {
          id: row.id,
          eventType: row.event_type,
          payload: JSON.parse(row.payload_json),
          createdAt: row.created_at,
        });
      }
    });
    tx();
  }
}
