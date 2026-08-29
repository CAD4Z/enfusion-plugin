import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  type ScriptDebugSaid,
  scriptDebugNoteOf,
  scriptDebugRead,
  scriptDebugSaidOf,
} from './scriptDebug';

/**
 * The frames a real `DayZDiag_x64.exe` was watched sending, built the way it builds them. The
 * numbers in the module frame below are the ones it actually announced for the stock `1_Core`.
 */
function frame(command: number, length: number, body: Uint8Array): Uint8Array {
  const bytes = new Uint8Array(8 + body.byteLength);
  const view = new DataView(bytes.buffer);
  view.setInt32(0, command, true);
  view.setInt32(4, length, true);
  bytes.set(body, 8);

  return bytes;
}

function bytesOf(build: (view: DataView, bytes: Uint8Array) => void, size: number): Uint8Array {
  const bytes = new Uint8Array(size);
  build(new DataView(bytes.buffer), bytes);

  return bytes;
}

function attach(pid: number): Uint8Array {
  return frame(1, 4, bytesOf((view) => view.setUint32(0, pid, true), 4));
}

function log(text: string): Uint8Array {
  const body = new TextEncoder().encode(text);

  return frame(
    20,
    0,
    bytesOf(
      (view, bytes) => {
        view.setInt32(0, body.byteLength, true);
        bytes.set(body, 4);
      },
      4 + body.byteLength,
    ),
  );
}

/** What the game announces about a script module the moment it connects, before anything else. */
function moduleOf(files: readonly string[], lines: number, marked: number): Uint8Array {
  const names = files.map((name) => new TextEncoder().encode(name));
  const size =
    8 + 12 + names.reduce((total, name) => total + 4 + name.byteLength, 0) + lines * 8 + marked * 4;

  return frame(
    4,
    0,
    bytesOf((view, bytes) => {
      view.setBigUint64(0, 0x21157850170n, true);
      view.setInt32(8, files.length, true);
      view.setInt32(12, lines, true);
      view.setInt32(16, marked, true);
      let at = 20;

      for (const name of names) {
        view.setInt32(at, name.byteLength, true);
        bytes.set(name, at + 4);
        at += 4 + name.byteLength;
      }
    }, size),
  );
}

function forget(): Uint8Array {
  return frame(5, 0, bytesOf((view) => view.setBigUint64(0, 0x21157850170n, true), 8));
}

/** The one frame that fills its length in honestly, sent when the game has stopped somewhere. */
function callstack(length: number): Uint8Array {
  return frame(6, length, new Uint8Array(length));
}

function variables(names: readonly string[]): Uint8Array {
  const each = names.map((name) => new TextEncoder().encode(name));
  const size = 4 + each.reduce((total, name) => total + 12 + name.byteLength, 0);

  return frame(
    7,
    0,
    bytesOf((view, bytes) => {
      view.setInt32(0, names.length, true);
      let at = 4;

      for (const name of each) {
        view.setUint16(at + 8, name.byteLength, true);
        bytes.set(name, at + 12);
        at += 12 + name.byteLength;
      }
    }, size),
  );
}

function joined(...parts: readonly Uint8Array[]): Uint8Array {
  const all = new Uint8Array(parts.reduce((total, part) => total + part.byteLength, 0));
  let at = 0;

  for (const part of parts) {
    all.set(part, at);
    at += part.byteLength;
  }

  return all;
}

test('the connection says which process it is, and then the log comes down it', () => {
  const read = scriptDebugRead(joined(attach(4242), log('SCRIPT       : hello\n')));

  assert.deepEqual(read.said, [
    { kind: 'attached', pid: 4242 },
    { kind: 'log', text: 'SCRIPT       : hello\n' },
  ] satisfies ScriptDebugSaid[]);
  assert.equal(read.rest.byteLength, 0);
});

/**
 * The mistake the first draft made, and the reason this is checked against real bytes rather than
 * against a reading of the disassembly alone: a game announces every module it has loaded the
 * moment it connects, and a reader that does not measure those never reaches a log line at all.
 */
test('the modules a game announces on connecting are measured and passed over', () => {
  const read = scriptDebugRead(
    joined(
      attach(9),
      moduleOf(['scripts/1_Core/proto\\endebug.c', 'scripts/1_Core/param.c'], 3644, 0),
      forget(),
      moduleOf([], 0, 0),
      log('after the modules\n'),
    ),
  );

  assert.deepEqual(read.said, [
    { kind: 'attached', pid: 9 },
    { kind: 'log', text: 'after the modules\n' },
  ] satisfies ScriptDebugSaid[]);
  assert.equal(read.rest.byteLength, 0);
});

