import assert from 'node:assert/strict';
import { test } from 'node:test';
import { parseConfig } from './config';

test('reads every CfgPatches class with what it requires', () => {
  const config = parseConfig(`
class CfgPatches
{
	class CADMap
	{
		units[] = {};
		requiredVersion = 0.1;
		requiredAddons[] = { "DZ_Scripts", "CADCore" };
	};
};
`);

  assert.deepEqual(config.patches, [{ name: 'CADMap', requiredAddons: ['DZ_Scripts', 'CADCore'] }]);
});

test('reads the mod a CfgMods block declares, which is what makes an addon the main one', () => {
  const config = parseConfig(`
class CfgMods
{
	class CADCore
	{
		type = "mod";
		dir = "CADCore";
		name = "CAD4Z Core";
	};
};
`);

  assert.deepEqual(config.mod, { dir: 'CADCore' });
});

test('gets past what real configs are written with: includes, comments, doubled quotes', () => {
  const config = parseConfig(`
#include "\\DZ\\data\\basicDefines.hpp"

// The mission addon of the map.
class CfgPatches
{
	/* The class name has nothing to do with the folder name. */
	class DZ_Worlds_Sakhal_CE
	{
		requiredAddons[] = {"DZ_Data"};
	};
};

class CfgMods
{
	class CADCore
	{
		overview = "Says ""hello"" to nobody";
		dir = "CADCore";
	};
};
`);

  assert.deepEqual(config.patches, [
    { name: 'DZ_Worlds_Sakhal_CE', requiredAddons: ['DZ_Data'] },
  ]);
  assert.equal(config.mod?.dir, 'CADCore');
});

test('has no mod declaration when there is no CfgMods', () => {
  const config = parseConfig('class CfgPatches { class DZ_Worlds_Sakhal_CE {}; };');

  assert.equal(config.mod, undefined);
});
