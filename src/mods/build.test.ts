import assert from 'node:assert/strict';
import { test } from 'node:test';
import {
  type BuildJob,
  type BuildPlan,
  type BuildSource,
  DEFAULT_EXCLUDE,
  buildPlanOf,
  jobsOf,
} from './build';
import type { MachineSettings } from './machine';
import { modsFromScan } from './model';
import type { Link, LinkState } from './workDrive';

const TOOLS = 'F:\\DayZ Tools';
const PBOPROJECT = 'C:\\Mikero\\bin\\pboProject.exe';
const KEY = 'C:\\keys\\hurfy.biprivatekey';
const MODS = 'P:\\Mods';

const CORE = link('CADCore', 'F:\\Code\\CADCore\\CADCore');
const MAP = link('CADMap', 'F:\\Code\\CADMap\\CADMap');

/**
 * The whole plan for the plainest case there is: one addon, one mod, a key set. Compared entire,
 * because what a build is worth checking for is the command line it will run and the file it will
 * judge itself by — not which functions were called on the way there.
 */
test('a single-addon mod comes out as pack, sign and the root files, in that order', () => {
  const plan = buildPlanOf([job()], settings());

  assert.deepEqual(plan, {
    refusals: [],
    warnings: [],
    steps: [
      {
        kind: 'pack',
        subject: 'CADCore',
        what: 'Packing CADCore',
        folders: ['P:\\Mods\\@CADCore', 'P:\\Mods\\@CADCore\\Addons', 'P:\\Mods\\@CADCore\\Keys'],
        stale: [
          'P:\\Mods\\@CADCore\\Addons\\CADCore.pbo',
          'P:\\Mods\\@CADCore\\Addons\\CADCore.pbo.*.bisign',
        ],
        command:
          'start "pboProject" /wait /MIN "C:\\Mikero\\bin\\pboProject.exe" -P -R -W -K ' +
          '-Mod="P:\\Mods\\@CADCore" "P:\\CADCore" ' +
          `+X="${DEFAULT_EXCLUDE.join(',')}"`,
        pbo: 'P:\\Mods\\@CADCore\\Addons\\CADCore.pbo',
        log: { path: 'P:\\temp\\CADCore.packing.log', appends: false },
        attempts: 2,
        pauseMs: 3000,
      },
      {
        kind: 'sign',
        subject: 'CADCore',
        what: 'Signing CADCore.pbo',
        program: 'F:\\DayZ Tools\\Bin\\DsUtils\\DSSignFile.exe',
        arguments: [KEY, 'P:\\Mods\\@CADCore\\Addons\\CADCore.pbo'],
      },
      {
        kind: 'copy',
        subject: 'CADCore',
        what: 'Copying mod.cpp',
        from: 'F:\\Code\\CADCore\\CADCore\\mod.cpp',
        to: 'P:\\Mods\\@CADCore\\mod.cpp',
      },
      {
        kind: 'copy',
        subject: 'CADCore',
        what: 'Copying meta.cpp',
        from: 'F:\\Code\\CADCore\\CADCore\\meta.cpp',
        to: 'P:\\Mods\\@CADCore\\meta.cpp',
      },
      {
        kind: 'copy',
        subject: 'CADCore',
        what: 'Copying hurfy.bikey',
        from: 'C:\\keys\\hurfy.bikey',
        to: 'P:\\Mods\\@CADCore\\Keys\\hurfy.bikey',
      },
    ],
  });
});

/** The one thing the console is for, and the one reason there is a shell in this at all. */
test('the builder is started in a console of its own, under a title of its own', () => {
  assert.ok(packOf(buildPlanOf([job()], settings())).command.startsWith('start "pboProject" /wait /MIN '));
  assert.ok(
    packOf(buildPlanOf([job()], settings({ builder: 'AddonBuilder' }))).command.startsWith(
      'start "AddonBuilder" /wait /MIN ',
    ),
  );
});

