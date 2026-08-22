import assert from 'node:assert/strict';
import { test } from 'node:test';
import { type FormEdit, type ManifestKind, type TextChange, changesOf, formOf } from './form';

/** The file with the changes in it, which is what the editor is left holding. */
function applied(source: string, changes: readonly TextChange[]): string {
  return [...changes]
    .sort((a, b) => b.offset - a.offset)
    .reduce(
      (text, change) =>
        text.slice(0, change.offset) + change.content + text.slice(change.offset + change.length),
      source,
    );
}

function edited(source: string, edit: FormEdit, kind: ManifestKind = 'mod'): string {
  return applied(source, changesOf(kind, source, edit));
}

const MANIFEST = `{
  // What the panel and the launcher call this mod.
  "name": "CAD4Z Core",
  "version": "1.0.0",

  "launch": {
    "modsDirectory": "Addons",
    "targets": [
      {
        "name": "Client",
        "map": "ChernarusPlus",
        "run": "client"
      }
    ]
  }
}
`;

test('a mod manifest is shown as its own fields and its launch block', () => {
  const form = formOf('mod', MANIFEST);

  assert.equal(form.kind, 'mod');
  assert.equal(form.mod?.name, 'CAD4Z Core');
  assert.equal(form.mod?.version, '1.0.0');
  assert.equal(form.launch.modsDirectory, 'Addons');
  assert.deepEqual(form.launch.targets, [
    {
      name: 'Client',
      mod: undefined,
      map: 'ChernarusPlus',
      run: 'client',
      serverConfig: undefined,
    },
  ]);
  assert.equal(form.refusal, undefined);
  assert.deepEqual(form.problems, []);
});

test('a workspace manifest has no fields of its own and the same launch block', () => {
  const form = formOf('workspace', '{ "launch": { "modsDirectory": "Built" } }');

  assert.equal(form.mod, undefined);
  assert.equal(form.launch.modsDirectory, 'Built');
  assert.equal(form.refusal, undefined);
});

test('an empty file is a form with empty fields rather than one that refuses', () => {
  for (const source of ['', '{}', '\n\t ']) {
    const form = formOf('mod', source);

    assert.equal(form.refusal, undefined, source);
    assert.equal(form.mod?.name, undefined);
    assert.deepEqual(form.launch.targets, []);
  }
});

test('a broken file keeps everything that was readable and is not written into', () => {
  const source = `{
  "name": "CAD4Z Core"
  "author": "hurfy"
}`;
  const form = formOf('mod', source);

  assert.equal(form.mod?.name, 'CAD4Z Core');
  assert.deepEqual(form.problems, [{ message: 'Comma expected.', line: 3, column: 3 }]);
  assert.ok(form.refusal?.includes('syntax error'));
  assert.deepEqual(changesOf('mod', source, { kind: 'set', path: ['name'], value: 'Other' }), []);
});

test('a file that is not an object at all is shown and left alone', () => {
  const source = '["CAD4Z Core"]';
  const form = formOf('mod', source);

  assert.ok(form.refusal?.includes('a list'));
  assert.deepEqual(changesOf('mod', source, { kind: 'set', path: ['name'], value: 'x' }), []);
});

test('a key answered for twice is shown, named, and written into neither time', () => {
  const source = '{\n  "name": "One",\n  "name": "Two"\n}\n';
  const form = formOf('mod', source);

  // The reader takes the last, the way JSON.parse does; an edit would land on the first.
  assert.equal(form.mod?.name, 'Two');
  assert.ok(form.refusal?.includes('"name" is written twice'), form.refusal);
  assert.deepEqual(changesOf('mod', source, { kind: 'set', path: ['name'], value: 'x' }), []);
});

test('a key answered for twice is found however deep in the file it is', () => {
  const source = '{ "launch": { "targets": [ { "name": "A", "name": "B" } ] } }';

  assert.ok(formOf('mod', source).refusal?.includes('"name" is written twice'));
});

test('a list the reader could not read whole leaves rows that are not addresses', () => {
  // The first target has no name, so the reader drops it — and "Client", the one row the form
  // would show, is written second. Editing row 0 would rename the target nobody can see.
  const source = `{
  "launch": {
    "targets": [
      { "map": "A" },
      { "name": "Client", "map": "B" }
    ]
  }
}
`;
  const form = formOf('mod', source);

  assert.deepEqual(
    form.launch.targets.map((target) => target.name),
    ['Client'],
  );
  assert.ok(form.refusal?.includes('launch.targets'), form.refusal);
  assert.deepEqual(
    changesOf('mod', source, { kind: 'clear', path: ['launch', 'targets', 0] }),
    [],
  );
});

