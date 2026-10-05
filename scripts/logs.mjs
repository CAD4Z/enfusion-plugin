/**
 * Shows the extension's log from every VS Code window of the latest session that has one, with the
 * folder each window has open: where a question about the running extension starts.
 *
 *   node scripts/logs.mjs        the last 40 lines of each log
 *   node scripts/logs.mjs 200    the last 200 lines
 *
 * VS Code keeps a folder per session in its logs folder and one per window inside that; the
 * extension's output channel goes to window<N>/exthost/<publisher>.<name>/<channel>.log. The folder
 * a window has open is in the workspace storage that the window's extension host log names.
 */

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

const DEFAULT_LINES = 40;

function main() {
  const lines = Number(process.argv[2] ?? DEFAULT_LINES);
  const packageJson = path.join(path.dirname(fileURLToPath(import.meta.url)), '..', 'package.json');
  /** @type {unknown} */
  const manifest = JSON.parse(fs.readFileSync(packageJson, 'utf8'));
  const extension = `${stringField(manifest, 'publisher')}.${stringField(manifest, 'name')}`;
  const userData = userDataFolder();
  const logsRoot = path.join(userData, 'logs');

  if (!fs.existsSync(logsRoot)) {
    process.stderr.write(`no VS Code logs in ${logsRoot}\n`);
    process.exitCode = 1;
    return;
  }

  // Sessions are named by the time they started, so the newest sorts last.
  const sessions = fs.readdirSync(logsRoot).sort().reverse();
  for (const session of sessions) {
    const windows = windowLogsOf(path.join(logsRoot, session), extension);
    if (windows.length === 0) {
      continue;
    }

    process.stdout.write(`VS Code session ${session}\n`);
    for (const { window, log, folder } of windows) {
      process.stdout.write(`\n== ${window}  ${folder ?? '(no folder known)'}\n${log}\n`);
      process.stdout.write(`${tail(log, lines)}\n`);
    }
    return;
  }

  process.stderr.write(`no window of any VS Code session has a log of ${extension}\n`);
  process.exitCode = 1;
}

/**
 * The windows of one session that have a log of the extension, each with the folder it has open.
 *
 * @param {string} session
 * @param {string} extension
 * @returns {{ window: string, log: string, folder: string | undefined }[]}
 */
function windowLogsOf(session, extension) {
  return fs
    .readdirSync(session)
    .filter((entry) => entry.startsWith('window'))
    .flatMap((window) => {
      const channelFolder = path.join(session, window, 'exthost', extension);
      if (!fs.existsSync(channelFolder)) {
        return [];
      }

      const folder = openFolderOf(path.join(session, window, 'exthost', 'exthost.log'));
      return fs
        .readdirSync(channelFolder)
        .filter((file) => file.endsWith('.log'))
        .map((file) => ({ window, log: path.join(channelFolder, file), folder }));
    });
}

/**
 * The folder a window has open, from the workspace storage its extension host log names.
 *
 * @param {string} hostLog
 * @returns {string | undefined}
 */
function openFolderOf(hostLog) {
  if (!fs.existsSync(hostLog)) {
    return undefined;
  }

  const storage = /workspaceStorage[\\/]([0-9a-f]{32})/.exec(fs.readFileSync(hostLog, 'utf8'));
  if (storage === null) {
    return undefined;
  }

  const description = path.join(userDataFolder(), 'User', 'workspaceStorage', storage[1], 'workspace.json');
  if (!fs.existsSync(description)) {
    return undefined;
  }

  /** @type {unknown} */
  const workspace = JSON.parse(fs.readFileSync(description, 'utf8'));
  const uri = stringField(workspace, 'folder') ?? stringField(workspace, 'workspace');
  if (uri === undefined) {
    return undefined;
  }

  return uri.startsWith('file:') ? fileURLToPath(uri) : uri;
}

/**
 * A string field of a parsed JSON object, or nothing.
 *
 * @param {unknown} value
 * @param {string} key
 * @returns {string | undefined}
 */
function stringField(value, key) {
  if (typeof value !== 'object' || value === null) {
    return undefined;
  }

  const field = /** @type {Record<string, unknown>} */ (value)[key];
  return typeof field === 'string' ? field : undefined;
}

/**
 * The last lines of a file.
 *
 * @param {string} file
 * @param {number} count
 * @returns {string}
 */
function tail(file, count) {
  return fs.readFileSync(file, 'utf8').trimEnd().split(/\r?\n/).slice(-count).join('\n');
}

/** Where VS Code keeps its user data on this platform. */
function userDataFolder() {
  if (process.platform === 'win32') {
    return path.join(process.env.APPDATA ?? path.join(os.homedir(), 'AppData', 'Roaming'), 'Code');
  }
  if (process.platform === 'darwin') {
    return path.join(os.homedir(), 'Library', 'Application Support', 'Code');
  }
  return path.join(process.env.XDG_CONFIG_HOME ?? path.join(os.homedir(), '.config'), 'Code');
}

main();
