/** Starting DayZ Workbench on one discovered `.gproj`. */

import { spawn } from 'node:child_process';
import { access } from 'node:fs/promises';
import { windowsFolder } from '../mods/paths';

export async function startWorkbench(
  executable: string,
  project: string,
  repository: string,
): Promise<void> {
  await access(executable);
  await access(project);

  const child = spawn(executable, ['-doLogs', `-repository=${repository}`], {
    cwd: windowsFolder(project),
    detached: true,
    stdio: 'ignore',
    windowsHide: false,
  });
  child.on('error', () => undefined);

  await new Promise<void>((resolve, reject) => {
    const onSpawn = (): void => {
      child.off('error', onError);
      resolve();
    };
    const onError = (error: Error): void => {
      child.off('spawn', onSpawn);
      reject(error);
    };

    child.once('spawn', onSpawn);
    child.once('error', onError);
  });
  child.unref();
}
