/**
 * Answering the debugger socket the game dials out on, so that its script log lands in the console
 * the launch was started from.
 *
 * The protocol and the reasons behind every strictness here are in `src/mods/scriptDebug.ts`. This
 * is the socket around it: one listener per role, opened before the game is started and named on
 * its command line, and closed when the launch is over.
 *
 * Bound on `127.0.0.1` and on port zero — the port is whatever the machine had free, and the game
 * is told which one it was. A fixed port is a port two launches fight over and a port something
 * else on the machine may already hold; there is no reason to pick one when the number only has to
 * survive as far as a command line.
 *
 * A connection is not a launch. The game connects a moment after the script VM starts ticking, and
 * connects again a few seconds after anything drops it, so the listener stays open and takes them
 * as they come rather than expecting one.
 */

import { type Server, type Socket, createServer } from 'node:net';
import type { LaunchRole } from '../mods/launch';
import {
  FORCED_SCRIPT_DEBUG_PORT,
  type ScriptDebugSaid,
  scriptDebugRead,
} from '../mods/scriptDebug';

/** A listener that is up, and the port to put on the command line of the game that dials it. */
export interface ScriptDebugPort {
  readonly role: LaunchRole;
  readonly port: number;
  /** Drops whatever is connected and stops listening. Doing it twice is not an error. */
  close(): void;
}

/** What the holder of a listener is told, which is everything and in the order it happened. */
export interface ScriptDebugHandler {
  /** One thing the game said on the `SCRIPT` channel, newline and all. */
  said(role: LaunchRole, text: string): void;
  /** The connection itself: attached, dropped, given up on. */
  note(role: LaunchRole, note: string): void;
}

/**
 * A listener for one role, up and bound, or the reason there is none.
 *
 * A port that could not be opened is not worth failing a launch over: the game runs perfectly well
 * with nobody on the other end of its debugger socket, and what is lost is the console echo rather
 * than the launch. So this answers with a sentence instead of throwing, and the caller launches
 * anyway and says what will be missing.
 */
export async function openScriptDebugPort(
  role: LaunchRole,
  handler: ScriptDebugHandler,
): Promise<ScriptDebugPort | string> {
  const server = createServer();
  const sockets = new Set<Socket>();
  // Everything a closing listener drops is expected rather than worth a line, and the console it
  // would say the line into belongs to a launch that is already over.
  let listening = true;

  server.on('connection', (socket) => {
    sockets.add(socket);
    read(socket, role, handler, () => listening);
    socket.on('close', () => sockets.delete(socket));
  });

  // The client is met wherever the machine had a socket free and told so on its command line; the
  // server is met on the port it is going to dial whatever anybody tells it.
  const forced = FORCED_SCRIPT_DEBUG_PORT[role];
  const port = await bound(server, forced ?? ANY);
  if (typeof port === 'string') {
    // Worth naming what else could be holding it. The two ports a role is forced onto are the
    // Workbench's own, so the usual answer is a Workbench that is open — and the second usual one
    // is a launch of ours that did not let go.
    return (
      `the ${role}'s script log will not be echoed: ${port}` +
      (forced === undefined
        ? ''
        : `. A ${role} always dials ${forced}, so something else is on it — a Workbench, or a ` +
          'launch that has not let go of it.')
    );
  }

  return {
    role,
    port,
    close: () => {
      listening = false;

      for (const socket of sockets) {
        socket.destroy();
      }
      sockets.clear();
      server.close();
    },
  };
}

/** The port asked for, or the one the machine gave for `ANY` — or what it said instead. */
async function bound(server: Server, wanted: number): Promise<number | string> {
  return new Promise<number | string>((resolve) => {
    server.once('error', (error: Error) => {
      resolve(error.message);
    });
    server.listen({ host: LOOPBACK, port: wanted }, () => {
      const address = server.address();

      resolve(
        address === null || typeof address === 'string'
          ? 'the machine gave the listener no port'
          : address.port,
      );
    });
  });
}

/**
 * One connection, read to its end.
 *
 * Everything the socket delivers is appended to whatever was left over from the last read, because
 * a frame arrives in as many pieces as the network felt like. What comes out is handed on as it
 * is; the one thing decided here is what to do about a frame we cannot measure, and the answer is
 * to close the socket. That is the safe end of every state the game can be in — including halted,
 * waiting to be told to carry on — and it costs only the seconds until it connects again.
 */
function read(
  socket: Socket,
  role: LaunchRole,
  handler: ScriptDebugHandler,
  listening: () => boolean,
): void {
  let rest: Uint8Array = new Uint8Array(0);

  socket.on('data', (chunk: Buffer) => {
    const read = scriptDebugRead(joined(rest, chunk));
    rest = read.rest;

    for (const said of read.said) {
      if (!tell(said, role, handler)) {
        rest = new Uint8Array(0);
        socket.destroy();
        return;
      }
    }
  });

  // A game that is gone and a socket that broke are the same thing from here: the launch says when
  // it is over, and until it does the listener is waiting for the next connection either way.
  socket.on('error', () => {
    socket.destroy();
  });
  socket.on('close', () => {
    if (listening()) {
      handler.note(role, 'script debugger detached');
    }
  });
}

/** Says the one thing, and answers whether the socket is still worth reading. */
function tell(said: ScriptDebugSaid, role: LaunchRole, handler: ScriptDebugHandler): boolean {
  switch (said.kind) {
    case 'attached':
      handler.note(
        role,
        `script debugger attached to pid ${said.pid} — anything logged before this is in the ` +
          'profile’s script log only',
      );
      return true;
    case 'log':
      handler.said(role, said.text);
      return true;
    case 'halted':
      // Nothing here ever asked it to stop, so this is the game having found a breakpoint left
      // over from somewhere else. Closing is what lets it go on, and is all we can offer it.
      handler.note(
        role,
        'the game stopped at a breakpoint, which nothing here can step; the connection was ' +
          'dropped to let it carry on',
      );
      return false;
    case 'unreadable':
      handler.note(
        role,
        `the script debugger sent frame ${said.command}, which this does not read; the connection ` +
          'was dropped and the game will open another',
      );
      return false;
  }
}

function joined(rest: Uint8Array, chunk: Uint8Array): Uint8Array {
  if (rest.byteLength === 0) {
    return chunk;
  }

  const both = new Uint8Array(rest.byteLength + chunk.byteLength);
  both.set(rest, 0);
  both.set(chunk, rest.byteLength);

  return both;
}

const LOOPBACK = '127.0.0.1';

/** Port zero: the machine picks, and it picks one that is free. */
const ANY = 0;
