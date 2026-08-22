/**
 * The three commands that make something: a mod, a `mod.enf` for a mod that is already there, and
 * an addon of either.
 *
 * The mod is made where the developer asked for it — the folder clicked in the explorer, or the
 * open folder when the command came from the palette or from an empty panel — and it is asked two
 * things and no more: what it is called, and whether it packs into one pbo or one per addon.
 * Everything else about it is worked out from the name in `src/mods/init.ts`, which is also where
 * every refusal is worded.
 *
 * A mod somebody else wrote is asked nothing at all: what a `mod.enf` would be filled in with is
 * in its `config.cpp` already, so it is read out of there and shown, and the one question is
 * whether to write it down.
 *
 * A mod that cannot be built until something is set up by hand is not a mod that was made for the
 * developer, so the last thing making one does is put it on the work drive: it is a build away
 * from being a pbo the moment the wizard closes.
 */

import * as vscode from 'vscode';
import {
  type AddonPlan,
  type Adoption,
  type InitPlan,
  addonNameProblemOf,
  addonPlanOf,
  addonsRefusalOf,
  adoptionOf,
  initPlanOf,
  modNameProblemOf,
} from '../mods/init';
import { CONFIG_FILE, MANIFEST_FILE, type Layout, type Mod, mainAddonOf } from '../mods/model';
import { type WorkDriveState, linkPathOf, linksToMake, refusalOf } from '../mods/workDrive';
import { createFrom, existingOf, folderAt, holds, requireAddon, textOf } from '../platform/init';
import { readMachineSettings } from '../platform/machine';
import { makeLinks, platformRefusal, readLinks, readWorkDrive } from '../platform/workDrive';
import { type Discovery, findMods } from '../platform/workspace';
import { WORK_DRIVE_COMMAND } from './workDrive';

/** The command ids, which the palette, the explorer's menu and the panel all go through. */
export const INIT_COMMAND = {
  mod: 'enfusion.init',
  adopt: 'enfusion.adopt',
  addon: 'enfusion.addon.add',
} as const;

/** Registers all three, and calls back after each so the panel shows what was made. */
export function registerInitCommands(
  log: vscode.LogOutputChannel,
  changed: () => void,
): vscode.Disposable {
  const commands = new InitCommands(log, changed);

  return vscode.Disposable.from(
    vscode.commands.registerCommand(INIT_COMMAND.mod, (where?: vscode.Uri) => commands.mod(where)),
    vscode.commands.registerCommand(INIT_COMMAND.adopt, (target?: { mod: string }) =>
      commands.adopt(target),
    ),
    vscode.commands.registerCommand(INIT_COMMAND.addon, (target?: { mod: string }) =>
      commands.addon(target),
    ),
  );
}

class InitCommands {
  constructor(
    private readonly log: vscode.LogOutputChannel,
    private readonly changed: () => void,
  ) {}

  /** A new mod in the folder the developer pointed at. */
  async mod(where: vscode.Uri | undefined): Promise<void> {
    const root = where === undefined ? await openFolder() : await folderAt(where);
    if (root === undefined) {
      return;
    }

    // One folder holds one mod: `mod.enf` in it is what says whose folder it is.
    if (await holds(root, MANIFEST_FILE)) {
      await vscode.window.showWarningMessage(
        `There is a mod here already: ${vscode.workspace.asRelativePath(root, true)} has a ` +
          `${MANIFEST_FILE}. Pick another folder, or add an addon to the mod that is here.`,
      );
      return;
    }

    const name = await this.askName(root);
    if (name === undefined) {
      return;
    }

    const layout = await askLayout(name);
    if (layout === undefined) {
      return;
    }

    const plan = initPlanOf(name, layout);
    if (!(await this.write(root, plan, name))) {
      return;
    }

    this.log.info(`created ${name} (${layout}) in ${root.fsPath}`);
    this.changed();

    // The manifest is what the mod is configured by, so it is what the developer is left looking
    // at — through the editor's own opening, so it comes up the way a `.enf` comes up everywhere
    // else: as the form, where the fields a new mod has not answered for are boxes to fill in.
    await vscode.commands.executeCommand('vscode.open', vscode.Uri.joinPath(root, MANIFEST_FILE));
    await this.link(root, name, `Made ${name}`);
  }

