export type EntityType =
  | "person"
  | "game"
  | "mechanic"
  | "aesthetic"
  | "affect"
  | "play_context"
  | "session"
  | "utterance"
  | "reaction"
  | "design_gap"
  | "hypothesis";

export interface BaseEntity {
  id: string;
  workspaceId: string;
  createdAt: number;
  updatedAt: number;
}

export interface Person extends BaseEntity {
  name: string;
  role?: string;
}

export interface Game extends BaseEntity {
  title: string;
  genre?: string;
}

export interface Mechanic extends BaseEntity {
  gameId?: string;
  name: string;
  description?: string;
}

export interface Aesthetic extends BaseEntity {
  gameId?: string;
  name: string;
  description?: string;
}

export interface Affect extends BaseEntity {
  sessionId?: string;
  subjectId?: string;
  mood: string;
  score?: number;
}

export interface PlayContext extends BaseEntity {
  gameId?: string;
  platform?: string;
  mode?: string;
}

export interface Session extends BaseEntity {
  title: string;
  startedAt: number;
  endedAt?: number;
}

export interface Utterance extends BaseEntity {
  sessionId: string;
  speakerId?: string;
  rawContent: string;
  normalizedContent?: string;
  postedAt: number;
}

export interface Reaction extends BaseEntity {
  utteranceId: string;
  actorId?: string;
  reactionType: string;
  intensity?: number;
}

export interface DesignGap extends BaseEntity {
  gameId?: string;
  title: string;
  description?: string;
  status?: string;
}

export interface Hypothesis extends BaseEntity {
  designGapId?: string;
  statement: string;
  status?: string;
  integrated?: boolean;
}

export interface EventRecord {
  id: string;
  eventType: string;
  payloadJson: string;
  createdAt: number;
}