/** The variables of a stopped frame are measured the same way, and are nobody's business here. */
test('a dump of variables is passed over without losing the place in the stream', () => {
  const read = scriptDebugRead(joined(variables(['player', 'm_Health']), log('still here\n')));

  assert.deepEqual(read.said, [{ kind: 'log', text: 'still here\n' }] satisfies ScriptDebugSaid[]);
  assert.equal(read.rest.byteLength, 0);
});

/**
 * A callstack means the game has stopped and is waiting to be told to go on. Saying so is the
 * whole job: what releases it is the socket being closed, and that is the holder's to do.
 */
test('a callstack is reported as halted, because the game is waiting on us to close', () => {
  const read = scriptDebugRead(joined(log('before\n'), callstack(168)));

  assert.deepEqual(read.said, [
    { kind: 'log', text: 'before\n' },
    { kind: 'halted' },
  ] satisfies ScriptDebugSaid[]);
});

/**
 * The one thing a reader of a socket has to get right. A frame arrives in as many pieces as the
 * network felt like, and every prefix of one is a prefix rather than a failure: nothing is said
 * until the whole of it is there, and what is left over is handed back to be read again.
 */
test('a frame split anywhere is one frame once the rest of it has arrived', () => {
  const whole = joined(
    attach(7),
    moduleOf(['scripts/1_Core/param.c'], 9, 2),
    log('one\n'),
    log('two\n'),
  );

  for (let at = 0; at <= whole.byteLength; at++) {
    const first = scriptDebugRead(whole.subarray(0, at));
    const second = scriptDebugRead(joined(first.rest, whole.subarray(at)));

    assert.deepEqual(
      [...first.said, ...second.said],
      [
        { kind: 'attached', pid: 7 },
        { kind: 'log', text: 'one\n' },
        { kind: 'log', text: 'two\n' },
      ] satisfies ScriptDebugSaid[],
      `split at ${at}`,
    );
    assert.equal(second.rest.byteLength, 0, `split at ${at}`);
  }
});

/**
 * Everything before the frame we cannot measure is still worth saying; everything after it is not
 * ours to find. The caller closes the socket on this, which is what the game reads as the debugger
 * having gone — it carries on running and opens another connection.
 */
test('a frame this does not read ends the reading rather than being skipped', () => {
  const read = scriptDebugRead(joined(log('before\n'), frame(19, 0, new Uint8Array(4))));

  assert.deepEqual(read.said, [
    { kind: 'log', text: 'before\n' },
    { kind: 'unreadable', command: 19 },
  ] satisfies ScriptDebugSaid[]);
  assert.equal(read.rest.byteLength, 0);
});

/** A count that could not be one is the same thing: the place in the stream has been lost. */
test('a log frame that says it is longer than any log ends the reading', () => {
  const bytes = frame(20, 0, bytesOf((view) => view.setInt32(0, 0x7fffffff, true), 4));

  assert.deepEqual(scriptDebugRead(bytes).said, [
    { kind: 'unreadable', command: 20 },
  ] satisfies ScriptDebugSaid[]);
});

/** And a known command whose header says something the engine never says is not that command. */
test('a frame whose length is not the one that command carries is not read as that command', () => {
  assert.deepEqual(scriptDebugRead(frame(1, 8, new Uint8Array(8))).said, [
    { kind: 'unreadable', command: 1 },
  ] satisfies ScriptDebugSaid[]);
});

/**
 * One write to the log is not one line. The engine ends every write with a newline of its own, and
 * a stack trace comes down as a dozen lines in a single write — so each of them gets the prefix,
 * and the empty piece the trailing newline leaves behind is not a line at all.
 */
test('every line of one write carries the prefix, and the trailing newline makes no line', () => {
  assert.deepEqual(scriptDebugSaidOf('client', 'first\r\nsecond\n'), [
    `${GREEN}[CLIENT]${OFF} first`,
    `${GREEN}[CLIENT]${OFF} second`,
  ]);
});

/**
 * And what a write really ends with, watched on the wire: a null rather than the newline the
 * engine's own code says it puts back. A console that printed it would end every line with a
 * character that is not one.
 */
test('the null a write really ends with is not printed, and the server is red', () => {
  assert.deepEqual(scriptDebugSaidOf('server', 'SCRIPT       : CAD4Z probe alive\u0000'), [
    `${RED}[SERVER]${OFF} SCRIPT       : CAD4Z probe alive`,
  ]);
});

test('a note about the connection is dimmed, so it does not read as something a mod printed', () => {
  assert.equal(
    scriptDebugNoteOf('client', 'attached'),
    `${GREEN}[CLIENT]${OFF} ${DIM}attached${OFF}`,
  );
});

/** The escapes VS Code turns into colour, spelled out rather than pasted into an expectation. */
const GREEN = '\u001b[32m';
const RED = '\u001b[31m';
const OFF = '\u001b[0m';
const DIM = '\u001b[2m';