  /**
   * The `mod.enf` an unconfigured mod has not got, filled in from the `config.cpp` that declares
   * it. Nothing is written before the developer has seen what would go in it, and a developer who
   * says no leaves the disk as it was and the mod in the list, unconfigured.
   */
  async adopt(target: { mod: string } | undefined): Promise<void> {
    const found = await findMods();
    const mod = await modFor(target, found, {
      among: isUnconfigured,
      empty: `Every mod of this workspace has a ${MANIFEST_FILE} already.`,
      placeHolder: `Mod to write a ${MANIFEST_FILE} for`,
    });

    if (mod === undefined) {
      return;
    }

    // The main addon is the one that declares the mod, so its config is the one with the answers.
    const config = found.uris.get(mainAddonOf(mod)?.config ?? '');
    const adoption = adoptionOf(
      mod,
      config === undefined ? '' : await textOf(config),
      openFolders(),
    );

    if (adoption.refusal !== undefined) {
      this.log.warn(`adopt ${mod.name}: ${adoption.refusal}`);
      await vscode.window.showWarningMessage(adoption.refusal);
      return;
    }

    if (!(await confirmed(mod, adoption, config))) {
      this.log.info(`adopt ${mod.name}: declined, nothing written`);
      return;
    }

    const root = rootOf(mod, found);
    if (root === undefined) {
      await vscode.window.showErrorMessage(`${mod.name} could not be found on disk any more.`);
      return;
    }

    if (!(await this.write(root, adoption, `${MANIFEST_FILE} for ${mod.name}`))) {
      return;
    }

    this.log.info(`adopted ${mod.name} as ${adoption.fields.name}`);
    this.changed();

    // The manifest is what the mod is configured by, so it is what the developer is left looking
    // at: the fields it was filled in with are the ones worth reading over, and the form is where
    // they read as fields.
    await vscode.commands.executeCommand('vscode.open', vscode.Uri.joinPath(root, MANIFEST_FILE));

    // And onto the work drive, the way a new mod goes: an adopted mod that cannot be built until
    // another button is found is not a mod that was configured for the developer.
    await this.link(root, mod.name, `Configured ${mod.name}`);
  }

  /** A new addon of a mod that is laid out as several. */
  async addon(target: { mod: string } | undefined): Promise<void> {
    const found = await findMods();
    const mod = await modFor(target, found, {
      among: ANY,
      empty: 'No mod of this workspace can take an addon.',
      placeHolder: 'Mod to add an addon to',
    });

    if (mod === undefined) {
      return;
    }

    // Asked before the name is, so that a mod which takes no addon says so before a name is
    // thought of rather than after.
    const refusal = addonsRefusalOf(mod);
    if (refusal !== undefined) {
      this.log.warn(`add addon: ${refusal}`);
      await vscode.window.showWarningMessage(refusal);
      return;
    }

    const name = await askAddonName(mod);
    if (name === undefined) {
      return;
    }

    const plan = addonPlanOf(mod, name);
    if (plan.refusal !== undefined) {
      await vscode.window.showWarningMessage(plan.refusal);
      return;
    }

    const root = rootOf(mod, found);
    if (root === undefined) {
      await vscode.window.showErrorMessage(`${mod.name} could not be found on disk any more.`);
      return;
    }

    if (!(await this.write(root, plan, `${mod.name}\\${name}`))) {
      return;
    }

    await this.require(plan, found.uris);
    this.log.info(`added ${mod.name}\\${name}`);
    this.changed();

    // The config is what the addon is an addon by, so it is what the developer is left in.
    const config = plan.files.find((file) => file.path.endsWith(CONFIG_FILE));
    if (config !== undefined) {
      await vscode.window.showTextDocument(vscode.Uri.joinPath(root, ...config.path.split('/')));
    }
  }

  /** The plan carried out, unless something it would write is there already. */
  private async write(root: vscode.Uri, plan: InitPlan, what: string): Promise<boolean> {
    const existing = await existingOf(root, plan);
    if (existing.length > 0) {
      const message = `${what} was not made: ${existing.join(', ')} would be written over.`;
      this.log.warn(message);
      await vscode.window.showErrorMessage(message);
      return false;
    }

    try {
      await createFrom(root, plan);
      return true;
    } catch (error: unknown) {
      const message = error instanceof Error ? error.message : String(error);
      this.log.error(`could not make ${what}: ${message}`);
      await vscode.window.showErrorMessage(`Could not make ${what}: ${message}`);
      return false;
    }
  }

  /**
   * The new addon written into the main addon's `requiredAddons`. It is the whole point of the
   * command doing more than making a folder — an addon nothing requires is one the engine loads in
   * whatever order it likes — so where it does not happen, it is said.
   */
  private async require(plan: AddonPlan, uris: ReadonlyMap<string, vscode.Uri>): Promise<void> {
    const requirement = plan.requires;
    const config = requirement && uris.get(requirement.config);

    if (requirement === undefined || config === undefined) {
      if (plan.warning !== undefined) {
        this.log.warn(plan.warning);
        await vscode.window.showWarningMessage(plan.warning);
      }
      return;
    }

    try {
      if (await requireAddon(config, requirement)) {
        return;
      }

      await vscode.window.showWarningMessage(
        `${requirement.required} was made, but nothing requires it: ${requirement.patch} is no ` +
          `longer in ${vscode.workspace.asRelativePath(config, true)}.`,
      );
    } catch (error: unknown) {
      const message = error instanceof Error ? error.message : String(error);
      await vscode.window.showWarningMessage(
        `${requirement.required} was made, but could not be written into ${requirement.patch}: ${message}`,
      );
    }
  }