test('AddonBuilder is handed the addons folder, the prefix and the drive to count includes from', () => {
  const pack = packOf(buildPlanOf([job({ within: 'Scripts', addon: 'Scripts' })], settings({ builder: 'AddonBuilder' })));

  assert.equal(
    pack.command,
    'start "AddonBuilder" /wait /MIN "F:\\DayZ Tools\\Bin\\AddonBuilder\\AddonBuilder.exe" ' +
      '"P:\\CADCore\\Scripts" "P:\\Mods\\@CADCore\\Addons" -prefix="CADCore\\Scripts" ' +
      '-project=P:\\ -temp="P:\\temp"',
  );
  assert.equal(pack.pbo, 'P:\\Mods\\@CADCore\\Addons\\Scripts.pbo');
});

/**
 * `-exclude=` crashes AddonBuilder 1.0.240639 before it starts, with any list at all — its own
 * example one included. Passing one would fail every build of every mod that excludes anything,
 * which with a default list is every mod.
 */
test('AddonBuilder is handed no exclude list, because the option it takes one through crashes it', () => {
  const command = packOf(buildPlanOf([job()], settings({ builder: 'AddonBuilder' }))).command;

  assert.ok(!command.includes('-exclude'), command);
});

test('both builders keep their workings on the work drive, under the letter that is set', () => {
  const elsewhere = { ...job(), link: link('CADCore', 'F:\\Code\\CADCore\\CADCore', 'linked', 'W:') };

  assert.equal(packOf(buildPlanOf([elsewhere], settings())).log.path, 'W:\\temp\\CADCore.packing.log');
  assert.ok(
    packOf(buildPlanOf([elsewhere], settings({ builder: 'AddonBuilder' }))).command.includes(
      '-project=W:\\ -temp="W:\\temp"',
    ),
  );
});

/** Which log to read is a fact about the builder, and the runner should not have to know it. */
test('pboProject writes one log per addon, and AddonBuilder appends to one for everything', () => {
  assert.deepEqual(packOf(buildPlanOf([job()], settings())).log, {
    path: 'P:\\temp\\CADCore.packing.log',
    appends: false,
  });

  assert.deepEqual(packOf(buildPlanOf([job()], settings({ builder: 'AddonBuilder' }))).log, {
    path: 'F:\\DayZ Tools\\Bin\\Logs\\AddonBuilder.User.rpt',
    appends: true,
  });
});

/** pboProject pointed at a folder that is not there packs nothing and says nothing about it. */
test('the folders of the built mod are made before the builder is let near them', () => {
  assert.deepEqual(packOf(buildPlanOf([job()], settings())).folders, [
    'P:\\Mods\\@CADCore',
    'P:\\Mods\\@CADCore\\Addons',
    'P:\\Mods\\@CADCore\\Keys',
  ]);
});

/** Both builders answer 0 for a build that failed, so what is there afterwards has to be this run's. */
test('the old pbo and every signature of it come off before the builder runs', () => {
  assert.deepEqual(packOf(buildPlanOf([job()], settings())).stale, [
    'P:\\Mods\\@CADCore\\Addons\\CADCore.pbo',
    'P:\\Mods\\@CADCore\\Addons\\CADCore.pbo.*.bisign',
  ]);
});

test('a build that produced nothing is run once more, and never a third time', () => {
  const pack = packOf(buildPlanOf([job()], settings()));

  assert.equal(pack.attempts, 2);
  assert.ok(pack.pauseMs > 0, 'a second run with no wait would find the same handle held');
});

/**
 * The list is the developer's, and a builder that will not take it is worth being told about:
 * otherwise the pbo quietly carries what it was asked not to and nothing anywhere says so.
 */
test('AddonBuilder ignoring an exclude list is said out loud, and does not stop the build', () => {
  const plan = buildPlanOf([job({ exclude: ['*.psd'] })], settings({ builder: 'AddonBuilder' }));

  assert.equal(plan.refusals.length, 0);
  assert.equal(plan.steps.filter((step) => step.kind === 'pack').length, 1);
  assert.equal(plan.warnings.length, 1);
  assert.ok(plan.warnings[0]?.includes('CADCore'), plan.warnings[0]);
});

test('a mod that excludes nothing has nothing taken from it, so there is nothing to warn about', () => {
  assert.deepEqual(buildPlanOf([job()], settings({ builder: 'AddonBuilder' })).warnings, []);
  assert.deepEqual(buildPlanOf([job({ exclude: ['*.psd'] })], settings()).warnings, []);
});