test('the same holds for a plain list, and for the file a workspace is configured by', () => {
  assert.ok(formOf('mod', '{ "exclude": [1, "*.psd"] }').refusal?.includes('exclude'));
  assert.ok(
    formOf('workspace', '{ "launch": { "clientMods": ["@CF", 2] } }').refusal?.includes(
      'launch.clientMods',
    ),
  );
});

test('a list of the wrong type altogether shows no rows, so there is nothing to misaddress', () => {
  const form = formOf('mod', '{ "exclude": "*.psd" }');

  assert.deepEqual(form.mod?.exclude, []);
  assert.equal(form.refusal, undefined);
});

test('a field that is there is replaced where it stands, and nothing else moves', () => {
  assert.equal(
    edited(MANIFEST, { kind: 'set', path: ['name'], value: 'CAD4Z' }),
    MANIFEST.replace('"CAD4Z Core"', '"CAD4Z"'),
  );
});

test('a field that is not there is written where the schema writes it', () => {
  assert.equal(
    edited(MANIFEST, { kind: 'set', path: ['author'], value: 'hurfy' }),
    `{
  // What the panel and the launcher call this mod.
  "name": "CAD4Z Core",
  "author": "hurfy",
  "version": "1.0.0",

  "launch": {
    "modsDirectory": "Addons",
    "targets": [
      {
        "name": "Client",
        "map": "ChernarusPlus",
        "run": "client"
      }
    ]
  }
}
`,
  );
});

test('a new field goes after a field nobody declared rather than in front of it', () => {
  assert.equal(
    edited('{\n  "name": "CAD4Z Core",\n  "autor": "hurfy"\n}\n', {
      kind: 'set',
      path: ['version'],
      value: '1.0.0',
    }),
    '{\n  "name": "CAD4Z Core",\n  "autor": "hurfy",\n  "version": "1.0.0"\n}\n',
  );
});

test('a workspace file is written into the same way, and its launch block made the same way', () => {
  assert.equal(
    edited(
      '{\n  "$schema": "./workspace.enf.schema.json"\n}\n',
      { kind: 'set', path: ['launch', 'modsDirectory'], value: 'Built' },
      'workspace',
    ),
    '{\n  "$schema": "./workspace.enf.schema.json",\n  "launch": {\n    "modsDirectory": "Built"\n  }\n}\n',
  );
});

test('a launch block the form has to make goes last, where both files write it', () => {
  const written = edited(
    '{\n  "$schema": "./mod.enf.schema.json",\n  "name": "CAD4Z Core",\n  "version": "1.0.0"\n}\n',
    { kind: 'set', path: ['launch', 'modsDirectory'], value: 'Addons' },
  );

  assert.equal(
    written,
    '{\n  "$schema": "./mod.enf.schema.json",\n  "name": "CAD4Z Core",\n  "version": "1.0.0",\n' +
      '  "launch": {\n    "modsDirectory": "Addons"\n  }\n}\n',
  );
});

test('a box emptied takes the field out of the file', () => {
  assert.equal(
    edited(MANIFEST, { kind: 'set', path: ['version'], value: '' }),
    `{
  // What the panel and the launcher call this mod.
  "name": "CAD4Z Core",

  "launch": {
    "modsDirectory": "Addons",
    "targets": [
      {
        "name": "Client",
        "map": "ChernarusPlus",
        "run": "client"
      }
    ]
  }
}
`,
  );
});

test('a field taken out leaves the note at the end of the line above where it was', () => {
  assert.equal(
    edited('{\n  "name": "A", // the mod as the launcher shows it\n  "version": "1"\n}\n', {
      kind: 'clear',
      path: ['version'],
    }),
    '{\n  "name": "A" // the mod as the launcher shows it\n}\n',
  );
});

test('a field taken out leaves the note above the field below it', () => {
  assert.equal(
    edited('{\n  "name": "A",\n  // the version the launcher shows\n  "version": "1"\n}\n', {
      kind: 'clear',
      path: ['name'],
    }),
    '{\n  // the version the launcher shows\n  "version": "1"\n}\n',
  );
});

test('a field taken out does take the notes and the blank line that were its own', () => {
  assert.equal(
    edited(
      '{\n  "name": "A",\n\n  // who made it\n  "author": "hurfy",\n  "version": "1"\n}\n',
      { kind: 'clear', path: ['author'] },
    ),
    '{\n  "name": "A",\n  "version": "1"\n}\n',
  );
});

