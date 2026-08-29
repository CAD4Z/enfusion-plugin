/**
 * The Enfusion script debugger, as much of it as reading a log takes.
 *
 * The diag build carries a debugger of its own, and it is the game that reaches out: every tick of
 * the script VM it tries to open a TCP connection to `-debugger=<host>` on `-debuggerPort=<n>`,
 * and the Workbench is what usually answers. Nothing stops us answering instead — and what comes
 * down that socket is the `SCRIPT` log channel, live, line by line: every `Print`, every `Error`,
 * every stack trace the VM writes. It is the same text that lands in `script_<stamp>.log`, which
 * is the point: this is not a second, lesser copy of the log but the very stream that log is
 * written from.
 *
 * The protocol is documented nowhere. It was read off `DayZDiag_x64.exe` and then checked against
 * the bytes a running one actually sent, which is worth saying because the reading alone got it
 * wrong: the game announces every loaded script module the moment it connects, and a reader that
 * only knew the two frames it wanted spent the whole launch hanging up and being dialled again.
 *
 *     frame     := command:i32le  length:i32le  body
 *     ATTACH    := 1   length 4   pid:u32le
 *     MODULE    := 4   length 0   module:u64le  files:i32le  lines:i32le  marked:i32le
 *                                 files  × (length:i32le  name:length bytes)
 *                                 lines  × 8 bytes
 *                                 marked × 4 bytes
 *     FORGET    := 5   length 0   module:u64le
 *     CALLSTACK := 6   length n   n bytes
 *     VARIABLES := 7   length 0   count:i32le  count × (12 bytes, name length at +8 as u16le,
 *                                 then that many bytes)
 *     LOG       := 20  length 0   count:i32le  text:count bytes, ending in a null
 *
 * `length` is only what the engine's own reader skips a command it does not know by, and the
 * engine mostly writes zero into it and says how long the frame is again inside the body. So every
 * frame is measured by its own shape rather than by that number, and only the callstack — which
 * fills it in honestly — is taken at its word.
 *
 * Of those, three are worth anything here. `ATTACH` says which process is talking, `LOG` is the
 * whole point, and `CALLSTACK` is the one to be afraid of: the game sends it when it has stopped
 * at a breakpoint, and then waits to be told to carry on. We set no breakpoints, so it should
 * never arrive — and if it does, the socket is closed, which the game reads as the debugger having
 * gone and is its way out. The rest are measured, skipped, and never spoken of.
 *
 * Anything that is not one of the six ends the reading rather than being skipped: a frame we
 * cannot measure is a stream we can no longer find our place in, and printing the wreckage of a
 * mis-parse into a developer's console is worse than saying we stopped. Closing is safe there too.
 *
 * What this does not carry is the beginning. The game connects on the first tick of the script VM,
 * and the modules have already been compiled and logged by then — `Module: GameLib; loaded 18x
 * files` and whatever a mod prints from a static constructor are in the file and not in the
 * stream. See `docs/adr/0004-the-script-log-comes-off-the-debugger-port.md`.
 */

import type { LaunchRole } from './launch';

/** The host the game is told to reach: ours, and named as a number so no resolver is involved. */
export const SCRIPT_DEBUG_HOST = '127.0.0.1';

/**
 * The port a role will dial whatever it is told, where the engine insists on one.
 *
 * `-debuggerPort` is only half honoured, and finding that out is most of what this file cost. The
 * engine reads the argument at startup and then, once it knows which process it is, writes over
 * it: a server is forced to 1001 and a `-client2` to 1002, and only a plain client keeps the
 * number it was given. It is why the Workbench debugs both sides by picking a side rather than a
 * port — 1000 and 1001 are simply where the two of them are.
 *
 * So the client is told a port of our choosing and the server is met on the one it is going to use
 * anyway. A role forced to a port is also a role that cannot be launched twice on one machine, and
 * a Workbench already sitting on 1001 is a listener of ours that will not bind — which is a
 * sentence in the console rather than a failed launch.
 */
export const FORCED_SCRIPT_DEBUG_PORT: Readonly<Record<LaunchRole, number | undefined>> = {
  client: undefined,
  server: 1001,
  client2: 1002,
};

/** Every frame the game was seen to send. Anything else ends the reading. */
const COMMAND = {
  attach: 1,
  module: 4,
  forget: 5,
  callstack: 6,
  variables: 7,
  log: 20,
} as const;

const HEADER = 8;

/**
 * What a count in a frame may be before the frame is treated as wreckage rather than as data.
 *
 * Generous rather than tight, because the real numbers are larger than a guess would have been:
 * the `World` module of a stock DayZ announces 2130 files and 123991 lines in one frame. These are
 * here to keep a stream we have lost our place in from asking for an absurd allocation, not to
 * second-guess the engine.
 */
