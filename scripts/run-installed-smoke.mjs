import fs from 'node:fs';
import path from 'node:path';
import process from 'node:process';
import { runTests, runVSCodeCommand } from '@vscode/test-electron';

const VSCODE_VERSION = '1.104.0';

async function main() {
  const vsix = process.argv[2];
  if (vsix === undefined) {
    throw new Error('usage: node scripts/run-installed-smoke.mjs PATH_TO_VSIX');
  }

  const profile = path.resolve('.vscode-test', 'installed-smoke');
  const extensions = path.join(profile, 'extensions');
  const userData = path.join(profile, 'user-data');
  const workspace = path.resolve('out', 'smoke-workspace');
  cleanOwnedDirectory(profile, path.resolve('.vscode-test'));
  cleanOwnedDirectory(workspace, path.resolve('out'));
  fs.mkdirSync(extensions, { recursive: true });
  fs.mkdirSync(userData, { recursive: true });
  fs.mkdirSync(workspace, { recursive: true });

  await runVSCodeCommand(
    [
      `--extensions-dir=${extensions}`,
      `--user-data-dir=${userData}`,
      '--install-extension',
      path.resolve(vsix),
      '--force',
    ],
    {
      version: VSCODE_VERSION,
    },
  );

  const installedName = fs
    .readdirSync(extensions, { withFileTypes: true })
    .find((entry) => entry.isDirectory() && entry.name.startsWith('hurfy.enfusion-plugin-'))?.name;
  if (installedName === undefined) {
    throw new Error('the VSIX install produced no hurfy.enfusion-plugin directory');
  }

  const installed = path.join(extensions, installedName);
  await runTests({
    version: VSCODE_VERSION,
    // The development extension is only the test driver. The extension under test is resolved
    // exclusively from the clean profile where the packaged VSIX was installed above.
    extensionDevelopmentPath: path.resolve('test', 'smoke', 'driver'),
    extensionTestsPath: path.resolve('out', 'smoke', 'activation.js'),
    extensionTestsEnv: {
      EXPECTED_EXTENSION_PATH: installed,
      // Some agent/CI hosts set this for their own Electron wrapper. VS Code itself must not.
      ELECTRON_RUN_AS_NODE: undefined,
    },
    launchArgs: [
      workspace,
      `--extensions-dir=${extensions}`,
      `--user-data-dir=${userData}`,
      '--disable-workspace-trust',
    ],
  });
}

/** @param {string} target @param {string} owner */
function cleanOwnedDirectory(target, owner) {
  if (path.dirname(target) !== owner) {
    throw new Error(`refusing to clean non-child path: ${target}`);
  }
  fs.rmSync(target, { recursive: true, force: true });
}

main().catch((error) => {
  process.stderr.write(`${error instanceof Error ? error.stack : String(error)}\n`);
  process.exitCode = 1;
});
