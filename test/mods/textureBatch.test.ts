import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  type TextureBatchInput,
  type TextureBatchItemInput,
  textureBatchPlanOf,
  withTextureBatchProfile,
} from '../../src/mods/textureBatch';
import { DEFAULT_TEXTURE_PROFILE } from '../../src/mods/textureConversion';
import { BATCH_MAX_JOBS } from '../../src/mods/textureBatchProtocol';

const revision = { size: 91, modified: 1_725_000_000_000 };

test('the context target is primary while the captured selection is deduplicated and stable', () => {
  const plan = textureBatchPlanOf(batch({
    primary: 'C:\\mod\\Mod\\zeta.png',
    items: [
      item('C:\\mod\\Mod\\zeta.png'),
      item('C:\\mod\\Mod\\Alpha.tga'),
      item('c:\\MOD\\mod\\ZETA.PNG'),
    ],
  }));

  assert.equal(plan.kind, 'ready');
  assert.equal(plan.kind === 'ready' && plan.primary, 'C:\\mod\\Mod\\zeta.png');
  assert.deepEqual(
    plan.kind === 'ready' && plan.items.map(({ source }) => source),
    ['C:\\mod\\Mod\\Alpha.tga', 'C:\\mod\\Mod\\zeta.png'],
  );
  assert.deepEqual(plan.kind === 'ready' && plan.profile, DEFAULT_TEXTURE_PROFILE);
});

test('only metadata owned by the primary initializes the complete common profile', () => {
  const common = { ...DEFAULT_TEXTURE_PROFILE, FormatCompress: 'Best' as const, GenerateMips: false };
  const primary = item('C:\\mod\\Mod\\zeta.png', {
    metadata: {
      kind: 'valid',
      revision,
      value: metadata('0123456789ABCDEF', 'zeta.png', common),
    },
  });
  const other = item('C:\\mod\\Mod\\alpha.tga', {
    metadata: {
      kind: 'valid',
      revision,
      value: metadata('1023456789ABCDEF', 'alpha.tga', DEFAULT_TEXTURE_PROFILE),
    },
  });

  const plan = textureBatchPlanOf(batch({ items: [other, primary] }));

  assert.equal(plan.kind, 'ready');
  assert.deepEqual(plan.kind === 'ready' && plan.profile, common);
  assert.equal(
    plan.kind === 'ready' && plan.items.every(
      (candidate) => candidate.kind !== 'ready' || candidate.plan.profile === plan.profile,
    ),
    true,
  );

  const takeover = textureBatchPlanOf(batch({
    items: [item('C:\\mod\\Mod\\zeta.png', {
      metadata: {
        kind: 'valid',
        revision,
        value: metadata('0123456789ABCDEF', 'former.tga', common),
      },
    })],
  }));
  assert.deepEqual(takeover.kind === 'ready' && takeover.profile, DEFAULT_TEXTURE_PROFILE);
});

test('an unsupported profile not owned by the primary is replaced by defaults', () => {
  const plan = textureBatchPlanOf(batch({
    items: [item('C:\\mod\\Mod\\zeta.png', {
      outputRevision: revision,
      metadata: {
        kind: 'unsupported',
        revision,
        identity: {
          guid: '0123456789ABCDEF',
          name: 'Mod/zeta.edds',
          sourceFile: 'former.tga',
        },
        reason: 'Conversion DXT5 is not supported.',
      },
    })],
  }));

  assert.equal(plan.kind, 'ready');
  assert.deepEqual(plan.kind === 'ready' && plan.profile, DEFAULT_TEXTURE_PROFILE);
  assert.equal(plan.kind === 'ready' && plan.items[0]?.kind, 'ready');
});

