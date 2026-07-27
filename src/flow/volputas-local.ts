/**
 * ローカル実行 Volputas のペルソナ分析を読む HTTP 境界。
 *
 * 接続先は Excubitor topology が注入する VOLPUTAS_URL を正本とし、ポートを保持しない。
 * ローカル専用機能なので loopback URL 以外は受理しない。
 */

export interface VolputasPersonaAxis {
  label: string;
  score: number;
  evidenceWeight: number;
}

export interface VolputasPersonaAnalysis {
  schemaVersion: number;
  modelVersion: string;
  analyzedAt: string;
  axes: Record<string, VolputasPersonaAxis>;
  leadingAxes: Array<VolputasPersonaAxis & { id: string }>;
  evidence: {
    surveys: number;
    gameplay: number;
    voices: number;
    emotionCurves: number;
  };
  note: string;
}

export interface VolputasPersonaStatus {
  analysis: VolputasPersonaAnalysis | null;
  evidenceCount: number;
  stale: boolean;
}

/**
 * サービス間で共有する読み取り専用スナップショット。
 * facet/dimensions はタグ付きなので、Vo側の分析項目追加で Di の型を破壊しない。
 */
export interface PersonaDiscussionSnapshot {
  schema: "ludiars.persona-discussion-snapshot";
  schemaVersion: 1;
  subject: { kind: "local-self" };
  source: {
    service: "volputas";
    modelVersion: string;
    analyzedAt: string;
    freshness: "current" | "stale";
  };
  facets: Array<{
    key: string;
    dimensions: Array<{
      key: string;
      label: string;
      score: number;
      scale: { min: 0; max: 100 };
      evidenceWeight: number;
    }>;
  }>;
  evidenceSummary: {
    total: number;
    byType: Record<string, number>;
  };
  notes: string[];
}

type FetchLike = typeof fetch;

function requiredObject(value: unknown, label: string): Record<string, unknown> {
  if (!value || typeof value !== "object" || Array.isArray(value)) {
    throw new TypeError(`${label} must be an object`);
  }
  return value as Record<string, unknown>;
}

function finiteNumber(value: unknown, label: string): number {
  if (typeof value !== "number" || !Number.isFinite(value)) {
    throw new TypeError(`${label} must be a finite number`);
  }
  return value;
}

function parseAnalysis(value: unknown): VolputasPersonaAnalysis | null {
  if (value === null) return null;
  const item = requiredObject(value, "Volputas persona analysis");
  const axesInput = requiredObject(item.axes, "Volputas persona axes");
  const axes: Record<string, VolputasPersonaAxis> = {};
  for (const [id, raw] of Object.entries(axesInput)) {
    const axis = requiredObject(raw, `Volputas persona axis ${id}`);
    if (typeof axis.label !== "string") throw new TypeError(`Volputas persona axis ${id}.label must be a string`);
    axes[id] = {
      label: axis.label,
      score: finiteNumber(axis.score, `Volputas persona axis ${id}.score`),
      evidenceWeight: finiteNumber(axis.evidenceWeight, `Volputas persona axis ${id}.evidenceWeight`),
    };
  }
  const evidence = requiredObject(item.evidence, "Volputas persona evidence");
  const leadingAxes = Array.isArray(item.leadingAxes)
    ? item.leadingAxes.map((raw, index) => {
        const axis = requiredObject(raw, `Volputas leading axis ${index}`);
        if (typeof axis.id !== "string" || typeof axis.label !== "string") {
          throw new TypeError(`Volputas leading axis ${index} is invalid`);
        }
        return {
          id: axis.id,
          label: axis.label,
          score: finiteNumber(axis.score, `Volputas leading axis ${index}.score`),
          evidenceWeight: finiteNumber(axis.evidenceWeight, `Volputas leading axis ${index}.evidenceWeight`),
        };
      })
    : [];
  if (
    typeof item.schemaVersion !== "number" ||
    typeof item.modelVersion !== "string" ||
    typeof item.analyzedAt !== "string"
  ) {
    throw new TypeError("Volputas persona analysis metadata is invalid");
  }
  return {
    schemaVersion: item.schemaVersion,
    modelVersion: item.modelVersion,
    analyzedAt: item.analyzedAt,
    axes,
    leadingAxes,
    evidence: {
      surveys: finiteNumber(evidence.surveys, "Volputas evidence.surveys"),
      gameplay: finiteNumber(evidence.gameplay, "Volputas evidence.gameplay"),
      voices: finiteNumber(evidence.voices, "Volputas evidence.voices"),
      emotionCurves: finiteNumber(evidence.emotionCurves, "Volputas evidence.emotionCurves"),
    },
    note: typeof item.note === "string" ? item.note : "",
  };
}

