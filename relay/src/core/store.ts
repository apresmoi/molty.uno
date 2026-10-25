// Storage + clock seams. The Durable Object host passes `ctx.storage`-backed
// implementations; tests and a future Node host use the in-memory ones.

export interface KV {
  get<T>(key: string): Promise<T | undefined>;
  put<T>(key: string, value: T): Promise<void>;
  delete(key: string): Promise<void>;
  /** All entries whose key starts with `prefix`. */
  list<T>(prefix: string): Promise<Map<string, T>>;
}

export class MemoryKV implements KV {
  readonly data = new Map<string, unknown>();
  async get<T>(key: string) {
    const v = this.data.get(key);
    return v === undefined ? undefined : (structuredClone(v) as T);
  }
  async put<T>(key: string, value: T) {
    this.data.set(key, structuredClone(value));
  }
  async delete(key: string) {
    this.data.delete(key);
  }
  async list<T>(prefix: string) {
    const out = new Map<string, T>();
    for (const [k, v] of [...this.data.entries()].sort(([a], [b]) => (a < b ? -1 : 1))) {
      if (k.startsWith(prefix)) out.set(k, structuredClone(v) as T);
    }
    return out;
  }
}

export interface Clock {
  now(): number;
  setTimeout(fn: () => void, ms: number): unknown;
  clearTimeout(handle: unknown): void;
}

/** Looks globals up at call time so test fake timers apply. */
export const realClock: Clock = {
  now: () => Date.now(),
  setTimeout: (fn, ms) => globalThis.setTimeout(fn, ms),
  clearTimeout: (h) => globalThis.clearTimeout(h as ReturnType<typeof setTimeout>),
};

export function sleep(clock: Clock, ms: number): Promise<void> {
  return new Promise((resolve) => clock.setTimeout(resolve, ms));
}