test('an invalid primary blocks authoring while invalid non-primary selections are isolated', () => {
  const invalid = {
    kind: 'invalid' as const,
    revision,
    reason: 'its GUID is malformed',
  };
  assert.deepEqual(
    textureBatchPlanOf(batch({ items: [item('C:\\mod\\Mod\\zeta.png', { metadata: invalid })] })),
    { kind: 'refused', reason: 'The primary texture profile is not readable: its GUID is malformed' },
  );

  const plan = textureBatchPlanOf(batch({
    items: [
      item('C:\\mod\\Mod\\zeta.png'),
      item('C:\\mod\\Mod\\broken.tga', { metadata: invalid }),
      item('D:\\external\\loose.png'),
      item('C:\\mod\\Mod\\folder', { kind: 'folder', sourceRevision: undefined }),
    ],
  }));
  assert.equal(plan.kind, 'ready');
  assert.deepEqual(
    plan.kind === 'ready' && plan.items.map((candidate) => [candidate.source, candidate.kind]),
    [
      ['C:\\mod\\Mod\\broken.tga', 'refused'],
      ['C:\\mod\\Mod\\folder', 'refused'],
      ['C:\\mod\\Mod\\zeta.png', 'ready'],
      ['D:\\external\\loose.png', 'refused'],
    ],
  );
  assert.equal(plan.kind === 'ready' && plan.jobs.length, 1);
});

test('every source sharing a Windows-normalized destination is refused as one collision group', () => {
  const plan = textureBatchPlanOf(batch({
    items: [
      item('C:\\mod\\Mod\\same.png'),
      item('c:/MOD/Mod/SAME.tga'),
      item('C:\\mod\\Mod\\zeta.png'),
    ],
  }));

  assert.equal(plan.kind, 'ready');
  assert.deepEqual(
    plan.kind === 'ready' && plan.items.map((candidate) => [
      candidate.source,
      candidate.kind,
      candidate.kind === 'refused' ? candidate.reason : candidate.plan.profile.FormatCompress,
    ]),
    [
      ['C:\\mod\\Mod\\same.png', 'refused', 'Multiple selected sources resolve to the same EDDS output.'],
      ['c:/MOD/Mod/SAME.tga', 'refused', 'Multiple selected sources resolve to the same EDDS output.'],
      ['C:\\mod\\Mod\\zeta.png', 'ready', 'Fastest'],
    ],
  );
  assert.deepEqual(
    plan.kind === 'ready' && plan.jobs.map(({ source }) => source),
    ['C:\\mod\\Mod\\zeta.png'],
  );
});

test('a refused source still prevents a ready collision peer from reaching the encoder', () => {
  const invalid = { kind: 'invalid' as const, revision, reason: 'its GUID is malformed' };
  const plan = textureBatchPlanOf(batch({
    items: [
      item('C:\\mod\\Mod\\zeta.png'),
      item('C:\\mod\\Mod\\same.png'),
      item('C:\\mod\\Mod\\same.tga', { metadata: invalid }),
    ],
  }));

  assert.equal(plan.kind, 'ready');
  assert.deepEqual(
    plan.kind === 'ready' && plan.jobs.map(({ source }) => source),
    ['C:\\mod\\Mod\\zeta.png'],
  );
  assert.equal(
    plan.kind === 'ready' && plan.items.find(({ source }) => source.endsWith('same.png'))?.kind,
    'refused',
  );
});

test('a common profile replaces every ready item profile as one whole value', () => {
  const original = textureBatchPlanOf(batch({
    items: [item('C:\\mod\\Mod\\alpha.tga'), item('C:\\mod\\Mod\\zeta.png')],
  }));
  assert.equal(original.kind, 'ready');
  if (original.kind !== 'ready') return;
  const common = { ...DEFAULT_TEXTURE_PROFILE, FormatCompress: 'Copy' as const, GenerateMips: false };

  const changed = withTextureBatchProfile(original, common);

  assert.deepEqual(changed.profile, common);
  assert.equal(changed.items.every(
    (candidate) => candidate.kind !== 'ready' || candidate.plan.profile === changed.profile,
  ), true);
  assert.equal(changed.jobs.every((job) => job.profile === changed.profile), true);
});