const MOST = {
  files: 100_000,
  lines: 5_000_000,
  text: 1024 * 1024,
} as const;

/** Something the game said down the debugger socket, of the things worth hearing. */
export type ScriptDebugSaid =
  /** The connection, and which process it belongs to. */
  | { readonly kind: 'attached'; readonly pid: number }
  /** One write to the `SCRIPT` channel, exactly as the engine formatted it, newline and all. */
  | { readonly kind: 'log'; readonly text: string }
  /**
   * The game has stopped somewhere and is waiting to be told to go on. Whoever is holding the
   * socket closes it, which is what lets it go on.
   */
  | { readonly kind: 'halted' }
  /**
   * A frame this does not know how to measure. The reading is over: the socket is closed, and the
   * game carries on and connects again.
   */
  | { readonly kind: 'unreadable'; readonly command: number };

/** What one read of the socket amounted to, and what has to be read again with more after it. */
export interface ScriptDebugRead {
  readonly said: readonly ScriptDebugSaid[];
  /** The bytes that were not yet a whole frame. Empty once something unreadable has been found. */
  readonly rest: Uint8Array;
}

/**
 * Everything whole in the bytes so far, and the tail that is not whole yet.
 *
 * Called again with `rest` and whatever the socket delivered after it, which is what makes a frame
 * that arrived in three pieces one frame rather than three failures. Nothing is kept between
 * calls: what a reader has to remember is the tail, and the tail is handed back.
 */
export function scriptDebugRead(bytes: Uint8Array): ScriptDebugRead {
  const said: ScriptDebugSaid[] = [];
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let at = 0;

  while (bytes.byteLength - at >= HEADER) {
    const span = spanAt(view, bytes.byteLength, at);

    if (span === 'short') {
      break;
    }

    if (span === 'unreadable') {
      return {
        said: [...said, { kind: 'unreadable', command: view.getInt32(at, true) }],
        rest: empty(),
      };
    }

    const heard = heardAt(bytes, view, at);
    if (heard !== undefined) {
      said.push(heard);
    }

    at += span;
  }

  return { said, rest: bytes.subarray(at) };
}

/**
 * How long the frame at `at` is, header and all — or that there is not enough to tell yet, or that
 * it is not a frame this knows.
 *
 * Every frame is measured rather than trusted, which for the two that carry a list means walking
 * it. That is the price of a protocol whose length field mostly says zero.
 */
function spanAt(view: DataView, end: number, at: number): number | 'short' | 'unreadable' {
  const command = view.getInt32(at, true);
  const length = view.getInt32(at + 4, true);
  const body = at + HEADER;

  switch (command) {
    case COMMAND.attach:
      return length === 4 ? sized(end, body, 4) : 'unreadable';
    case COMMAND.forget:
      return length === 0 ? sized(end, body, 8) : 'unreadable';
    case COMMAND.callstack:
      // The one frame that fills its length in, and the one we only ever want the size of.
      return length < 0 ? 'unreadable' : sized(end, body, length);
    case COMMAND.module:
      return length === 0 ? moduleSpanAt(view, end, body) : 'unreadable';
    case COMMAND.variables:
      return length === 0 ? variablesSpanAt(view, end, body) : 'unreadable';
    case COMMAND.log:
      return length === 0 ? countedSpanAt(view, end, body, MOST.text) : 'unreadable';
    default:
      return 'unreadable';
  }
}

/** The module the game announces on connecting: its files by name, and a word for every line. */
function moduleSpanAt(view: DataView, end: number, body: number): number | 'short' | 'unreadable' {
  if (end - body < 8 + 12) {
    return 'short';
  }

  const files = view.getInt32(body + 8, true);
  const lines = view.getInt32(body + 12, true);
  const marked = view.getInt32(body + 16, true);

  if (
    files < 0 ||
    files > MOST.files ||
    lines < 0 ||
    lines > MOST.lines ||
    marked < 0 ||
    marked > MOST.lines
  ) {
    return 'unreadable';
  }

  let at = body + 8 + 12;

  for (let file = 0; file < files; file++) {
    const span = countedSpanAt(view, end, at, MOST.text);
    if (typeof span !== 'number') {
      return span;
    }
    at += span - HEADER;
  }

  at += lines * 8 + marked * 4;

  return at > end ? 'short' : at - (body - HEADER);
}