test('an item taken out of a list leaves the notes of the items around it', () => {
  assert.equal(
    edited(
      '{\n  "exclude": [\n    "*.psd", // Photoshop\n    "*.blend", // Blender\n    "*.max"\n  ]\n}\n',
      { kind: 'clear', path: ['exclude', 2] },
    ),
    '{\n  "exclude": [\n    "*.psd", // Photoshop\n    "*.blend" // Blender\n  ]\n}\n',
  );
});

test('a field of the launch block is written into the block rather than beside it', () => {
  const written = edited(MANIFEST, { kind: 'set', path: ['launch', 'clientMods'], value: 'x' });

  assert.ok(written.includes('"modsDirectory": "Addons",\n    "clientMods": "x",'), written);
});

test('a launch block the file has not got is made to hold the field', () => {
  assert.equal(
    edited('{ "name": "CAD4Z Core" }', {
      kind: 'set',
      path: ['launch', 'modsDirectory'],
      value: 'Addons',
    }),
    '{\n  "name": "CAD4Z Core",\n  "launch": {\n    "modsDirectory": "Addons"\n  }\n}',
  );
});

test('a list takes an item at its end, and an emptied one adds nothing', () => {
  const source = '{\n  "exclude": ["*.psd"]\n}\n';

  assert.equal(
    edited(source, { kind: 'append', path: ['exclude'], value: '*.blend' }),
    '{\n  "exclude": [\n    "*.psd",\n    "*.blend"\n  ]\n}\n',
  );
  assert.deepEqual(changesOf('mod', source, { kind: 'append', path: ['exclude'], value: '' }), []);
});

test('a list the file has not got is made by the first item added to it', () => {
  assert.equal(
    edited('{}', { kind: 'append', path: ['exclude'], value: '*.psd' }),
    '{\n  "exclude": [\n    "*.psd"\n  ]\n}',
  );
});

test('an item of a list is edited where it sits, and the line it sits on is left alone', () => {
  assert.equal(
    edited('{\n  "exclude": ["*.psd", "*.blend"]\n}\n', {
      kind: 'set',
      path: ['exclude', 1],
      value: '*.max',
    }),
    '{\n  "exclude": ["*.psd", "*.max"]\n}\n',
  );
});

test('an item sharing its line with the others is taken out of the line it shares', () => {
  const source = '{\n  "exclude": ["*.psd", "*.blend"]\n}\n';

  assert.equal(
    edited(source, { kind: 'clear', path: ['exclude', 0] }),
    '{\n  "exclude": ["*.blend"]\n}\n',
  );
  assert.equal(
    edited(source, { kind: 'clear', path: ['exclude', 1] }),
    '{\n  "exclude": ["*.psd"]\n}\n',
  );
});

test('the last item of a list leaves an empty list rather than an unparseable one', () => {
  for (const source of ['{ "exclude": ["*.psd"] }', '{\n  "exclude": [\n    "*.psd"\n  ]\n}\n']) {
    const written = edited(source, { kind: 'clear', path: ['exclude', 0] });

    assert.equal(formOf('mod', written).refusal, undefined, written);
    assert.deepEqual(formOf('mod', written).mod?.exclude, [], written);
  }
});

test('the last field of a block leaves an empty block rather than an unparseable one', () => {
  const written = edited('{ "name": "A", "launch": { "modsDirectory": "Addons" } }', {
    kind: 'clear',
    path: ['launch', 'modsDirectory'],
  });

  assert.equal(written, '{ "name": "A", "launch": {} }');
  assert.equal(formOf('mod', written).refusal, undefined);
});

test('a target is added named so that no other target of the file is', () => {
  const once = edited(MANIFEST, { kind: 'addTarget' });
  assert.deepEqual(
    formOf('mod', once).launch.targets.map((target) => target.name),
    ['Client', 'Target'],
  );

  const twice = edited(once, { kind: 'addTarget' });
  assert.deepEqual(
    formOf('mod', twice).launch.targets.map((target) => target.name),
    ['Client', 'Target', 'Target 2'],
  );
});

test('a target is added to a file with no launch block at all', () => {
  const written = edited('{}', { kind: 'addTarget' });

  assert.deepEqual(
    formOf('mod', written).launch.targets.map((target) => target.name),
    ['Target'],
  );
});

test('a target is taken out whole, and its fields are written inside it', () => {
  const emptied = edited(MANIFEST, { kind: 'clear', path: ['launch', 'targets', 0] });
  assert.deepEqual(formOf('mod', emptied).launch.targets, []);

  const written = edited(MANIFEST, {
    kind: 'set',
    path: ['launch', 'targets', 0, 'run'],
    value: 'both',
  });
  assert.equal(formOf('mod', written).launch.targets[0]?.run, 'both');
});

