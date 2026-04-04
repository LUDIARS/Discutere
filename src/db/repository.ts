/**
 * Discutere — リポジトリ層
 */
import { eq, and, desc } from "drizzle-orm";
import { db, schema } from "./connection.js";

type User = typeof schema.users.$inferSelect;
type Task = typeof schema.tasks.$inferSelect;
type TaskLog = typeof schema.taskLogs.$inferSelect;
type ChannelMonitor = typeof schema.channelMonitors.$inferSelect;

// ── Users ─────────────────────────────────────────

export const userRepo = {
  async findById(id: string): Promise<User | undefined> {
    const rows = await db.select().from(schema.users).where(eq(schema.users.id, id));
    return rows[0];
  },
  async upsert(data: { id: string; login: string; displayName: string; email: string | null; avatarUrl: string; role: string }): Promise<void> {
    const existing = await this.findById(data.id);
    if (existing) {
      await db.update(schema.users)
        .set({ ...data, updatedAt: new Date() })
        .where(eq(schema.users.id, data.id));
    } else {
      await db.insert(schema.users).values({ ...data, createdAt: new Date(), updatedAt: new Date() });
    }
  },
};

// ── Tasks ─────────────────────────────────────────

export const taskRepo = {
  async findByWorkspaceId(workspaceId: string): Promise<Task[]> {
    return db.select().from(schema.tasks)
      .where(eq(schema.tasks.workspaceId, workspaceId))
      .orderBy(desc(schema.tasks.createdAt));
  },
  async findByWorkspaceIdAndStatus(workspaceId: string, status: string): Promise<Task[]> {
    return db.select().from(schema.tasks)
      .where(and(eq(schema.tasks.workspaceId, workspaceId), eq(schema.tasks.status, status)))
      .orderBy(desc(schema.tasks.createdAt));
  },
  async findById(id: string): Promise<Task | undefined> {
    const rows = await db.select().from(schema.tasks).where(eq(schema.tasks.id, id));
    return rows[0];
  },
  async findByAssignee(assigneeId: string): Promise<Task[]> {
    return db.select().from(schema.tasks)
      .where(eq(schema.tasks.assigneeId, assigneeId))
      .orderBy(desc(schema.tasks.createdAt));
  },
  async create(data: typeof schema.tasks.$inferInsert): Promise<void> {
    await db.insert(schema.tasks).values(data);
  },
  async update(id: string, data: Partial<typeof schema.tasks.$inferInsert>): Promise<void> {
    await db.update(schema.tasks)
      .set({ ...data, updatedAt: new Date() })
      .where(eq(schema.tasks.id, id));
  },
  async deleteById(id: string): Promise<void> {
    await db.delete(schema.tasks).where(eq(schema.tasks.id, id));
  },
};

// ── Task Logs ─────────────────────────────────────

export const taskLogRepo = {
  async findByTaskId(taskId: string): Promise<TaskLog[]> {
    return db.select().from(schema.taskLogs)
      .where(eq(schema.taskLogs.taskId, taskId))
      .orderBy(desc(schema.taskLogs.createdAt));
  },
  async create(data: typeof schema.taskLogs.$inferInsert): Promise<void> {
    await db.insert(schema.taskLogs).values(data);
  },
};

// ── Channel Monitors ──────────────────────────────

export const monitorRepo = {
  async findByWorkspaceId(workspaceId: string): Promise<ChannelMonitor[]> {
    return db.select().from(schema.channelMonitors)
      .where(eq(schema.channelMonitors.workspaceId, workspaceId));
  },
  async findActiveByWorkspaceId(workspaceId: string): Promise<ChannelMonitor[]> {
    return db.select().from(schema.channelMonitors)
      .where(and(
        eq(schema.channelMonitors.workspaceId, workspaceId),
        eq(schema.channelMonitors.isActive, true),
      ));
  },
  async findById(id: string): Promise<ChannelMonitor | undefined> {
    const rows = await db.select().from(schema.channelMonitors).where(eq(schema.channelMonitors.id, id));
    return rows[0];
  },
  async create(data: typeof schema.channelMonitors.$inferInsert): Promise<void> {
    await db.insert(schema.channelMonitors).values(data);
  },
  async update(id: string, data: Partial<typeof schema.channelMonitors.$inferInsert>): Promise<void> {
    await db.update(schema.channelMonitors)
      .set({ ...data, updatedAt: new Date() })
      .where(eq(schema.channelMonitors.id, id));
  },
  async deleteById(id: string): Promise<void> {
    await db.delete(schema.channelMonitors).where(eq(schema.channelMonitors.id, id));
  },
};