test('the manifest excludes what it names, and the default list is what it replaces', () => {
  const named = packOf(buildPlanOf([job({ exclude: ['*.psd', 'Workbench'] })], settings()));

  assert.ok(named.command.endsWith('+X="*.psd,Workbench"'), named.command);
  assert.ok(
    packOf(buildPlanOf([job()], settings())).command.endsWith(`+X="${DEFAULT_EXCLUDE.join(',')}"`),
  );
});

/** An unsigned pbo is a pbo, and a developer who set no key asked for exactly that. */
test('with no key set there is no signing step and no key copied, and no complaint either', () => {
  const plan = buildPlanOf([job()], settings({ privateKey: '' }));

  assert.deepEqual(plan.refusals, []);
  assert.deepEqual(
    plan.steps.map((step) => step.kind),
    ['pack', 'copy', 'copy'],
  );
});

/** A key that is set and cannot be used would leave every pbo unsigned without saying so. */
test('a key with no DSSignFile to use it stops the build rather than quietly skipping the signing', () => {
  const plan = buildPlanOf([job()], settings({ dayzTools: '' }));

  assert.deepEqual(plan.steps, []);
  assert.equal(plan.refusals.length, 1);
  assert.ok(plan.refusals[0]?.reason.includes('DSSignFile'), plan.refusals[0]?.reason);
});

test('a machine with no builder on it builds nothing, and says which one it wanted', () => {
  const missing = buildPlanOf([job()], settings({ pboProject: '' }));
  assert.deepEqual(missing.steps, []);
  assert.ok(missing.refusals[0]?.reason.includes('pboProject'), missing.refusals[0]?.reason);

  const other = buildPlanOf([job()], settings({ builder: 'AddonBuilder', dayzTools: '', privateKey: '' }));
  assert.ok(other.refusals[0]?.reason.includes('AddonBuilder'), other.refusals[0]?.reason);
});

/**
 * The refusal a mod that is not on the work drive gets is the sentence the panel was already
 * showing next to it, so pressing the button explains nothing new.
 */
test('a mod that is not on the work drive refuses to build, and names the path it is not at', () => {
  for (const [state, expected] of [
    ['unlinked', 'nothing is at P:\\CADCore'],
    ['elsewhere', 'points at'],
    ['occupied', 'not a link'],
    ['unavailable', 'not mounted'],
  ] as const) {
    const plan = buildPlanOf([job({ link: link('CADCore', 'F:\\Old', state) })], settings());

    assert.deepEqual(plan.steps, []);
    assert.equal(plan.refusals[0]?.subject, 'CADCore');
    assert.ok(plan.refusals[0]?.reason.includes(expected), `${state}: ${plan.refusals[0]?.reason}`);
  }
});

test('a mod with nowhere to build to refuses, and names the file that should have said where', () => {
  const plan = buildPlanOf([job({ modsDirectory: '  ' })], settings());

  assert.deepEqual(plan.steps, []);
  assert.ok(plan.refusals[0]?.reason.includes('workspace.enf'), plan.refusals[0]?.reason);
});

/**
 * Everything the manifest contributes ends up inside quotes on a command line, and a manifest
 * comes out of whatever repository was opened. Windows has no path and no mask with a quotation
 * mark in it, so refusing one costs nothing and closes the one way a file could end the quoting
 * and start a command of its own.
 */
test('a quotation mark in what goes on the command line is refused rather than quoted', () => {
  for (const job_ of [job({ modsDirectory: 'P:\\Mods" & calc & "' }), job({ exclude: ['a" & calc'] })]) {
    const plan = buildPlanOf([job_], settings());

    assert.deepEqual(plan.steps, []);
    assert.ok(plan.refusals[0]?.reason.includes('quotation mark'), plan.refusals[0]?.reason);
  }
});

/** A `.enf` is text under git: an absolute mods directory is only ever right on one machine. */
test('a relative mods directory is counted from the file that set it, an absolute one is not', () => {
  assert.equal(
    packOf(buildPlanOf([job({ modsDirectory: '..\\builds' })], settings())).pbo,
    'F:\\Code\\..\\builds\\@CADCore\\Addons\\CADCore.pbo',
  );
  assert.equal(
    packOf(buildPlanOf([job({ modsDirectory: 'D:/Mods' })], settings())).pbo,
    'D:\\Mods\\@CADCore\\Addons\\CADCore.pbo',
  );
});