  /**
   * The mod put onto the work drive, which is what makes it buildable without another command
   * being found first. A drive that is down is not a failure of the initialisation: the mod is
   * made either way, and what is missing is said with the button that settles it.
   */
  private async link(root: vscode.Uri, name: string, done: string): Promise<void> {
    const prefixRoot = vscode.Uri.joinPath(root, name);
    const drive = await readWorkDrive(await readMachineSettings());
    const refusal = platformRefusal() ?? refusalOf(drive, 'link');

    if (refusal !== undefined) {
      this.log.warn(`link ${name}: ${refusal}`);
      await this.offerMount(`${done}, but it is not on the work drive. ${refusal}`, drive.state);
      return;
    }

    const links = await readLinks(drive, [
      { prefixRoot: prefixRoot.path, name, target: prefixRoot.fsPath },
    ]);

    try {
      await makeLinks(linksToMake(links));
    } catch (error: unknown) {
      const message = error instanceof Error ? error.message : String(error);
      this.log.error(`could not link ${name}: ${message}`);
      await vscode.window.showWarningMessage(`${done}, but could not link it: ${message}`);
      return;
    } finally {
      this.changed();
    }

    const path = linkPathOf(drive.letter, name);
    this.log.info(`linked ${name} as ${path}`);
    await vscode.window.showInformationMessage(`${done} and linked it as ${path}.`);
  }

  /** The one press that settles the commonest reason a new mod is not on the drive. */
  private async offerMount(message: string, state: WorkDriveState): Promise<void> {
    if (state !== 'unmounted') {
      await vscode.window.showWarningMessage(message);
      return;
    }

    const mount = 'Mount Work Drive';
    if ((await vscode.window.showWarningMessage(message, mount)) === mount) {
      await vscode.commands.executeCommand(WORK_DRIVE_COMMAND.mount);
    }
  }

  /**
   * What the mod is called. The folder it is being made in is offered as the answer, because a
   * repository cloned for one mod is usually named after it — and the same name is checked
   * against what is already in that folder, since the mod's own folder goes inside it.
   */
  private async askName(root: vscode.Uri): Promise<string | undefined> {
    // Off the `Uri` rather than off the workspace: a folder outside the open one has a relative
    // path that is not relative at all, and its last segment is not its name.
    const suggested = root.path.split('/').filter((segment) => segment !== '').at(-1) ?? '';

    const name = await vscode.window.showInputBox({
      title: 'New Enfusion mod',
      prompt:
        'The name of the mod: its folder, the P:\\<Name> it is linked as, the @<Name> it is ' +
        'loaded as, and the class its config declares.',
      value: modNameProblemOf(suggested) === undefined ? suggested : '',
      validateInput: (typed) => modNameProblemOf(typed),
    });

    return name?.trim();
  }
}

/**
 * How the mod packs. It is asked because it is the one thing a mod cannot be changed to later
 * without moving files: `config.cpp` in the prefix root packs the whole mod, and a `config.cpp`
 * per subfolder packs one pbo each.
 */
async function askLayout(name: string): Promise<Layout | undefined> {
  const items = [
    {
      label: 'One pbo for the whole mod',
      detail: `${name}\\config.cpp — everything under the prefix root packs into ${name}.pbo`,
      layout: 'single' as const,
    },
    {
      label: 'One pbo per addon',
      detail: `${name}\\Scripts\\config.cpp — each folder of the prefix root packs into its own pbo`,
      layout: 'multi' as const,
    },
  ];

  const picked = await vscode.window.showQuickPick(items, {
    title: `New Enfusion mod: ${name}`,
    placeHolder: 'How the mod is packed',
  });

  return picked?.layout;
}

async function askAddonName(mod: Mod): Promise<string | undefined> {
  const name = await vscode.window.showInputBox({
    title: `New addon of ${mod.name}`,
    prompt: `The addon's folder in the prefix root, which is also the name of its pbo.`,
    validateInput: (typed) =>
      addonNameProblemOf(typed) ??
      (mod.addons.some((addon) => addon.name.toLowerCase() === typed.trim().toLowerCase())
        ? `${mod.name} already has an addon called ${typed.trim()}.`
        : undefined),
  });

  return name?.trim();
}

/**
 * The one question adoption asks. What would be written is shown before it is written, because the
 * whole of the offer is these fields out of this file — and saying no writes nothing at all.
 */
