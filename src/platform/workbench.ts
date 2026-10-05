/**
 * Starting DayZ Workbench on one discovered `.gproj`.
 *
 * Through the shell — `ShellExecuteEx`, the way Explorer and `start` run a program — rather than by
 * creating the process. A Workbench marked to run as administrator, by the compatibility setting
 * or by a manifest, is one `CreateProcess` refuses with `ERROR_ELEVATION_REQUIRED`, which Node
 * reports as `spawn … EACCES`; the shell is what asks UAC, or elevates without asking where the
 * machine is set to, and it starts an ordinary Workbench just the same. `LaunchWorkbench.bat` went
 * through `start` for the same reason, without having to say so.
 *
 * PowerShell is the way to the shell that takes a working directory, and Workbench needs one: it
 * reads `dayz.gproj` out of the folder it was started in. The paths ride in the environment rather
 * than in the script, so the script is the same every time and no path can end a string in it.
 */

import { execFile } from 'node:child_process';
import { access } from 'node:fs/promises';
import { promisify } from 'node:util';
import { windowsFolder } from '../mods/paths';
import { workbenchArgumentsOf } from '../mods/workbench';

const run = promisify(execFile);

/**
 * `UseShellExecute` is what makes `Process.Start` a `ShellExecuteEx`. `Start-Process` would be the
 * same call, but it reads its path as a wildcard, and a folder with `[` in its name is a path it
 * would not find. What went wrong is written out as UTF-8, because that is what is read back, and
 * from under the wrapper PowerShell puts around a failed call: "The operation was canceled by the
 * user" rather than "Exception calling Start with 1 argument(s)".
 */
const START = [
  "$ErrorActionPreference = 'Stop'",
  '[Console]::OutputEncoding = [Text.Encoding]::UTF8',
  'try {',
  '  $start = New-Object System.Diagnostics.ProcessStartInfo',
  '  $start.FileName = $env:ENFUSION_WORKBENCH',
  '  $start.Arguments = $env:ENFUSION_WORKBENCH_ARGUMENTS',
  '  $start.WorkingDirectory = $env:ENFUSION_WORKBENCH_FOLDER',
  '  $start.UseShellExecute = $true',
  '  [void][System.Diagnostics.Process]::Start($start)',
  '} catch {',
  '  $failure = $_.Exception',
  '  if ($failure.InnerException) { $failure = $failure.InnerException }',
  '  [Console]::Out.Write($failure.Message)',
  '  exit 1',
  '}',
].join('\n');

/**
 * Resolves once Workbench is up, or once UAC has been answered: a refused prompt is a failure, and
 * the message it fails with is the one Windows gave.
 */
export async function startWorkbench(
  executable: string,
  project: string,
  repository: string,
): Promise<void> {
  await access(executable);
  await access(project);

  try {
    await run(
      'powershell.exe',
      ['-NoProfile', '-NonInteractive', '-EncodedCommand', Buffer.from(START, 'utf16le').toString('base64')],
      {
        env: {
          ...process.env,
          ENFUSION_WORKBENCH: executable,
          ENFUSION_WORKBENCH_ARGUMENTS: workbenchArgumentsOf(repository),
          ENFUSION_WORKBENCH_FOLDER: windowsFolder(project),
        },
        windowsHide: true,
      },
    );
  } catch (error: unknown) {
    const said = (error as { stdout?: unknown }).stdout;

    throw new Error(
      typeof said === 'string' && said.trim() !== ''
        ? said.trim()
        : error instanceof Error ? error.message : String(error),
    );
  }
}
