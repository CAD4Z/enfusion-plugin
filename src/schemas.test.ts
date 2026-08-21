/**
 * The two schemas say the same thing about the launch block, because a relative `$ref` between two
 * schema files contributed by an extension does not resolve — the editor loses the base URI and
 * goes looking for `file:///./<name>`. Duplication that nothing checks is duplication that drifts,
 * so this is the check: the blocks have to stay byte-for-byte the same idea.
 *
 * It reads the files from the package root rather than importing them, which is what `npm test`
 * runs from.
 */

import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test } from 'node:test';

function schema(name: string): Record<string, unknown> {
  return JSON.parse(readFileSync(join(process.cwd(), 'schemas', name), 'utf8')) as Record<
    string,
    unknown
  >;
}

const mod = schema('mod.enf.schema.json');
const workspace = schema('workspace.enf.schema.json');

test('both schemas describe the launch block the same way', () => {
  assert.deepEqual(workspace.definitions, mod.definitions);
});

test('both schemas let a file point an editor at the schema it is written against', () => {
  for (const [name, root] of [
    ['mod.enf', mod],
    ['workspace.enf', workspace],
  ] as const) {
    const properties = root.properties as Record<string, unknown>;

    assert.ok(properties.$schema, `${name} has no $schema property`);
    assert.equal(root.additionalProperties, false, `${name} accepts fields nobody declared`);
  }
});
