import type { EddsConversion, EddsFailureCategory } from './edds';

export const BATCH_PROTOCOL_VERSION = 1;
export const BATCH_EVENT_MAX_BYTES = 256 * 1024;

/**
 * The documented hard ceiling on one batch, well above the hundred-item selection the feature
 * must support. It lives here so the plan refuses an oversized selection before an editor offers
 * to run it, and the process boundary refuses the same count before it spawns anything.
 */
export const BATCH_MAX_JOBS = 256;

export type TextureBatchEvent =
  | { readonly protocolVersion: 1; readonly kind: 'batch-started'; readonly jobCount: number }
  | { readonly protocolVersion: 1; readonly kind: 'progress'; readonly id: string; readonly progress: number }
  | {
      readonly protocolVersion: 1;
      readonly kind: 'diagnostic';
      readonly id: string;
      readonly category: EddsFailureCategory;
      readonly code: string;
      readonly message: string;
    }
  | {
      readonly protocolVersion: 1;
      readonly kind: 'result';
      readonly id: string;
      readonly status: 'Converted';
      readonly conversion: EddsConversion;
    }
  | {
      readonly protocolVersion: 1;
      readonly kind: 'result';
      readonly id: string;
      readonly status: 'Failed' | 'Cancelled';
      readonly reason: string;
      readonly retryable: boolean;
    }
  | {
      readonly protocolVersion: 1;
      readonly kind: 'complete';
      readonly converted: number;
      readonly failed: number;
      readonly cancelled: number;
    };

/** Incremental line reader for stdout; transport chunk boundaries have no protocol meaning. */
export class TextureBatchProtocolReader {
  private pending = '';

  push(chunk: string): TextureBatchEvent[] {
    this.pending += chunk;
    const events: TextureBatchEvent[] = [];
    for (;;) {
      const newline = this.pending.indexOf('\n');
      if (newline < 0) break;
      const line = this.pending.slice(0, newline).replace(/\r$/, '');
      this.pending = this.pending.slice(newline + 1);
      if (line.length > 0) {
        assertLineSize(line);
        events.push(eventOf(line));
      }
    }
    this.assertBounded();
    return events;
  }

  finish(): TextureBatchEvent[] {
    if (this.pending.length === 0) return [];
    this.assertBounded();
    const line = this.pending.replace(/\r$/, '');
    this.pending = '';
    return line.length === 0 ? [] : [eventOf(line)];
  }

  private assertBounded(): void {
    assertLineSize(this.pending);
  }
}

function eventOf(line: string): TextureBatchEvent {
  let value: unknown;
  try {
    value = JSON.parse(line);
  } catch {
    throw new Error('The native batch stream contains malformed JSON.');
  }
  if (!isRecord(value)) throw new Error('The native batch event must be a JSON object.');
  if (value.protocolVersion !== BATCH_PROTOCOL_VERSION) {
    throw new Error(`The native batch event uses incompatible protocol version ${String(value.protocolVersion)}.`);
  }
  switch (value.kind) {
    case 'batch-started':
      requireCount(value.jobCount, 'jobCount');
      return value as TextureBatchEvent;
    case 'progress':
      requireId(value.id);
      if (typeof value.progress !== 'number' || value.progress < 0 || value.progress > 1) {
        throw new Error('A native batch progress event has an invalid progress value.');
      }
      return value as TextureBatchEvent;
    case 'diagnostic':
      requireId(value.id);
      if (!isFailureCategory(value.category)) {
        throw new Error('A native batch diagnostic has an invalid category.');
      }
      requireText(value.code, 'code');
      requireText(value.message, 'message');
      return value as TextureBatchEvent;
    case 'result':
      requireId(value.id);
      if (value.status === 'Converted') {
        if (!isConversion(value.conversion)) {
          throw new Error('A converted native batch result has no valid conversion facts.');
        }
      } else if (value.status === 'Failed' || value.status === 'Cancelled') {
        requireText(value.reason, 'reason');
        if (typeof value.retryable !== 'boolean') {
          throw new Error('A failed native batch result has no retryability value.');
        }
      } else {
        throw new Error('A native batch result has an unknown status.');
      }
      return value as TextureBatchEvent;
    case 'complete':
      requireCount(value.converted, 'converted');
      requireCount(value.failed, 'failed');
      requireCount(value.cancelled, 'cancelled');
      return value as TextureBatchEvent;
    default:
      throw new Error(`The native converter emitted an unknown batch event: ${String(value.kind)}.`);
  }
}

function assertLineSize(value: string): void {
  if (new TextEncoder().encode(value).byteLength > BATCH_EVENT_MAX_BYTES) {
    throw new Error(`A native batch event exceeds ${BATCH_EVENT_MAX_BYTES} bytes.`);
  }
}

function isFailureCategory(value: unknown): value is EddsFailureCategory {
  return value === 'invalid-invocation' || value === 'invalid-input' ||
    value === 'unsupported-format' || value === 'cancelled' || value === 'internal-failure';
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === 'object' && value !== null && !Array.isArray(value);
}

function requireId(value: unknown): asserts value is string {
  requireText(value, 'id');
}

function requireText(value: unknown, field: string): asserts value is string {
  if (typeof value !== 'string' || value.length === 0) {
    throw new Error(`A native batch event has an invalid ${field}.`);
  }
}

function requireCount(value: unknown, field: string): asserts value is number {
  if (!Number.isSafeInteger(value) || (value as number) < 0) {
    throw new Error(`A native batch event has an invalid ${field}.`);
  }
}

function isConversion(value: unknown): value is EddsConversion {
  return isRecord(value) && Number.isSafeInteger(value.width) && (value.width as number) > 0 &&
    Number.isSafeInteger(value.height) && (value.height as number) > 0 &&
    Number.isSafeInteger(value.mipCount) && (value.mipCount as number) > 0 &&
    typeof value.pixelFormat === 'string' && typeof value.registered === 'boolean';
}
