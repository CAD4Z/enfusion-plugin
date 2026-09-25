import assert from 'node:assert/strict';
import { test } from 'node:test';
import { projectOf } from '../../src/mods/workbench';

const ROOT = '/f:/Code/cad4z/CADCore';

test('the conventional dayz.gproj of the target mod wins over helper projects', () => {
  assert.equal(
    projectOf(ROOT, [
      `${ROOT}/Tools/Preview.gproj`,
      '/f:/Code/cad4z/Another/Workbench/dayz.gproj',
      `${ROOT}/CADCore/Workbench/dayz.gproj`,
    ]),
    `${ROOT}/CADCore/Workbench/dayz.gproj`,
  );
});

test('without dayz.gproj the shallowest stable project is used', () => {
  assert.equal(
    projectOf(ROOT, [
      `${ROOT}/Deep/Workbench/Zeta.gproj`,
      `${ROOT}/Workbench/Beta.gproj`,
      `${ROOT}/Workbench/Alpha.gproj`,
    ]),
    `${ROOT}/Workbench/Alpha.gproj`,
  );
});

test('a project from another mod is never borrowed', () => {
  assert.equal(projectOf(ROOT, ['/f:/Code/cad4z/CADMap/Workbench/dayz.gproj']), undefined);
});
