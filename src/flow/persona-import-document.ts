// @spec インポート (Vo → Di)
export interface PersonaDocument {
  personas: unknown[];
  invalidJsonLines: number;
}

function asPersonas(value: unknown): unknown[] {
  if (Array.isArray(value)) return value;
  if (value && typeof value === "object") {
    const personas = (value as Record<string, unknown>).personas;
    return Array.isArray(personas) ? personas : [value];
  }
  return [value];
}

export function parsePersonaDocument(text: string): PersonaDocument {
  const trimmed = text.trim();
  if (!trimmed) return { personas: [], invalidJsonLines: 0 };
  try {
    return { personas: asPersonas(JSON.parse(trimmed) as unknown), invalidJsonLines: 0 };
  } catch {
    const personas: unknown[] = [];
    let invalidJsonLines = 0;
    for (const line of trimmed.split(/\r?\n/).filter(Boolean)) {
      try {
        personas.push(JSON.parse(line) as unknown);
      } catch {
        invalidJsonLines += 1;
      }
    }
    return { personas, invalidJsonLines };
  }
}