test('a field of a target is written where the schema writes it, not at the end', () => {
  const written = edited(MANIFEST, {
    kind: 'set',
    path: ['launch', 'targets', 0, 'mod'],
    value: 'CADCore',
  });

  assert.ok(
    written.includes('"name": "Client",\n        "mod": "CADCore",\n        "map"'),
    written,
  );
});

test('the comments around what is written survive being written around', () => {
  const source = `{
  // What the panel and the launcher call this mod.
  "name": "CAD4Z Core",
  /* Sources the builder has no business packing. */
  "exclude": [
    "*.psd" // Photoshop
  ]
}
`;

  const named = edited(source, { kind: 'set', path: ['author'], value: 'hurfy' });
  assert.ok(named.includes('// What the panel and the launcher call this mod.'), named);
  assert.ok(named.includes('/* Sources the builder has no business packing. */'), named);
  assert.ok(named.includes('"*.psd" // Photoshop'), named);

  const excluded = edited(source, { kind: 'append', path: ['exclude'], value: '*.blend' });
  assert.ok(excluded.includes('/* Sources the builder has no business packing. */'), excluded);
  assert.ok(excluded.includes('// Photoshop'), excluded);
  assert.deepEqual(formOf('mod', excluded).mod?.exclude, ['*.psd', '*.blend']);
});

test('what the form adds is indented the way the file already is', () => {
  const tabbed = '{\n\t"name": "CAD4Z Core",\n\t"launch": {\n\t\t"modsDirectory": "Addons"\n\t}\n}';

  assert.equal(
    edited(tabbed, { kind: 'set', path: ['launch', 'clientMods'], value: 'x' }),
    '{\n\t"name": "CAD4Z Core",\n\t"launch": {\n\t\t"modsDirectory": "Addons",\n\t\t"clientMods": "x"\n\t}\n}',
  );

  const wide = '{\n    "launch": {\n        "modsDirectory": "Addons"\n    }\n}';
  assert.equal(
    edited(wide, { kind: 'set', path: ['launch', 'clientMods'], value: 'x' }),
    '{\n    "launch": {\n        "modsDirectory": "Addons",\n        "clientMods": "x"\n    }\n}',
  );
});

test('a file written with CRLF stays written with CRLF', () => {
  const source = '{\r\n  "name": "CAD4Z Core"\r\n}\r\n';
  const written = edited(source, { kind: 'set', path: ['version'], value: '1.0.0' });

  assert.equal(written, '{\r\n  "name": "CAD4Z Core",\r\n  "version": "1.0.0"\r\n}\r\n');
});

test('a trailing comma is JSONC, so the form still writes into the file', () => {
  const written = edited('{\n  "name": "CAD4Z Core",\n}\n', {
    kind: 'set',
    path: ['version'],
    value: '1.0.0',
  });

  assert.equal(formOf('mod', written).mod?.version, '1.0.0');
  assert.equal(formOf('mod', written).mod?.name, 'CAD4Z Core');
});

test('writing back what is already there is not a change, so the file is not marked dirty', () => {
  assert.deepEqual(
    changesOf('mod', MANIFEST, { kind: 'set', path: ['name'], value: 'CAD4Z Core' }),
    [],
  );
  assert.deepEqual(
    changesOf('mod', MANIFEST, {
      kind: 'set',
      path: ['launch', 'targets', 0, 'run'],
      value: 'client',
    }),
    [],
  );
  assert.deepEqual(changesOf('mod', MANIFEST, { kind: 'clear', path: ['author'] }), []);
});

test('a block written as something else is left as it is rather than written over', () => {
  assert.deepEqual(
    changesOf('mod', '{ "launch": "everything" }', {
      kind: 'set',
      path: ['launch', 'modsDirectory'],
      value: 'Addons',
    }),
    [],
  );
});

test('the schema line a file points at itself with stays first', () => {
  assert.equal(
    edited('{\n  "$schema": "./mod.enf.schema.json"\n}\n', {
      kind: 'set',
      path: ['name'],
      value: 'CAD4Z Core',
    }),
    '{\n  "$schema": "./mod.enf.schema.json",\n  "name": "CAD4Z Core"\n}\n',
  );
});

test('an empty file is one the form writes into rather than refuses', () => {
  assert.equal(
    edited('', { kind: 'set', path: ['name'], value: 'CAD4Z Core' }),
    '{\n  "name": "CAD4Z Core"\n}',
  );
});
