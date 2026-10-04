import assert from 'node:assert/strict';
import { test } from 'node:test';
import { defaultFontName, fontCommandPlanOf, fontGenerationSummaryOf } from '../../../src/mods/font/fontCommands';

const context = {
  platform: 'win32', scheme: 'file',
  roots: [{ root: 'C:/Mods/Example', prefixRoot: 'C:/Mods/Example/Example' }],
};

test('generating a font plans three siblings and its Workbench resource name', () => {
  const plan = fontCommandPlanOf({
    ...context, kind: 'generate', file: 'C:/Mods/Example/Example/Fonts/Inter-Regular.ttf',
    name: 'SDF_InterRegular32', size: 32, existing: [], metadata: { kind: 'missing' },
  });
  assert.deepEqual(plan, {
    kind: 'ready', action: 'generate',
    output: 'C:/Mods/Example/Example/Fonts/SDF_InterRegular32.fnt',
    atlas: 'C:/Mods/Example/Example/Fonts/SDF_InterRegular32.edds',
    metadata: 'C:/Mods/Example/Example/Fonts/SDF_InterRegular32.fnt.meta',
    resourceName: 'Example/Fonts/SDF_InterRegular32.fnt', notice: undefined,
    source: 'C:/Mods/Example/Example/Fonts/Inter-Regular.ttf', size: 32,
    characters: undefined, replace: [],
  });
});

test('regeneration targets the selected recipe and refuses a missing one', () => {
  const input = { ...context, kind: 'regenerate' as const, file: 'C:/Mods/Example/Example/Fonts/Old.FNT',
    existing: ['C:/Mods/Example/Example/Fonts/Old.FNT'], metadata: { kind: 'present' as const } };
  const plan = fontCommandPlanOf(input);
  assert.equal(plan.kind, 'ready');
  if (plan.kind !== 'ready') return;
  assert.equal(plan.action, 'regenerate');
  assert.equal(plan.metadata, 'C:/Mods/Example/Example/Fonts/Old.FNT.meta');
  assert.equal('source' in plan, false, 'native code must resolve the saved recipe, not a guessed sibling TTF');
  assert.deepEqual(plan.replace, []);
  assert.equal(fontCommandPlanOf({ ...input, metadata: { kind: 'missing' } }).kind, 'refused');
});

test('a font outside a prefix root gets no recipe, as a texture gets no registration there', () => {
  const generate = { ...context, kind: 'generate' as const, file: 'C:/Mods/Example/Fonts/Sans.ttf',
    name: 'Font', size: 32, existing: [], metadata: { kind: 'missing' as const } };
  for (const roots of [context.roots, [{ root: 'C:/Mods' }, { root: 'C:/Mods/Example' }]]) {
    const plan = fontCommandPlanOf({ ...generate, roots });
    assert.equal(plan.kind, 'ready');
    if (plan.kind !== 'ready') return;
    assert.equal(plan.metadata, 'C:/Mods/Example/Fonts/Font.fnt.meta', 'the recipe path is still checked for a conflict');
    assert.equal(plan.resourceName, undefined);
    assert.equal(plan.notice, 'The recipe was skipped because the font is outside an Enfusion prefix root.');
  }
  const reasonOf = (plan: ReturnType<typeof fontCommandPlanOf>) => plan.kind === 'refused' ? plan.reason : '';
  assert.match(reasonOf(fontCommandPlanOf({ ...generate, existing: ['C:/Mods/Example/Fonts/Font.fnt.meta'],
    metadata: { kind: 'present' } })), /cannot replace one that has a \.fnt\.meta/);
  for (const metadata of [{ kind: 'present' as const }, { kind: 'missing' as const }]) {
    assert.match(reasonOf(fontCommandPlanOf({ ...context, kind: 'regenerate', file: 'C:/Mods/Example/Fonts/Font.fnt',
      existing: [], metadata })), /no recipe to regenerate from/);
  }
});

test('any occupied sibling requires explicit replacement, including an orphan atlas or recipe', () => {
  const output = 'C:/Mods/Example/Example/Font.fnt';
  for (const existing of [[output], [output + '.meta'], ['C:/Mods/Example/Example/Font.edds']]) {
    const plan = fontCommandPlanOf({ ...context, kind: 'generate', file: 'C:/Mods/Example/Example/font.ttf',
      name: 'Font', size: 32, characters: 'C:/Mods/Example/Example/shared.txt', existing, metadata: { kind: 'present' } });
    assert.equal(plan.kind, 'ready');
    if (plan.kind !== 'ready') return;
    assert.equal(plan.action, 'replace');
    assert.deepEqual(plan.replace, existing);
    assert.equal(plan.action === 'replace' ? plan.characters : undefined, 'C:/Mods/Example/Example/shared.txt');
  }
});

