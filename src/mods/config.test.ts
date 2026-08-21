import assert from 'node:assert/strict';
import { test } from 'node:test';
import { parseConfig, withRequiredAddon } from './config';

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

  assert.deepEqual(config.patches, [
    { name: 'CADMap', requiredAddons: ['DZ_Scripts', 'CADCore'], author: undefined, version: undefined },
  ]);
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
		picture = "";
		overview = "Base module required by the other CAD4Z mods.";
		author = "cad4z";
		version = "1.2.0";
	};
};
`);

  assert.deepEqual(config.mod, {
    dir: 'CADCore',
    name: 'CAD4Z Core',
    overview: 'Base module required by the other CAD4Z mods.',
    author: 'cad4z',
    version: '1.2.0',
  });
});

/** Half of what a config copied from a template holds is fields nobody ever filled in. */
test('a field written empty says nothing, the way a field that is not there says nothing', () => {
  const config = parseConfig('class CfgMods { class M { dir = "M"; name = ""; author = "  "; }; };');

  assert.deepEqual(config.mod, {
    dir: 'M',
    name: undefined,
    overview: undefined,
    author: undefined,
    version: undefined,
  });
});

/** Where a mod says nothing about itself, the addon it is packed from sometimes does. */
test('reads the author and version an addon carries in its own CfgPatches class', () => {
  const config = parseConfig(`
class CfgPatches
{
	class MyMod
	{
		requiredVersion = 0.1;
		requiredAddons[] = { "DZ_Data" };
		author = "Someone";
		version = "1.0";
	};
};
`);

  assert.deepEqual(config.patches, [
    { name: 'MyMod', requiredAddons: ['DZ_Data'], author: 'Someone', version: '1.0' },
  ]);
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
    { name: 'DZ_Worlds_Sakhal_CE', requiredAddons: ['DZ_Data'], author: undefined, version: undefined },
  ]);
  assert.equal(config.mod?.dir, 'CADCore');
  assert.equal(config.mod?.overview, 'Says "hello" to nobody');
});

test('has no mod declaration when there is no CfgMods', () => {
  const config = parseConfig('class CfgPatches { class DZ_Worlds_Sakhal_CE {}; };');

  assert.equal(config.mod, undefined);
});

test('writes an addon into the requiredAddons of a class that has one', () => {
  const source = `class CfgPatches
{
	class CADCore_Scripts
	{
		requiredAddons[] = { "DZ_Scripts" };
	};
};
`;

  assert.equal(
    withRequiredAddon(source, 'CADCore_Scripts', 'CADCore_Data'),
    `class CfgPatches
{
	class CADCore_Scripts
	{
		requiredAddons[] = { "DZ_Scripts", "CADCore_Data" };
	};
};
`,
  );
});

test('keeps a list written a line at a time written that way', () => {
  const source = `class CfgPatches
{
	class CADCore_Scripts
	{
		requiredAddons[] =
		{
			"DZ_Scripts",
			"DZ_Data"
		};
	};
};
`;

  assert.equal(
    withRequiredAddon(source, 'CADCore_Scripts', 'CADCore_Data'),
    `class CfgPatches
{
	class CADCore_Scripts
	{
		requiredAddons[] =
		{
			"DZ_Scripts",
			"DZ_Data",
			"CADCore_Data"
		};
	};
};
`,
  );
});

test('gives a class with no requiredAddons one, indented the way its members are', () => {
  const source = `class CfgPatches
{
	class CADCore_Scripts
	{
		units[] = {};
	};
};
`;

  assert.equal(
    withRequiredAddon(source, 'CADCore_Scripts', 'CADCore_Data'),
    `class CfgPatches
{
	class CADCore_Scripts
	{
		units[] = {};
		requiredAddons[] = { "CADCore_Data" };
	};
};
`,
  );
});

test('fills in an empty list rather than leaving a stray comma in it', () => {
  const source = 'class CfgPatches { class Main { requiredAddons[] = {}; }; };';

  assert.equal(
    withRequiredAddon(source, 'Main', 'Extra'),
    'class CfgPatches { class Main { requiredAddons[] = { "Extra" }; }; };',
  );
});

test('leaves the file alone when the addon is already required, whatever the case', () => {
  const source = 'class CfgPatches { class Main { requiredAddons[] = { "extra" }; }; };';

  assert.equal(withRequiredAddon(source, 'Main', 'Extra'), source);
});

test('answers with nothing at all when the class it was to be written into is not there', () => {
  const source = 'class CfgPatches { class Other { requiredAddons[] = {}; }; };';

  assert.equal(withRequiredAddon(source, 'Main', 'Extra'), undefined);
});

/** A comment is not an item, so the comma that separates items must not end up inside one. */
test('a comment after the last item does not swallow the comma written after it', () => {
  const source = 'class CfgPatches { class Main { requiredAddons[] = { "DZ_Data" // and more\n}; }; };';

  assert.equal(
    withRequiredAddon(source, 'Main', 'Extra'),
    'class CfgPatches { class Main { requiredAddons[] = { "DZ_Data",\n"Extra" // and more\n}; }; };',
  );
});

test('a list holding nothing but a comment is filled in, not continued from', () => {
  const source = 'class CfgPatches { class Main { requiredAddons[] = { /* none yet */ }; }; };';

  assert.equal(
    withRequiredAddon(source, 'Main', 'Extra'),
    'class CfgPatches { class Main { requiredAddons[] = { /* none yet */ "Extra" }; }; };',
  );
});

/** Configs are written on Windows, and one line of `\n` in a CRLF file is a whole-file diff. */
test('a file written with CRLF gets a line written with CRLF', () => {
  const source =
    'class CfgPatches\r\n{\r\n\tclass Main\r\n\t{\r\n\t\trequiredAddons[] =\r\n\t\t{\r\n\t\t\t"DZ_Data"\r\n\t\t};\r\n\t};\r\n};\r\n';

  assert.equal(
    withRequiredAddon(source, 'Main', 'Extra'),
    'class CfgPatches\r\n{\r\n\tclass Main\r\n\t{\r\n\t\trequiredAddons[] =\r\n\t\t{\r\n\t\t\t"DZ_Data",\r\n\t\t\t"Extra"\r\n\t\t};\r\n\t};\r\n};\r\n',
  );
});

test('a class with no requiredAddons in a CRLF file keeps the file CRLF', () => {
  const source = 'class CfgPatches\r\n{\r\n\tclass Main\r\n\t{\r\n\t\tunits[] = {};\r\n\t};\r\n};\r\n';

  assert.equal(
    withRequiredAddon(source, 'Main', 'Extra'),
    'class CfgPatches\r\n{\r\n\tclass Main\r\n\t{\r\n\t\tunits[] = {};\r\n\t\trequiredAddons[] = { "Extra" };\r\n\t};\r\n};\r\n',
  );
});
