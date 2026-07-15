import { randomUUID } from "node:crypto";

const LAUNCH_TTL_MS = 60_000;
const ACTOR_SESSION_TTL_MS = 8 * 60 * 60 * 1000;

interface TimedValue<T> {
  value: T;
  expiresAt: number;
}

export const GLAB_ACTOR_COOKIE = "discutere_glab_actor";

export class GlabLaunchStore {
  private readonly launches = new Map<string, TimedValue<string>>();
  private readonly sessions = new Map<string, TimedValue<string>>();

  constructor(
    private readonly now: () => number = Date.now,
    private readonly newId: () => string = randomUUID,
  ) {}

  createLaunch(cernereUserId: string): string {
    const userId = cernereUserId.trim();
    if (!userId || userId.length > 200) throw new Error("invalid Cernere user ID");
    this.sweep();
    const ticket = this.newId();
    this.launches.set(ticket, { value: userId, expiresAt: this.now() + LAUNCH_TTL_MS });
    return ticket;
  }

  consumeLaunch(ticket: string): { actorSession: string; cernereUserId: string } | null {
    this.sweep();
    const launch = this.launches.get(ticket);
    if (!launch) return null;
    this.launches.delete(ticket);
    const actorSession = this.newId();
    this.sessions.set(actorSession, {
      value: launch.value,
      expiresAt: this.now() + ACTOR_SESSION_TTL_MS,
    });
    return { actorSession, cernereUserId: launch.value };
  }

  resolveActor(actorSession: string): string | null {
    this.sweep();
    return this.sessions.get(actorSession)?.value ?? null;
  }

  private sweep(): void {
    const now = this.now();
    for (const [key, value] of this.launches) if (value.expiresAt <= now) this.launches.delete(key);
    for (const [key, value] of this.sessions) if (value.expiresAt <= now) this.sessions.delete(key);
  }
}

export const glabLaunchStore = new GlabLaunchStore();