/** One mod refusing is not a reason to leave the others unbuilt. */
test('the mods that can be built are, and the one that cannot is reported beside them', () => {
  const plan = buildPlanOf(
    [job(), job({ link: { ...MAP, state: 'unlinked', at: '' }, addon: 'CADMap' })],
    settings({ privateKey: '' }),
  );

  assert.deepEqual(
    plan.steps.filter((step) => step.kind === 'pack').map((step) => step.subject),
    ['CADCore'],
  );
  assert.deepEqual(
    plan.refusals.map((refusal) => refusal.subject),
    ['CADMap'],
  );
});

/**
 * Two mods of one name share the one `P:\<Name>`, and only the first of them has it — the second
 * is refused. Counting "the last addon of this mod" over the refused one as well would hang the
 * root files off an addon that is not being built, and lose them.
 */
test('the root files of a mod are copied even where a mod of the same name was refused after it', () => {
  const twin = job({ link: { ...CORE, prefixRoot: '/f:/Other/CADCore', state: 'elsewhere' } });
  const plan = buildPlanOf([job(), twin], settings({ privateKey: '' }));

  assert.deepEqual(
    plan.steps.map((step) => [step.kind, step.what]),
    [
      ['pack', 'Packing CADCore'],
      ['copy', 'Copying mod.cpp'],
      ['copy', 'Copying meta.cpp'],
    ],
  );
  assert.equal(plan.refusals.length, 1);
});

/**
 * The root files belong to the mod. Copying them after every addon would be three copies of the
 * same file for a mod of three pbo, and hanging them off one addon would lose them when that
 * addon is the one that fails.
 */
test('a mod of several addons packs each one and copies its root files once, at the end', () => {
  const plan = buildPlanOf(
    [
      job({ addon: 'Scripts', within: 'Scripts' }),
      job({ addon: 'Data', within: 'Data' }),
    ],
    settings({ privateKey: '' }),
  );

  assert.deepEqual(
    plan.steps.map((step) => [step.kind, step.subject]),
    [
      ['pack', 'CADCore\\Scripts'],
      ['pack', 'CADCore\\Data'],
      ['copy', 'CADCore'],
      ['copy', 'CADCore'],
    ],
  );
});

/**
 * The order is the graph's, all the way from the `config.cpp` files a scan found: CADCore before
 * the mod that requires it, and inside that mod the addon that is required before the one that
 * requires it. Nothing about the order is decided by the build.
 */
test('the addons of a workspace are built in the order the requiredAddons graph puts them', () => {
  const mods = modsFromScan({
    manifests: ['/w/CADMap/mod.enf', '/w/CADCore/mod.enf'],
    configs: [
      {
        path: '/w/CADMap/CADMap/config.cpp',
        source: config({ patch: 'CADMap', requires: ['CADCore_Scripts'], dir: 'CADMap' }),
      },
      {
        path: '/w/CADCore/CADCore/Data/config.cpp',
        source: config({ patch: 'CADCore_Data', requires: ['CADCore_Scripts'] }),
      },
      {
        path: '/w/CADCore/CADCore/Scripts/config.cpp',
        source: config({ patch: 'CADCore_Scripts', dir: 'CADCore' }),
      },
    ],
  });

  const sources: BuildSource[] = mods.map((mod) => ({
    mod,
    link: link(mod.name, `F:\\Code\\${mod.name}\\${mod.name}`),
    modsDirectory: MODS,
    exclude: [],
    configuredIn: 'F:\\Code',
    configuredBy: 'workspace.enf',
  }));

  const plan = buildPlanOf(jobsOf(sources), settings({ privateKey: '' }));

  assert.deepEqual(
    plan.steps.filter((step) => step.kind === 'pack').map((step) => step.subject),
    ['CADCore\\Scripts', 'CADCore\\Data', 'CADMap'],
  );
});

