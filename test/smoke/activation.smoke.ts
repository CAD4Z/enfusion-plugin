import { execFile } from 'node:child_process';
import assert from 'node:assert/strict';
import path from 'node:path';
import { promisify } from 'node:util';
import * as vscode from 'vscode';

const executeFile = promisify(execFile);

/** Runs inside VS Code against the directory installed from the packaged VSIX. */
export async function run(): Promise<void> {
  const extension = vscode.extensions.getExtension('hurfy.enfusion-plugin');
  assert.ok(extension, 'the clean profile did not discover hurfy.enfusion-plugin');
  assert.equal(
    path.resolve(extension.extensionPath),
    path.resolve(requiredEnvironment('EXPECTED_EXTENSION_PATH')),
    'the smoke did not activate the installed VSIX directory',
  );

  await extension.activate();
  const commands = await vscode.commands.getCommands(true);
  assert.ok(commands.includes('enfusion.edds.openPreview'), 'the EDDS preview command is not registered');

  const workspace = vscode.workspace.workspaceFolders?.[0];
  assert.ok(workspace, 'the smoke has no local workspace for its owned fixture');
  const fixture = vscode.Uri.joinPath(workspace.uri, 'activation-smoke.edds');
  await vscode.workspace.fs.writeFile(fixture, copyFixture());
  await vscode.commands.executeCommand('enfusion.edds.openPreview', fixture);
  await eventually(() =>
    vscode.window.tabGroups.all.some((group) =>
      group.tabs.some(
        (tab) =>
          tab.input instanceof vscode.TabInputCustom &&
          tab.input.viewType === 'enfusion.eddsPreview' &&
          tab.input.uri.toString() === fixture.toString(),
      ),
    ),
  );

  const executable = path.join(
    extension.extensionPath,
    'dist',
    'native',
    'win32-x64',
    'edds-convert.exe',
  );
  const result = await executeFile(executable, ['protocol', '--machine'], {
    encoding: 'utf8',
    shell: false,
    windowsHide: true,
  });
  const protocol: unknown = JSON.parse(result.stdout);
  assert.deepEqual(protocol, {
    protocolVersion: 1,
    kind: 'protocol',
    toolVersion: '0.1.0',
    commands: ['inspect', 'preview'],
  });
}

function copyFixture(): Uint8Array {
  const bytes = new Uint8Array(140);
  const words = new DataView(bytes.buffer);
  putText(bytes, 0, 'DDS ');
  words.setUint32(4, 124, true);
  words.setUint32(8, 0x0002100f, true);
  words.setUint32(12, 1, true);
  words.setUint32(16, 1, true);
  words.setUint32(20, 4, true);
  words.setUint32(28, 1, true);
  putText(bytes, 36, 'ENF1');
  words.setUint32(76, 32, true);
  words.setUint32(80, 0x41, true);
  words.setUint32(88, 32, true);
  words.setUint32(92, 0x00ff0000, true);
  words.setUint32(96, 0x0000ff00, true);
  words.setUint32(100, 0x000000ff, true);
  words.setUint32(104, 0xff000000, true);
  words.setUint32(108, 0x1000, true);
  putText(bytes, 128, 'COPY');
  words.setUint32(132, 4, true);
  bytes.set([3, 2, 1, 255], 136);
  return bytes;
}

function putText(bytes: Uint8Array, at: number, text: string): void {
  for (let index = 0; index < text.length; ++index) {
    bytes[at + index] = text.charCodeAt(index);
  }
}

async function eventually(condition: () => boolean): Promise<void> {
  for (let attempt = 0; attempt < 50; ++attempt) {
    if (condition()) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  assert.fail('the EDDS custom editor did not open');
}

function requiredEnvironment(name: string): string {
  const value = process.env[name];
  assert.ok(value, `${name} was not supplied to the extension smoke`);
  return value;
}