/** The variables of a stopped frame, which only ever arrive after we have asked for them. */
function variablesSpanAt(
  view: DataView,
  end: number,
  body: number,
): number | 'short' | 'unreadable' {
  if (end - body < 4) {
    return 'short';
  }

  const count = view.getInt32(body, true);
  if (count < 0 || count > MOST.files) {
    return 'unreadable';
  }

  let at = body + 4;

  for (let one = 0; one < count; one++) {
    if (end - at < VARIABLE) {
      return 'short';
    }

    at += VARIABLE + view.getUint16(at + 8, true);
  }

  return at > end ? 'short' : at - (body - HEADER);
}

/** The fixed part of one variable, with the length of its name eight bytes into it. */
const VARIABLE = 12;

/** A count and then that many bytes, which is how the engine writes both text and file names. */
function countedSpanAt(
  view: DataView,
  end: number,
  body: number,
  most: number,
): number | 'short' | 'unreadable' {
  if (end - body < 4) {
    return 'short';
  }

  const count = view.getInt32(body, true);
  if (count < 0 || count > most) {
    return 'unreadable';
  }

  return sized(end, body, 4 + count);
}

function sized(end: number, body: number, length: number): number | 'short' {
  return end - body < length ? 'short' : HEADER + length;
}

/** What the frame at `at` is worth saying, of the frames that are worth anything. */
function heardAt(bytes: Uint8Array, view: DataView, at: number): ScriptDebugSaid | undefined {
  switch (view.getInt32(at, true)) {
    case COMMAND.attach:
      return { kind: 'attached', pid: view.getUint32(at + HEADER, true) };
    case COMMAND.log: {
      const from = at + HEADER + 4;

      return { kind: 'log', text: TEXT.decode(bytes.subarray(from, from + view.getInt32(at + HEADER, true))) };
    }
    case COMMAND.callstack:
      return { kind: 'halted' };
    default:
      return undefined;
  }
}

/** Lenient on purpose: a log line is not worth losing over one byte the engine wrote in ANSI. */
const TEXT = new TextDecoder('utf-8', { fatal: false });

function empty(): Uint8Array {
  return new Uint8Array(0);
}

/**
 * The prefix each side's lines carry, and the colour it carries it in.
 *
 * Green for the client and red for the server because the two are told apart at a glance or not at
 * all: a launch that puts up both interleaves them in the one console, and a developer reading it
 * is looking for which machine said a thing rather than reading it top to bottom. Colour is the
 * part of that which survives being skimmed.
 *
 * VS Code renders ANSI in the `output` of a debug adapter's output events, so this is a string and
 * needs nothing else — beyond the adapter saying `supportsANSIStyling` when it is asked what it
 * can do.
 */
const STYLE: Readonly<Record<LaunchRole, string>> = {
  client: '\u001b[32m',
  server: '\u001b[31m',
  // Cyan for the second client: it has to be told from the first at a glance, and the two
  // colours already spoken for mean the other side of the launch and something that failed.
  client2: '\u001b[36m',
};

const PLAIN = '\u001b[0m';

/** What a note about the connection is dimmed with, so it reads as ours rather than as a mod's. */
const FAINT = '\u001b[2m';

const LABEL: Readonly<Record<LaunchRole, string>> = {
  client: '[CLIENT]',
  server: '[SERVER]',
  client2: '[CLIENT2]',
};

/**
 * What the console shows for one thing the game said: every line of it under the same prefix.
 *
 * One write to the log is not one line — a stack trace arrives as a dozen at once — so the text is
 * split rather than prefixed whole, and the empty pieces the split leaves behind are dropped. A
 * write that is nothing but a line ending says nothing and shows nothing.
 *
 * The nulls are what a write actually ends with. The engine formats a line, puts a newline on the
 * end of it, writes a null over that newline while it hands the buffer to the file, and puts the
 * newline back — and what goes down the socket, watched rather than reasoned about, is the version
 * with the null. A console printing that unfiltered ends every line with a character that is not
 * one, so they are taken out wherever they are rather than only at the end.
 *
 * Only the prefix is coloured. The line itself is left as the engine wrote it, because a script
 * error is hard enough to read without the console having an opinion about it.
 */
export function scriptDebugSaidOf(role: LaunchRole, text: string): string[] {
  return text
    .replace(/\0/g, '')
    .split('\n')
    .map((line) => line.replace(/\r$/, ''))
    .filter((line) => line !== '')
    .map((line) => `${STYLE[role]}${LABEL[role]}${PLAIN} ${line}`);
}

/**
 * What the console shows about the connection itself rather than about the game: attached,
 * dropped, given up on. Prefixed the same way and dimmed, so that it reads as the console talking
 * rather than as something a mod printed.
 */
export function scriptDebugNoteOf(role: LaunchRole, note: string): string {
  return `${STYLE[role]}${LABEL[role]}${PLAIN} ${FAINT}${note}${PLAIN}`;
}