test('font names stay one Windows filename and atlas sizes stay in the engine range', () => {
  const input = { ...context, kind: 'generate' as const, file: 'C:/Mods/Example/font.ttf',
    name: 'Font', size: 32, existing: [], metadata: { kind: 'missing' as const } };
  for (const name of ['', '..', '../escape', 'a/b', 'a\\b', 'font.', 'font ', ' font', 'a:b', 'a?', 'a"',
    'a*', 'a<', 'a>', 'a|', 'a\n', 'CON', 'nul', 'LPT1', 'COM9.old', 'COM¹', 'x'.repeat(250)]) {
    assert.equal(fontCommandPlanOf({ ...input, name }).kind, 'refused', JSON.stringify(name));
  }
  for (const size of [7, 41, 32.5, NaN]) assert.equal(fontCommandPlanOf({ ...input, size }).kind, 'refused');
  for (const size of [8, 32, 40]) assert.equal(fontCommandPlanOf({ ...input, size }).kind, 'ready');
  // "Shrift Regular", Russian for "font": a name outside ASCII is still one valid filename.
  assert.equal(fontCommandPlanOf({ ...input, name: '\u0428\u0440\u0438\u0444\u0442 Regular' }).kind, 'ready');
  assert.equal(defaultFontName('Inter Display', 'Semi Bold', 32), 'SDF_InterDisplaySemiBold32');
  assert.equal(defaultFontName('Bad/Family', 'Regular?', 8), 'SDF_BadFamilyRegular8');
});

test('a command refuses non-Windows, remote and out-of-root files in a mixed window', () => {
  const input = { ...context, kind: 'generate' as const, file: 'C:/Mods/Example/font.ttf',
    name: 'Font', size: 32, existing: [], metadata: { kind: 'missing' as const } };
  for (const override of [
    { platform: 'linux' }, { scheme: 'vscode-remote' }, { roots: [] },
    { file: 'C:/Other/font.ttf' }, { file: 'C:/Mods/Example2/font.ttf' },
    { file: 'C:/Mods/Example/../Elsewhere/font.ttf' }, { file: 'C:/Mods/Example/font.otf' },
  ]) {
    assert.equal(fontCommandPlanOf({ ...input, ...override }).kind, 'refused', JSON.stringify(override));
  }
  assert.equal(fontCommandPlanOf({ ...input, file: 'c:\\MODS\\EXAMPLE\\FONT.TTF' }).kind, 'ready');
});

test('a generation summary warns about skipped characters and names the first thin glyphs', () => {
  assert.deepEqual(fontGenerationSummaryOf('SDF_Sans32.fnt', { glyphCount: 287, missing: [], thin: [] }),
    { message: 'SDF_Sans32.fnt: 287 glyphs generated.', warning: false });
  const thin = fontGenerationSummaryOf('SDF_Thin32.fnt', { glyphCount: 287, missing: [0xad], thin: [0x48, 0x4e, 0x54] }, 2);
  assert.equal(thin.warning, true);
  assert.equal(thin.message, 'SDF_Thin32.fnt: 287 glyphs generated. Skipped characters: \u00ad (U+00AD). ' +
    '3 glyphs have strokes thinner than an atlas pixel and break up when drawn larger than the atlas size: ' +
    'H (U+0048), N (U+004E) and 1 more. A heavier weight or a larger size keeps them whole.');
  assert.deepEqual(fontGenerationSummaryOf('SDF_Sans32.fnt', { glyphCount: 287, missing: [], thin: [], notice: 'No recipe.' }),
    { message: 'SDF_Sans32.fnt: 287 glyphs generated. No recipe.', warning: false });
  assert.match(fontGenerationSummaryOf('SDF_Thin32.fnt', { glyphCount: 287, missing: [], thin: [0x48, 0x4e, 0x54] }).message,
    /: H \(U\+0048\), N \(U\+004E\), T \(U\+0054\)\. A heavier/);
});