test('an unsupported old non-primary profile keeps valid identity but is replaced by the common profile', () => {
  const plan = textureBatchPlanOf(batch({
    items: [
      item('C:\\mod\\Mod\\zeta.png'),
      item('C:\\mod\\Mod\\alpha.tga', {
        outputRevision: revision,
        metadata: {
          kind: 'unsupported',
          revision,
          identity: {
            guid: 'A0A1A2A3A4A5A6A7',
            name: 'Mod/alpha.edds',
            sourceFile: 'alpha.tga',
          },
          reason: 'Conversion DXT5 is not supported.',
        },
      }),
    ],
  }));

  assert.equal(plan.kind, 'ready');
  const alpha = plan.kind === 'ready'
    ? plan.items.find(({ source }) => source.toLowerCase().endsWith('alpha.tga'))
    : undefined;
  assert.equal(alpha?.kind, 'ready');
  assert.equal(alpha?.kind === 'ready' && alpha.plan.identity?.guid, 'A0A1A2A3A4A5A6A7');
  assert.deepEqual(alpha?.kind === 'ready' && alpha.plan.profile, DEFAULT_TEXTURE_PROFILE);
});

test('a destination reached through a dot segment is the same destination', () => {
  const plan = textureBatchPlanOf(batch({
    primary: 'C:\\mod\\Mod\\zeta.png',
    items: [
      item('C:\\mod\\Mod\\zeta.png'),
      item('C:\\mod\\Mod\\Art\\..\\zeta.png'),
      item('C:\\mod\\Mod\\alpha.tga'),
    ],
  }));

  assert.equal(plan.kind, 'ready');
  if (plan.kind !== 'ready') return;
  // The walked path is the same file, so it never became a second item to collide with.
  assert.deepEqual(plan.items.map(({ source }) => source), [
    'C:\\mod\\Mod\\alpha.tga',
    'C:\\mod\\Mod\\zeta.png',
  ]);
  assert.deepEqual(plan.items.map(({ kind }) => kind), ['ready', 'ready']);
});

test('a selection above the documented job ceiling is refused before an editor offers to run it', () => {
  const sources = Array.from(
    { length: BATCH_MAX_JOBS + 1 },
    (_, at) => `C:\\mod\\Mod\\t${String(at).padStart(4, '0')}.png`,
  );
  const primary = sources[0] ?? '';
  const items = sources.map((source, at) => item(source, {
    newGuid: at.toString(16).toUpperCase().padStart(16, '0'),
  }));

  const refused = textureBatchPlanOf(batch({ primary, items }));
  assert.equal(refused.kind, 'refused');
  assert.equal(
    refused.kind === 'refused' && refused.reason,
    `A conversion batch runs at most ${BATCH_MAX_JOBS} textures at once; this selection has ${BATCH_MAX_JOBS + 1}.`,
  );

  const ready = textureBatchPlanOf(batch({ primary, items: items.slice(0, BATCH_MAX_JOBS) }));
  assert.equal(ready.kind, 'ready');
  assert.equal(ready.kind === 'ready' && ready.jobs.length, BATCH_MAX_JOBS);
});

function batch(over: Partial<TextureBatchInput> = {}): TextureBatchInput {
  return {
    primary: 'C:\\mod\\Mod\\zeta.png',
    roots: [{ root: 'C:\\mod', prefixRoot: 'C:\\mod\\Mod' }],
    items: [item('C:\\mod\\Mod\\zeta.png')],
    occupiedGuids: [],
    ...over,
  };
}

function item(
  source: string,
  over: Partial<TextureBatchItemInput> = {},
): TextureBatchItemInput {
  return { ...baseItem(source), ...over };
}

function baseItem(source: string): TextureBatchItemInput {
  return {
    source,
    kind: 'file' as const,
    sourceRevision: revision,
    outputRevision: undefined,
    metadata: { kind: 'missing' as const },
    newGuid: source.toLowerCase().endsWith('zeta.png')
      ? '0123456789ABCDEF'
      : '1023456789ABCDEF',
  };
}

function metadata(
  guid: string,
  sourceFile: string,
  profile = DEFAULT_TEXTURE_PROFILE,
) {
  return {
    guid,
    name: 'Mod/texture.edds',
    sourceFile,
    sourceFormat: sourceFile.toLowerCase().endsWith('.png') ? 'PNG' as const : 'TGA' as const,
    profile,
  };
}
