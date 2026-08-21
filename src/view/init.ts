/**
 * The two commands that make something: a mod, and an addon of one.
 *
 * The mod is made where the developer asked for it — the folder clicked in the explorer, or the
 * open folder when the command came from the palette or from an empty panel — and it is asked two
 * things and no more: what it is called, and whether it packs into one pbo or one per addon.
 * Everything else about it is worked out from the name in `src/mods/init.ts`, which is also where
 * both refusals are worded.
 *
 * A mod that cannot be built until something is set up by hand is not a mod that was made for the
 * developer, so the last thing this does is put the new mod on the work drive: it is a build away
 * from being a pbo the moment the wizard closes.
 */

import * as vscode from 'vscode';
import {
  type AddonPlan,
  type InitPlan,
  addonNameProblemOf,
  addonPlanOf,
  addonsRefusalOf,
  initPlanOf,
  modNameProblemOf,
} from '../mods/init';
import { CONFIG_FILE, MANIFEST_FILE, type Layout, type Mod } from '../mods/model';
import { type WorkDriveState, linkPathOf, linksToMake, refusalOf } from '../mods/workDrive';
import { createFrom, existingOf, folderAt, holds, requireAddon } from '../platform/init';
import { readMachineSettings } from '../platform/machine';
import { makeLinks, platformRefusal, readLinks, readWorkDrive } from '../platform/workDrive';
import { findMods } from '../platform/workspace';
import { WORK_DRIVE_COMMAND } from './workDrive';

/** The command ids, which the palette, the explorer's menu and the panel all go through. */
export const INIT_COMMAND = {
  mod: 'enfusion.init',
  addon: 'enfusion.addon.add',
} as const;

/** Registers both, and calls back after each so the panel shows what was made. */
export function registerInitCommands(
  log: vscode.LogOutputChannel,
  changed: () => void,
): vscode.Disposable {
  const commands = new InitCommands(log, changed);

  return vscode.Disposable.from(
    vscode.commands.registerCommand(INIT_COMMAND.mod, (where?: vscode.Uri) => commands.mod(where)),
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

    // The manifest is what the mod is configured by, so it is what the developer is left looking at.
    await vscode.window.showTextDocument(vscode.Uri.joinPath(root, MANIFEST_FILE));
    await this.link(root, name);
  }

  /** A new addon of a mod that is laid out as several. */
  async addon(target: { mod: string } | undefined): Promise<void> {
    const found = await findMods();
    const mod =
      target === undefined
        ? await pickMod(found.mods)
        : found.mods.find((candidate) => candidate.name === target.mod);

    if (mod === undefined) {
      if (target !== undefined) {
        await vscode.window.showWarningMessage(
          `${target.mod} is no longer a mod of this workspace.`,
        );
      }
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

    const root = found.uris.get(mod.manifest ?? mod.addons[0]?.config ?? '')?.with({
      path: mod.root,
    });
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
  private async link(root: vscode.Uri, name: string): Promise<void> {
    const prefixRoot = vscode.Uri.joinPath(root, name);
    const drive = await readWorkDrive(await readMachineSettings());
    const refusal = platformRefusal() ?? refusalOf(drive, 'link');

    if (refusal !== undefined) {
      this.log.warn(`link ${name}: ${refusal}`);
      await this.offerMount(`${name} was made, but it is not on the work drive. ${refusal}`, drive.state);
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
      await vscode.window.showWarningMessage(`${name} was made, but could not be linked: ${message}`);
      return;
    } finally {
      this.changed();
    }

    const path = linkPathOf(drive.letter, name);
    this.log.info(`linked ${name} as ${path}`);
    await vscode.window.showInformationMessage(`Made ${name} and linked it as ${path}.`);
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

/** From the palette there is nothing to click, so the list of mods is the question asked. */
async function pickMod(mods: readonly Mod[]): Promise<Mod | undefined> {
  if (mods.length === 0) {
    await vscode.window.showWarningMessage('No mod of this workspace can take an addon.');
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
    { placeHolder: 'Mod to add an addon to' },
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
