export const EVENT_TYPES = {
  PersonCreated: "PersonCreated",
  PersonUpdated: "PersonUpdated",
  SessionCreated: "SessionCreated",
  SessionUpdated: "SessionUpdated",
  UtteranceCreated: "UtteranceCreated",
  UtteranceNormalized: "UtteranceNormalized",
  ReactionAdded: "ReactionAdded",
  DesignGapDetected: "DesignGapDetected",
  DesignGapUpdated: "DesignGapUpdated",
  HypothesisCreated: "HypothesisCreated",
  HypothesisUpdated: "HypothesisUpdated",
  HypothesisIntegrated: "HypothesisIntegrated",
  MechanicProposed: "MechanicProposed",
  AffectMeasured: "AffectMeasured",
  PlayContextCaptured: "PlayContextCaptured",
} as const;

export type EventType = (typeof EVENT_TYPES)[keyof typeof EVENT_TYPES];

export interface BasePayload {
  workspaceId: string;
  id: string;
  at?: number;
}

export interface PersonPayload extends BasePayload {
  name: string;
  role?: string;
}

export interface SessionPayload extends BasePayload {
  title: string;
  startedAt: number;
  endedAt?: number;
}

export interface UtteranceCreatedPayload extends BasePayload {
  sessionId: string;
  speakerId?: string;
  rawContent: string;
  normalizedContent?: string;
  postedAt: number;
}

export interface UtteranceNormalizedPayload extends BasePayload {
  normalizedContent: string;
}

export interface ReactionPayload extends BasePayload {
  utteranceId: string;
  actorId?: string;
  reactionType: string;
  intensity?: number;
}

export interface DesignGapPayload extends BasePayload {
  gameId?: string;
  title: string;
  description?: string;
  status?: string;
}

export interface HypothesisPayload extends BasePayload {
  designGapId?: string;
  statement: string;
  status?: string;
  integrated?: boolean;
}

export interface MechanicProposedPayload extends BasePayload {
  gameId?: string;
  name: string;
  description?: string;
}

export interface AffectMeasuredPayload extends BasePayload {
  sessionId?: string;
  subjectId?: string;
  mood: string;
  score?: number;
}

export interface PlayContextPayload extends BasePayload {
  gameId?: string;
  platform?: string;
  mode?: string;
}

export type EventPayload =
  | PersonPayload
  | SessionPayload
  | UtteranceCreatedPayload
  | UtteranceNormalizedPayload
  | ReactionPayload
  | DesignGapPayload
  | HypothesisPayload
  | MechanicProposedPayload
  | AffectMeasuredPayload
  | PlayContextPayload;

export interface DomainEvent<T extends EventPayload = EventPayload> {
  id: string;
  eventType: EventType;
  payload: T;
  createdAt: number;
}