function parseStatus(value: unknown): VolputasPersonaStatus {
  const envelope = requiredObject(value, "Volputas response");
  if (envelope.ok !== true) {
    const error = envelope.error && typeof envelope.error === "object"
      ? (envelope.error as { message?: unknown }).message
      : undefined;
    throw new Error(typeof error === "string" ? error : "Volputas returned an error");
  }
  const data = requiredObject(envelope.data, "Volputas response data");
  return {
    analysis: parseAnalysis(data.analysis),
    evidenceCount: finiteNumber(data.evidenceCount, "Volputas evidenceCount"),
    stale: data.stale === true,
  };
}

export function resolveVolputasBaseUrl(
  env: NodeJS.ProcessEnv = process.env
): string {
  const raw = env.VOLPUTAS_URL?.trim() || env.VOLUPTAS_URL?.trim();
  if (!raw) {
    throw new Error("VOLPUTAS_URL is not set; start Discutere through Excubitor topology");
  }
  const url = new URL(raw);
  if (!["127.0.0.1", "localhost", "::1", "[::1]"].includes(url.hostname)) {
    throw new Error(`Volputas local URL must use loopback host: ${url.hostname}`);
  }
  return url.toString().replace(/\/$/, "");
}

async function requestJson(fetchFn: FetchLike, url: string, init?: RequestInit): Promise<unknown> {
  const response = await fetchFn(url, init);
  const body = await response.json().catch(() => null);
  if (!response.ok) {
    const message = body && typeof body === "object" && "error" in body
      ? JSON.stringify((body as { error: unknown }).error)
      : `HTTP ${response.status}`;
    throw new Error(`Volputas request failed: ${message}`);
  }
  return body;
}

/**
 * refresh=true は Volputas の差分判定付き analyze を先に呼ぶ。
 * 入力が前回分析から変わっていなければ Volputas 側が再分析を省略する。
 */
export async function fetchLocalVolputasPersona(opts: {
  baseUrl?: string;
  refresh?: boolean;
  fetchFn?: FetchLike;
} = {}): Promise<VolputasPersonaStatus> {
  const baseUrl = opts.baseUrl ? resolveVolputasBaseUrl({ VOLPUTAS_URL: opts.baseUrl }) : resolveVolputasBaseUrl();
  const fetchFn = opts.fetchFn ?? fetch;
  if (opts.refresh) {
    await requestJson(fetchFn, `${baseUrl}/api/local/persona/analyze`, { method: "POST" });
  }
  return parseStatus(await requestJson(fetchFn, `${baseUrl}/api/local/persona`));
}

export function toPersonaDiscussionSnapshot(
  status: VolputasPersonaStatus
): PersonaDiscussionSnapshot {
  if (!status.analysis) throw new Error("Volputas persona has not been analyzed");
  const analysis = status.analysis;
  return {
    schema: "ludiars.persona-discussion-snapshot",
    schemaVersion: 1,
    subject: { kind: "local-self" },
    source: {
      service: "volputas",
      modelVersion: analysis.modelVersion,
      analyzedAt: analysis.analyzedAt,
      freshness: status.stale ? "stale" : "current",
    },
    facets: [{
      key: "volputas.evidence-persona.axes",
      dimensions: Object.entries(analysis.axes).map(([key, axis]) => ({
        key,
        label: axis.label,
        score: axis.score,
        scale: { min: 0, max: 100 },
        evidenceWeight: axis.evidenceWeight,
      })),
    }],
    evidenceSummary: {
      total: status.evidenceCount,
      byType: {
        surveys: analysis.evidence.surveys,
        gameplay: analysis.evidence.gameplay,
        voices: analysis.evidence.voices,
        emotionCurves: analysis.evidence.emotionCurves,
      },
    },
    notes: [
      analysis.note,
      "This snapshot is discussion context, not a diagnosis or identity record.",
    ].filter(Boolean),
  };
}

/** 個人名を含めず、ペーパーへ載せる当事者ペルソナ文脈に整形する。 */
export function formatVolputasPersonaContext(status: VolputasPersonaStatus): string {
  const snapshot = toPersonaDiscussionSnapshot(status);
  const axes = snapshot.facets
    .flatMap((facet) => facet.dimensions)
    .filter((axis) => axis.evidenceWeight > 0)
    .sort((left, right) => right.score - left.score)
    .map((axis) => `- ${axis.label} (${axis.key}): ${Math.round(axis.score)}/100`)
    .join("\n");
  const evidence = snapshot.evidenceSummary.byType;
  return [
    "## Volputas ローカルペルソナ（当事者の観点）",
    `分析日時: ${snapshot.source.analyzedAt}${snapshot.source.freshness === "stale" ? "（入力更新後・再分析前）" : ""}`,
    axes || "- 有効な傾向データなし",
    `根拠件数: アンケート ${evidence.surveys} / ゲームプレイ ${evidence.gameplay} / ユーザの声 ${evidence.voices} / 感情曲線 ${evidence.emotionCurves}`,
    ...snapshot.notes.slice(0, 1),
    "この傾向は議論の唯一の正解ではなく、当事者視点を検討するための参考情報として扱う。",
  ].filter(Boolean).join("\n");
}