async function confirmed(
  mod: Mod,
  adoption: Adoption,
  config: vscode.Uri | undefined,
): Promise<boolean> {
  const write = `Write ${MANIFEST_FILE}`;
  const answer = await vscode.window.showInformationMessage(
    `Configure ${mod.name} from its ${CONFIG_FILE}?`,
    { modal: true, detail: detailOf(adoption, config) },
    write,
  );

  return answer === write;
}

/** The fields as they were read, and where they were read from: the offer, in full. */
function detailOf(adoption: Adoption, config: vscode.Uri | undefined): string {
  const { name, version, description, author } = adoption.fields;
  const fields = [
    ['name', name],
    ['version', version],
    ['description', description],
    ['author', author],
  ] as const;

  const read = fields.flatMap(([field, value]) => (value === undefined ? [] : [`${field}: ${value}`]));
  const where = config === undefined ? CONFIG_FILE : vscode.workspace.asRelativePath(config, true);

  return (
    `${read.join('\n')}\n\n` +
    `Read out of ${where}. The only file written is ${MANIFEST_FILE} in the mod root — what is ` +
    'left blank can be filled in there afterwards — and the mod is put on the work drive, the ' +
    'way a new one is.'
  );
}

/**
 * The mod a command is about: the one the panel named, or the one picked from the list when the
 * command came from the palette with nothing to click.
 *
 * The panel's name is looked up among all the mods rather than among the ones worth offering, so
 * that a card gone stale — a mod configured since the panel drew it — is answered by whoever
 * refuses it, in the words of what it is now, instead of by "no longer a mod of this workspace".
 */
async function modFor(
  target: { mod: string } | undefined,
  found: Discovery,
  asking: Asking,
): Promise<Mod | undefined> {
  if (target === undefined) {
    return pickMod(found.mods.filter(asking.among), asking);
  }

  const mod = found.mods.find((candidate) => candidate.name === target.mod);
  if (mod === undefined) {
    await vscode.window.showWarningMessage(`${target.mod} is no longer a mod of this workspace.`);
  }

  return mod;
}

/** What a command offers when nothing was clicked, and how it says there is nothing to offer. */
interface Asking {
  /** The mods worth offering. */
  readonly among: (mod: Mod) => boolean;
  readonly empty: string;
  readonly placeHolder: string;
}

/** A mod found by its `config.cpp` alone, which is the only kind there is anything to adopt in. */
function isUnconfigured(mod: Mod): boolean {
  return mod.manifest === undefined;
}

/** Every mod is worth offering: the one that cannot take an addon says so when it is picked. */
function ANY(): boolean {
  return true;
}

/**
 * The folders the workspace has open, as the domain counts paths. A mod root outside every one of
 * them is a mod root nothing written into would ever be found in again.
 */
function openFolders(): string[] {
  return (vscode.workspace.workspaceFolders ?? []).map((folder) => folder.uri.path);
}

/**
 * The mod root as a `Uri`, borrowed from a file inside the mod: only files were searched for, so
 * the folder has none of its own, and building one from the path would assume the workspace is a
 * folder on this disk.
 */
function rootOf(mod: Mod, found: Discovery): vscode.Uri | undefined {
  return found.uris.get(mod.manifest ?? mod.addons[0]?.config ?? '')?.with({ path: mod.root });
}

/** From the palette there is nothing to click, so the list of mods is the question asked. */
async function pickMod(mods: readonly Mod[], asking: Asking): Promise<Mod | undefined> {
  if (mods.length === 0) {
    await vscode.window.showWarningMessage(asking.empty);
    return undefined;
  }

  if (mods.length === 1) {
    return mods[0];
  }

  const picked = await vscode.window.showQuickPick(
    mods.map((mod) => ({
      label: mod.name,
      description: vscode.workspace.asRelativePath(mod.root, true),
      mod,
    })),
    { placeHolder: asking.placeHolder },
  );

  return picked?.mod;
}

/**
 * Where a mod goes when nobody clicked a folder to say. One open folder is the answer; several is
 * a question, and none is the one case where there is nowhere to write at all.
 */
async function openFolder(): Promise<vscode.Uri | undefined> {
  const folders = vscode.workspace.workspaceFolders ?? [];

  if (folders.length === 0) {
    await vscode.window.showWarningMessage(
      'Open the folder the mod is to be made in, and then make it.',
    );
    return undefined;
  }

  if (folders.length === 1) {
    return folders[0]?.uri;
  }

  const picked = await vscode.window.showQuickPick(
    folders.map((folder) => ({ label: folder.name, description: folder.uri.fsPath, folder })),
    { placeHolder: 'Folder to make the mod in' },
  );

  return picked?.folder.uri;
}
