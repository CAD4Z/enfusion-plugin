import assert from 'node:assert/strict';
import { mkdir, mkdtemp, readlink, rm, symlink } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { test } from 'node:test';
import { filePatchingPlanOf } from '../../../src/mods/launch/launch';
import { modNameOf } from '../../../src/mods/modName';
import { samePath } from '../../../src/mods/paths';
import { prepareLaunch, readLinkFacts } from '../../../src/platform/launch/launch';

const WINDOWS_ONLY = { skip: process.platform !== 'win32' && 'the run folder is linked the Windows way' };

/**
 * A run folder whose link to ModA still points where ModA's prefix root was before its folder moved,
 * and the folder it is in now. The old one is gone, so the link points at nothing.
 */
interface MovedMod {
  readonly folder: string;
  readonly run: string;
  readonly before: string;
  readonly after: string;
}

async function movedMod(): Promise<MovedMod> {
  const folder = await mkdtemp(join(tmpdir(), 'enfusion-run-test-'));
  const run = join(folder, 'run');
  const before = join(folder, 'Old', 'ModA');
  const after = join(folder, 'Repo', 'ModA', 'ModA');

  await mkdir(run, { recursive: true });
  await mkdir(before, { recursive: true });
  await mkdir(after, { recursive: true });
  await symlink(before, join(run, 'ModA'), 'junction');
  await rm(join(folder, 'Old'), { recursive: true });

  return { folder, run, before, after };
}

test('a link whose folder is gone is read as a link, with where it pointed', WINDOWS_ONLY, async () => {
  const mod = await movedMod();

  try {
    const fact = (await readLinkFacts(mod.run)).get('ModA');

    assert.equal(fact?.kind, 'link');
    assert.equal(fact.kind === 'link' ? samePath(fact.target) : '', samePath(mod.before));
  } finally {
    await rm(mod.folder, { recursive: true, force: true });
  }
});

test('a launch after a mod moved links it where it is now', WINDOWS_ONLY, async () => {
  const mod = await movedMod();

  try {
    const filePatching = filePatchingPlanOf({
      root: mod.run,
      game: join(mod.folder, 'DayZ'),
      entries: [],
      mods: [
        {
          name: 'ModA',
          modName: modNameOf('ModA'),
          root: join(mod.folder, 'Repo', 'ModA'),
          prefixRoot: mod.after,
          addons: ['ModA'],
        },
      ],
      present: await readLinkFacts(mod.run),
    });

    await prepareLaunch({
      refusals: [],
      warnings: [],
      filePatching,
      folders: [],
      fresh: [],
      copies: [],
      windowSettings: [],
      processes: [],
    });

    assert.equal(samePath(await readlink(join(mod.run, 'ModA'))), samePath(mod.after));
  } finally {
    await rm(mod.folder, { recursive: true, force: true });
  }
});