/** The layout is read off the tree, and it is the whole of what a command line differs by. */
test('a single-addon mod is packed from the prefix root and a multi-addon one from below it', () => {
  const mods = modsFromScan({
    manifests: ['/w/CADCore/mod.enf'],
    configs: [
      {
        path: '/w/CADCore/CADCore/config.cpp',
        source: config({ patch: 'CADCore', dir: 'CADCore' }),
      },
    ],
  });

  const only = mods[0];
  assert.ok(only !== undefined);

  const jobs = jobsOf([
    {
      mod: only,
      link: CORE,
      modsDirectory: MODS,
      exclude: [],
      configuredIn: 'F:\\Code',
      configuredBy: 'mod.enf',
    },
  ]);

  assert.deepEqual(
    jobs.map((made) => [made.addon, made.within]),
    [['CADCore', '']],
  );
});

/**
 * The one place the addon's folder name and the pbo's part company. A single-addon mod is packed
 * by pointing the builder at the prefix root itself, and the prefix root sits on the drive under
 * the mod's name — so the pbo, the log and the signature all go by that name, whatever the folder
 * on disk is called. Looking for the folder's name afterwards would call a build that worked a
 * build that failed.
 */
test('a mod whose folder is not its name packs into a pbo named after the mod', () => {
  const client = link('CADNavigationClient', 'F:\\Code\\CADNavigation\\client');
  const plan = buildPlanOf([job({ link: client, addon: 'client', within: '' })], settings());
  const pack = plan.steps[0];

  assert.equal(pack?.kind, 'pack');
  assert.equal(pack.pbo, 'P:\\Mods\\@CADNavigationClient\\Addons\\CADNavigationClient.pbo');
  assert.equal(pack.log.path, 'P:\\temp\\CADNavigationClient.packing.log');
  assert.ok(pack.command.includes('"P:\\CADNavigationClient"'));
  assert.equal(plan.steps[1]?.what, 'Signing CADNavigationClient.pbo');
});

/** A multi-addon mod is pointed at the addon, so there the pbo is the addon's folder after all. */
test('an addon inside the prefix root packs into a pbo named after its own folder', () => {
  const plan = buildPlanOf([job({ addon: 'Scripts', within: 'Scripts' })], settings());
  const pack = plan.steps[0];

  assert.equal(pack?.kind, 'pack');
  assert.equal(pack.pbo, 'P:\\Mods\\@CADCore\\Addons\\Scripts.pbo');
  assert.equal(pack.log.path, 'P:\\temp\\Scripts.packing.log');
});

function job(over: Partial<BuildJob> = {}): BuildJob {
  return {
    link: CORE,
    addon: 'CADCore',
    within: '',
    modsDirectory: MODS,
    configuredIn: 'F:\\Code',
    configuredBy: 'workspace.enf',
    exclude: [],
    ...over,
  };
}

function settings(over: Partial<MachineSettings> = {}): MachineSettings {
  return {
    dayz: 'F:\\DayZ',
    executable: '',
    dayzTools: TOOLS,
    pboProject: PBOPROJECT,
    privateKey: KEY,
    workDrive: 'F:\\Workdrive',
    workDriveLetter: 'P:',
    filePatchingRoot: '',
    profiles: '',
    builder: 'pboProject',
    ...over,
  };
}

function link(name: string, target: string, state: LinkState = 'linked', letter = 'P:'): Link {
  return {
    prefixRoot: `/f:/Code/${name}/${name}`,
    name,
    path: `${letter}\\${name}`,
    target,
    at: state === 'linked' ? target : state === 'elsewhere' ? 'D:\\Somewhere' : '',
    state,
  };
}


/** The one pack step of a plan that has exactly one, which most of these do. */
function packOf(plan: BuildPlan) {
  const pack = plan.steps.find((step) => step.kind === 'pack');
  assert.ok(pack !== undefined, `no pack step: ${JSON.stringify(plan.refusals)}`);

  return pack;
}

/** A `config.cpp` in the shape a real one is written in, with only the parts the model reads. */
function config({
  patch,
  requires = [],
  dir,
}: {
  patch: string;
  requires?: string[];
  dir?: string;
}): string {
  const required = requires.map((addon) => `"${addon}"`).join(', ');
  const patches = `class CfgPatches { class ${patch} { requiredAddons[] = {${required}}; }; };`;
  const declaration =
    dir === undefined ? '' : `class CfgMods { class ${patch} { dir = "${dir}"; }; };`;

  return `${patches}\n${declaration}`;
}
