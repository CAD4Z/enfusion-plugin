/**
 * Run and Debug, the way a developer already knows it: F5 puts the game up, Stop takes it down.
 *
 * The configurations are handed over **dynamically**, out of the targets in the `.enf`, and no
 * `launch.json` is ever written. One written by hand is no use for configuring anything either: a
 * configuration of ours takes `type`, `request`, `target` and `build`, and any other field is
 * refused with a sentence pointing at the manifest. Stopping a file from being written is not something an
 * extension can do; making it pointless is. See
 * `docs/adr/0002-enf-is-the-only-project-configuration.md`.
 *
 * The debug adapter starts a process and kills it, and does nothing else — no breakpoints, no
 * stacks, no variables. What it buys over a command is what a developer gets for free around it:
 * the F5 they already press, the Stop button, and the session showing in the toolbar for as long
 * as the game is up.
 */

import * as vscode from 'vscode';
import {
  type LaunchRole,
  type LaunchTarget,
  filePatchingRootOf,
  launchPathsOf,
  launchPlanOf,
  runRootOf,
  targetById,
  targetsOf,
} from '../mods/launch';
import { type GameBuild, GAME_BUILDS, gameBuildOf } from '../mods/machine';
import {
  type LaunchAttempt,
  type LaunchExit,
  type LaunchGame,
  type LaunchSession,
  LaunchCoordinator,
  activeLaunchOf,
} from '../mods/launchSession';
import { MANIFEST_FILE } from '../mods/model';
import { windowsName } from '../mods/paths';
import { scriptDebugNoteOf, scriptDebugSaidOf } from '../mods/scriptDebug';
import { gamePrefixOf, sandboxPlanOf } from '../mods/sandbox';
import {
  localAppData,
  prepareLaunch,
  readFound,
  readGameRoot,
  readLinkFacts,
  startGame,
} from '../platform/launch';
import { readMachineSettings } from '../platform/machine';
import { openSandbox, sandboxedGame } from '../platform/sandbox';
import {
  type ScriptDebugHandler,
  type ScriptDebugPort,
  openScriptDebugPorts,
} from '../platform/scriptDebug';
import { readWorkDrive } from '../platform/workDrive';
import { findMods, launchModsOf, targetSourcesOf } from '../platform/workspace';

/** The debug type contributed in `package.json`; a configuration names it as `"type"`. */
export const LAUNCH_TYPE = 'enfusion';

export const LAUNCH_COMMAND = {
  select: 'enfusion.selectTarget',
  selectBuild: 'enfusion.selectBuild',
  start: 'enfusion.launch',
  secondClient: 'enfusion.launchSecondClient',
} as const;

/**
 * The request the second-client button turns into.
 *
 * It is asked of the session that is already running rather than done beside it, because the one
 * thing a second client is for — its log, beside the first one's — belongs in the console that
 * session owns, and nothing outside an adapter can write there.
 */
const SECOND_CLIENT_REQUEST = 'enfusionSecondClient';

/** The fields a configuration of ours takes. Anything else is the manifest's business. */
const CONFIGURATION_FIELDS: readonly string[] = [
  'type',
  'request',
  'name',
  'target',
  'build',
  'noDebug',
];

/** Where the chosen target is remembered, so a reopened workspace opens on the same one. */
const CHOSEN_KEY = 'enfusion.launch.target';

/** And the chosen build, beside it: both belong to the workspace rather than to the machine. */
const BUILD_KEY = 'enfusion.launch.build';

/** Registered as one thing, and told to look again whenever the mods can have changed. */
export interface Launching extends vscode.Disposable {
  refresh(): void;
  /**
   * What the next launch would put up, out of the targets whoever asks already has in hand.
   *
   * The panel shows the choice and the status bar shows the choice, and there is one of it: both
   * read it from here rather than from a memento key each of them knows the name of.
   */
  chosen(targets: readonly LaunchTarget[]): Chosen;
}

/** The target and the build the next launch uses. */
export interface Chosen {
  readonly target: LaunchTarget | undefined;
  readonly build: GameBuild;
}

export function registerLaunch(
  memento: vscode.Memento,
  log: vscode.LogOutputChannel,
  /** Called when the choice changes, so that whatever shows it says so without being asked. */
  onChosen: () => void,
): Launching {
  const launcher = new Launcher(log);
  const coordinator = new LaunchCoordinator();
  const bar = new LaunchBar(memento, launcher, onChosen);
  const configurations = new Configurations(launcher, bar);
  const started = new Started(log, coordinator);

  const disposable = vscode.Disposable.from(
    bar,
    // Twice on purpose: the dynamic registration is what fills the Run and Debug list, and the
    // ordinary one is what gets asked to resolve a configuration before it is launched.
    vscode.debug.registerDebugConfigurationProvider(
      LAUNCH_TYPE,
      configurations,
      vscode.DebugConfigurationProviderTriggerKind.Dynamic,
    ),
    vscode.debug.registerDebugConfigurationProvider(LAUNCH_TYPE, configurations),
    vscode.debug.registerDebugAdapterDescriptorFactory(LAUNCH_TYPE, {
      createDebugAdapterDescriptor: (session) =>
        new vscode.DebugAdapterInlineImplementation(
          new GameSession(
            targetOf(session.configuration),
            gameBuildOf(session.configuration.build),
            launcher,
            coordinator,
            log,
          ),
        ),
    }),
    // Both take what to choose, so that the panel's two lists set it directly rather than opening
    // a question a developer has already answered by picking from a list.
    vscode.commands.registerCommand(LAUNCH_COMMAND.select, (id?: unknown) => bar.choose(id)),
    vscode.commands.registerCommand(LAUNCH_COMMAND.selectBuild, (build?: unknown) =>
      bar.chooseBuild(build),
    ),
    vscode.commands.registerCommand(LAUNCH_COMMAND.start, () => started.start()),
    vscode.commands.registerCommand(LAUNCH_COMMAND.secondClient, () => addSecondClient()),
  );

  bar.refresh();

  return {
    dispose: () => {
      disposable.dispose();
    },
    refresh: () => {
      bar.refresh();
    },
    chosen: (targets) => bar.chosen(targets),
  };
}

/**
 * The panel's Start button, and the one launch it is allowed to have up.
 *
 * The button is the F5 a developer would have pressed rather than a second way of doing the same
 * thing: the very same configuration, resolved the very same way, so it puts up the target the
 * status bar shows, asks which one only where nothing is chosen, and shows in the debug toolbar
 * for as long as the game is up.
 *
 * One at a time, and not merely as a courtesy. Two launches of a workspace put two servers on the
 * one port and lay two sets of junctions into the one run folder. And a button that keeps the
 * keyboard focus is a button a held key presses again and again — which is how a single press
 * becomes a machine full of processes.
 *
 * The coordinator below is the authority for the panel button and Run and Debug alike. This class
 * only covers the short moment before the adapter exists and can claim that shared launch slot.
 */
class Started {
  /** The moment between asking for a session and being told it began, where nothing is up yet. */
  private starting = false;
  /** Whether the refusal has been shown, so a held key is not answered with a wall of them. */
  private refused = false;

  constructor(
    private readonly log: vscode.LogOutputChannel,
    private readonly coordinator: LaunchCoordinator,
  ) {}

  async start(): Promise<void> {
    if (this.starting || this.coordinator.busy) {
      this.log.warn('the game is already up; this launch was not started');
      await this.sayBusy();
      return;
    }

    this.refused = false;
    this.starting = true;

    try {
      await vscode.debug.startDebugging(vscode.workspace.workspaceFolders?.[0], {
        type: LAUNCH_TYPE,
        request: 'launch',
        name: 'Enfusion',
      });
    } finally {
      this.starting = false;
    }
  }

  private async sayBusy(): Promise<void> {
    if (this.refused) {
      return;
    }

    this.refused = true;
    await vscode.window.showWarningMessage(
      'The game is already up. Stop it before starting it again.',
    );
  }
}

/**
 * The second client, asked of the launch that is up.
 *
 * There has to be one for the asking to mean anything: a second client is started to sit beside a
 * game that is already playing, and it joins the server that launch put up. So a press with
 * nothing running is a sentence rather than a second launch of its own.
 */
async function addSecondClient(): Promise<void> {
  const session = vscode.debug.activeDebugSession;

  if (session?.type !== LAUNCH_TYPE) {
    await vscode.window.showWarningMessage(
      'A second client joins the launch that is already up, so there has to be one: press Start ' +
        'first, then this.',
    );
    return;
  }

  await session.customRequest(SECOND_CLIENT_REQUEST);
}

function targetOf(configuration: vscode.DebugConfiguration): string {
  const target: unknown = configuration.target;

  return typeof target === 'string' ? target : '';
}

/**
 * The Run and Debug list, and the gatekeeper of what a configuration may say.
 *
 * A configuration with no target in it is the ordinary case rather than a mistake: it is what F5
 * on a workspace with one target means, and what the status bar's choice is for. `build` is the
 * same: a configuration that names one launches that build whatever is chosen, and one that names
 * none launches the chosen one — which is what the list of targets below offers, so that the
 * ordinary F5 follows the panel rather than pinning a build the day it was written.
 */
class Configurations implements vscode.DebugConfigurationProvider {
  constructor(
    private readonly launcher: Launcher,
    private readonly bar: LaunchBar,
  ) {}

  async provideDebugConfigurations(): Promise<vscode.DebugConfiguration[]> {
    const targets = await this.launcher.targets();

    return targets.map((target) => ({
      type: LAUNCH_TYPE,
      request: 'launch',
      name: target.id,
      target: target.id,
    }));
  }

  async resolveDebugConfiguration(
    _folder: vscode.WorkspaceFolder | undefined,
    configuration: vscode.DebugConfiguration,
  ): Promise<vscode.DebugConfiguration | undefined> {
    const extra = Object.keys(configuration).filter(
      // The editor puts fields of its own into a configuration, and those are its to keep.
      (field) => !CONFIGURATION_FIELDS.includes(field) && !field.startsWith('__'),
    );

    if (extra.length > 0) {
      await vscode.window.showErrorMessage(
        `An Enfusion debug configuration takes "type", "request", "target" and "build", and this ` +
          `one also has ${extra.map((field) => `"${field}"`).join(', ')}. Everything about a ` +
          `launch is configured in ${MANIFEST_FILE}, not in launch.json.`,
      );
      return undefined;
    }

    const target = await this.targetFor(configuration);
    if (target === undefined) {
      return undefined;
    }

    const build =
      configuration.build === undefined
        ? this.bar.build()
        : gameBuildOf(configuration.build);

    // Remembered, so that the status bar and the panel show what is actually running: a launch
    // out of Run and Debug is as much a choice of target and build as picking them from a list is.
    this.bar.remember(target, build);

    return { type: LAUNCH_TYPE, request: 'launch', name: target.id, target: target.id, build };
  }

  /** The one it named, the one on the status bar, or — with several to pick from — the question. */
  private async targetFor(
    configuration: vscode.DebugConfiguration,
  ): Promise<LaunchTarget | undefined> {
    const targets = await this.launcher.targets();
    if (targets.length === 0) {
      await noTargets();
      return undefined;
    }

    const named = targetOf(configuration);
    if (named === '') {
      return this.bar.current(targets) ?? (await pick(targets));
    }

    const found = targetById(targets, named);
    if (found === undefined) {
      await vscode.window.showErrorMessage(
        `No launch target is called "${named}". This workspace has ` +
          `${targets.map((target) => `"${target.id}"`).join(', ')}, out of the "targets" of its ` +
          `${MANIFEST_FILE}.`,
      );
    }

    return found;
  }
}

/**
 * The chosen target and build, on the status bar and remembered between sessions.
 *
 * Both are shown rather than only offered, and for the one reason: the mistakes worth catching
 * here are launching the wrong map and launching the wrong build, and each of them costs a full
 * load of the game to find out about. The build especially — a `Release` launch that came up
 * without the change just made looks exactly like a mod that does not work.
 */
class LaunchBar implements vscode.Disposable {
  private readonly item: vscode.StatusBarItem;
  private targets: readonly LaunchTarget[] = [];

  constructor(
    private readonly memento: vscode.Memento,
    private readonly launcher: Launcher,
    private readonly onChosen: () => void,
  ) {
    this.item = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Left, 100);
    this.item.command = LAUNCH_COMMAND.select;
  }

  dispose(): void {
    this.item.dispose();
  }

  /** Reads the targets again, and keeps the choice pointing at one that still exists. */
  refresh(): void {
    void this.launcher.targets().then((targets) => {
      this.targets = targets;
      this.show();
    });
  }

  /** The one a launch with nothing named uses: the chosen one, or the only one there is. */
  current(targets: readonly LaunchTarget[]): LaunchTarget | undefined {
    const chosen = this.memento.get<string>(CHOSEN_KEY);
    const found = chosen === undefined ? undefined : targetById(targets, chosen);

    return found ?? (targets.length === 1 ? targets[0] : undefined);
  }

  /** The chosen build. A workspace that has never been asked launches the diag build. */
  build(): GameBuild {
    return gameBuildOf(this.memento.get(BUILD_KEY));
  }

  chosen(targets: readonly LaunchTarget[]): Chosen {
    return { target: this.current(targets), build: this.build() };
  }

  remember(target: LaunchTarget, build: GameBuild = this.build()): void {
    void this.memento.update(CHOSEN_KEY, target.id);
    void this.memento.update(BUILD_KEY, build);
    this.refresh();
    this.onChosen();
  }

  /**
   * Which target the next F5 puts up: the one named, or — where nothing named one, which is the
   * status bar being clicked — the one picked out of the list.
   */
  async choose(id?: unknown): Promise<void> {
    const targets = await this.launcher.targets();
    this.targets = targets;

    if (targets.length === 0) {
      this.show();
      await noTargets();
      return;
    }

    const named = typeof id === 'string' ? targetById(targets, id) : undefined;
    const picked = named ?? (typeof id === 'string' ? undefined : await pick(targets));
    if (picked !== undefined) {
      this.remember(picked);
    }
  }

  /** And which build. The panel's list names one; the palette asks. */
  async chooseBuild(build?: unknown): Promise<void> {
    const named = GAME_BUILDS.find((known) => known === build);
    const picked = named ?? (build === undefined ? await pickBuild() : undefined);
    if (picked === undefined) {
      return;
    }

    await this.memento.update(BUILD_KEY, picked);
    this.refresh();
    this.onChosen();
  }

  private show(): void {
    if (this.targets.length === 0) {
      this.item.hide();
      return;
    }

    const target = this.current(this.targets);
    const build = this.build();
    this.item.text = `$(rocket) ${target?.id ?? 'Select target'} · ${build}`;
    this.item.tooltip =
      target === undefined
        ? `Pick the Enfusion target to launch, as a ${build} build`
        : `Enfusion: ${target.mod}${target.map === undefined ? '' : `, ${target.map}`} — ` +
          `${describeRun(target.run)}, ${describeBuild(build)}`;
    this.item.show();
  }
}

/** What a target puts up, in the words the status bar, the palette and the panel all use. */
export function describeRun(run: LaunchTarget['run']): string {
  switch (run) {
    case 'client':
      return 'the client alone';
    case 'server':
      return 'the server alone';
    case 'both':
      return 'the server and a client';
  }
}

/** What each build is, in one line: the two differ in which game runs and what it reads. */
export function describeBuild(build: GameBuild): string {
  return build === 'Debug'
    ? 'the diag build, patched from the sources'
    : 'the retail build, out of the packed pbo';
}

async function pick(targets: readonly LaunchTarget[]): Promise<LaunchTarget | undefined> {
  const items = targets.map((target) => ({
    label: target.id,
    description: target.mod,
    detail: `${describeRun(target.run)}${target.map === undefined ? '' : ` · ${target.map}`}`,
    target,
  }));

  return (await vscode.window.showQuickPick(items, { placeHolder: 'Target to launch' }))?.target;
}

async function pickBuild(): Promise<GameBuild | undefined> {
  const items = GAME_BUILDS.map((build) => ({ label: build, detail: describeBuild(build), build }));

  return (await vscode.window.showQuickPick(items, { placeHolder: 'Build to launch' }))?.build;
}

async function noTargets(): Promise<void> {
  await vscode.window.showWarningMessage(
    `Nothing to launch: give ${MANIFEST_FILE} a "launch" block with "targets" in it, and they ` +
      'show up in Run and Debug by themselves.',
  );
}

/**
 * Everything a launch does between the button and the process. Read afresh every time rather than
 * kept: a developer who mounted the work drive or built a mod a second ago is exactly the case a
 * remembered answer gets wrong.
 */
class Launcher {
  constructor(private readonly log: vscode.LogOutputChannel) {}

  async targets(): Promise<LaunchTarget[]> {
    return targetsOf(targetSourcesOf(await findMods()));
  }

  /** One atomic running launch, or the refusal that acquired nothing. */
  async start(
    id: string,
    build: GameBuild,
    say: (text: string) => void,
    signal: AbortSignal,
  ): Promise<LaunchAttempt> {
    const games: LaunchGame[] = [];
    let listening: readonly ScriptDebugPort[] = [];

    signal.throwIfAborted();

    // A launch is one of the two things that puts a path out of a `mod.enf` on a command line, so
    // like a build it waits until the developer has said the folder is theirs.
    if (!vscode.workspace.isTrusted) {
      return {
        kind: 'refused',
        message:
          `Launching starts the game with paths out of this workspace’s ${MANIFEST_FILE}, so it ` +
          'needs the workspace to be trusted.',
      };
    }

    try {
      const [discovery, settings] = await Promise.all([findMods(), readMachineSettings()]);
      signal.throwIfAborted();

      const target = targetById(targetsOf(targetSourcesOf(discovery)), id);
      if (target === undefined) {
        return { kind: 'refused', message: `No launch target is called "${id}" any more.` };
      }

      const mods = launchModsOf(discovery);
      const runRoot = runRootOf(
        settings.filePatchingRoot,
        localAppData(),
        vscode.workspace.name ?? '',
      );
      const [drive, game, present, found] = await Promise.all([
        readWorkDrive(settings),
        readGameRoot(settings, build),
        readLinkFacts(filePatchingRootOf(runRoot)),
        // What the plan wants a yes or a no about — the pbo, the `server.cfg`, the mission — asked
        // for by the plan itself, so that the two can never go looking at different paths.
        readFound(launchPathsOf(target, mods)),
      ]);
      signal.throwIfAborted();

      // Opened before the plan rather than after it, because the plan puts their numbers on the
      // command lines it writes. A launch that is then refused closes them again.
      listening = await this.listen(say);
      signal.throwIfAborted();
      const plan = launchPlanOf({
        target,
        build,
        mods,
        settings,
        drive,
        runRoot,
        game,
        present,
        found,
        debugPorts: portsOf(listening),
      });

      if (plan.refusals.length > 0) {
        await this.rollback(games, listening);
        return { kind: 'refused', message: plan.refusals.join(' ') };
      }

      for (const warning of plan.warnings) {
        this.log.warn(warning);
        say(warning);
      }

      signal.throwIfAborted();
      await prepareLaunch(plan);
      signal.throwIfAborted();
      this.log.info(
        `launch: ${build}, ${plan.filePatching.junctions.length} link(s) made, ` +
          `${plan.filePatching.remove.length} taken off, ${plan.copies.length} layer(s) laid down, ` +
          `in ${runRoot}`,
      );

      if (plan.processes.length === 0) {
        await this.rollback(games, listening);
        return { kind: 'refused', message: `${target.id} puts nothing up.` };
      }

      for (const process_ of plan.processes) {
        signal.throwIfAborted();
        const command = `${process_.program} ${process_.arguments.join(' ')}`;
        this.log.info(command);
        say(command);
        games.push(await startGame(process_));
      }

      signal.throwIfAborted();
      return { kind: 'started', launch: activeLaunchOf(games, listening) };
    } catch (error: unknown) {
      await this.rollback(games, listening);
      throw error;
    }
  }

  /**
   * A second client for a launch that is already up: the same target, its own profile, its own
   * debugger port, and the sandbox that gives it a Steam of its own.
   *
   * Everything it needs is read afresh rather than remembered from the launch it joins — the
   * settings can have been edited since, and the run folder it is started in is the one on disk
   * rather than the one the plan described an hour ago.
   *
   * The sandbox is brought up before anything else is made ready, and that order is the point.
   * Making the box and waiting for a Steam to sign in takes as long as a developer takes to type a
   * password, and neither the debugger ports nor the links in the run folder should be held open
   * across that: what is opened here is opened for a client that is about to start.
   */
  async startSecond(
    id: string,
    build: GameBuild,
    say: (text: string) => void,
    signal: AbortSignal,
  ): Promise<LaunchAttempt> {
    const games: LaunchGame[] = [];
    let listening: readonly ScriptDebugPort[] = [];

    signal.throwIfAborted();

    if (!vscode.workspace.isTrusted) {
      return {
        kind: 'refused',
        message: `Starting a second client puts paths out of this workspace’s ${MANIFEST_FILE} on a
          command line, so it needs the workspace to be trusted.`.replace(/\s+/g, ' '),
      };
    }

    try {
      const [discovery, settings] = await Promise.all([findMods(), readMachineSettings()]);
      signal.throwIfAborted();

      const target = targetById(targetsOf(targetSourcesOf(discovery)), id);
      if (target === undefined) {
        return { kind: 'refused', message: `No launch target is called "${id}" any more.` };
      }

      // Where a machine is set up for two accounts, the box has to exist and its Steam has to be
      // signed in before there is anywhere to start a client; where it is not, a second client is
      // simply another client and there is nothing to put in front of it.
      const sandbox = sandboxPlanOf(settings.secondClient);
      if (sandbox.kind === 'wanting') {
        return { kind: 'refused', message: sandbox.said };
      }

      if (sandbox.kind === 'box') {
        const failed = await openSandbox(sandbox.sandbox, say, signal);
        signal.throwIfAborted();
        if (failed !== undefined) {
          return { kind: 'refused', message: failed };
        }
      }

      const prefix = sandbox.kind === 'box' ? gamePrefixOf(sandbox.sandbox) : [];
      const mods = launchModsOf(discovery);
      const runRoot = runRootOf(
        settings.filePatchingRoot,
        localAppData(),
        vscode.workspace.name ?? '',
      );
      const [drive, game, present, found] = await Promise.all([
        readWorkDrive(settings),
        readGameRoot(settings, build),
        readLinkFacts(filePatchingRootOf(runRoot)),
        readFound(launchPathsOf(target, mods)),
      ]);
      signal.throwIfAborted();

      listening = await this.listen(say, ['client2']);
      signal.throwIfAborted();
      const plan = launchPlanOf(
        {
          target,
          build,
          mods,
          settings,
          drive,
          runRoot,
          game,
          present,
          found,
          debugPorts: portsOf(listening),
        },
        ['client2'],
      );

      if (plan.refusals.length > 0) {
        await this.rollback(games, listening);
        return { kind: 'refused', message: plan.refusals.join(' ') };
      }

      const process_ = plan.processes[0];
      if (process_ === undefined) {
        await this.rollback(games, listening);
        return { kind: 'refused', message: `${target.id} puts no second client up.` };
      }

      signal.throwIfAborted();
      await prepareLaunch(plan);
      signal.throwIfAborted();

      const command = [...prefix, process_.program, ...process_.arguments].join(' ');
      this.log.info(command);
      say(command);

      const started = await startGame(process_, prefix);
      games.push(
        sandbox.kind === 'box'
          ? sandboxedGame(started, sandbox.sandbox, windowsName(process_.program))
          : started,
      );
      signal.throwIfAborted();

      return { kind: 'started', launch: activeLaunchOf(games, listening) };
    } catch (error: unknown) {
      await this.rollback(games, listening);
      throw error;
    }
  }

  /** Rolls back handles that have not yet been transferred to a `LaunchSession`. */
  private async rollback(
    games: readonly LaunchGame[],
    listening: readonly ScriptDebugPort[],
  ): Promise<void> {
    for (const failure of await activeLaunchOf(games, listening).stop()) {
      this.log.error(`launch cleanup failed: ${messageOf(failure)}`);
    }
  }

  /**
   * A listener for each role, whatever this launch turns out to put up.
   *
   * Both, rather than only the ones the target runs: a bound loopback socket nobody dials costs
   * nothing, and the alternative is the plan having to be built before the ports are open and the
   * ports having to be open before the plan is built.
   *
   * A role whose listener could not be opened is given no port at all, which the game reads as a
   * port to fail at connecting to — every few seconds, quietly, for as long as it runs. That is
   * the whole of the damage, so it is said once and the launch goes ahead.
   */
  private async listen(
    say: (text: string) => void,
    roles: readonly LaunchRole[] = ROLES,
  ): Promise<ScriptDebugPort[]> {
    const handler = handlerFor(say);
    const opened = await openScriptDebugPorts(roles, handler);

    for (const warning of opened.warnings) {
      this.log.warn(warning);
      say(warning);
    }

    return [...opened.listening];
  }
}

const ROLES: readonly LaunchRole[] = ['client', 'server'];

/**
 * The console, as the script debugger's reader wants it: lines with the role in front of them, in
 * the role's colour. The colour is the whole point — a launch that puts up both interleaves two
 * games in the one console, and telling them apart is what a developer reading it is doing.
 */
function handlerFor(say: (text: string) => void): ScriptDebugHandler {
  return {
    said: (role, text) => {
      for (const line of scriptDebugSaidOf(role, text)) {
        say(line);
      }
    },
    note: (role, note) => {
      say(scriptDebugNoteOf(role, note));
    },
  };
}

/** A role with no listener is given no port, and the game spends the launch failing to dial it. */
function portsOf(listening: readonly ScriptDebugPort[]): Record<LaunchRole, number> {
  const ports: Record<LaunchRole, number> = { client: NO_PORT, server: NO_PORT, client2: NO_PORT };

  for (const port of listening) {
    ports[port.role] = port.port;
  }

  return ports;
}

/** Not a port anything listens on, which is what a game is told when nothing is listening. */
const NO_PORT = 0;

/** One request of the debug protocol, which is all of it this adapter reads. */
interface DapRequest {
  readonly seq: number;
  readonly type: string;
  readonly command: string;
}

/**
 * The debug adapter: start on `launch`, kill on `terminate` or `disconnect`, and end the session
 * when the game goes away on its own. Every other request is answered so that the editor is not
 * left waiting, and none of them does anything — there is nothing here to step through.
 *
 * A launch is one session however many primary processes it put up, so Stop takes down every one
 * of them. And when any primary process goes, so do its siblings: a client whose server has gone
 * has nobody to talk to, and a server whose client is gone would otherwise be left running with
 * nothing in the editor to show it. An optional second client may leave without ending the launch.
 */
class GameSession implements vscode.DebugAdapter {
  private readonly messages = new vscode.EventEmitter<vscode.DebugProtocolMessage>();
  readonly onDidSendMessage = this.messages.event;

  private sequence = 1;
  private readonly lifecycle: LaunchSession;
  private terminated = false;
  private disposed = false;

  constructor(
    private readonly target: string,
    private readonly build: GameBuild,
    private readonly launcher: Launcher,
    coordinator: LaunchCoordinator,
    private readonly log: vscode.LogOutputChannel,
  ) {
    this.lifecycle = coordinator.session({
      ended: (exit) => {
        this.output(exitMessageOf(exit));
        this.terminate();
      },
      secondEnded: (exit) => {
        this.output(`${exitMessageOf(exit)} The launch it joined is still up.`);
      },
      cleanupFailed: (error) => {
        this.log.error(`Launch cleanup failed: ${messageOf(error)}`);
      },
    });
  }

  handleMessage(message: vscode.DebugProtocolMessage): void {
    const request = message as DapRequest;
    if (request.type !== 'request') {
      return;
    }

    void this.handle(request).catch((error: unknown) => {
      void this.failed(request, error);
    });
  }

  dispose(): void {
    this.disposed = true;
    void this.lifecycle.stop();
    this.messages.dispose();
  }

  private async handle(request: DapRequest): Promise<void> {
    switch (request.command) {
      case 'initialize':
        this.respond(request, {
          supportsConfigurationDoneRequest: true,
          supportsTerminateRequest: true,
          // The script log arrives with the role in front of it in the role's colour, and this is
          // what makes the editor read those escapes rather than print them.
          supportsANSIStyling: true,
        });
        this.event('initialized');
        return;
      case 'launch':
        await this.launch(request);
        return;
      case SECOND_CLIENT_REQUEST:
        await this.second(request);
        return;
      // Answered as empty rather than left to the default: a breakpoint set in some other file
      // is still handed to whichever session is running, and a response with no body at all is
      // one the editor has no shape for.
      case 'setBreakpoints':
        this.respond(request, { breakpoints: [] });
        return;
      case 'threads':
        this.respond(request, { threads: [] });
        return;
      case 'terminate':
      case 'disconnect':
        await this.lifecycle.stop();
        this.respond(request);
        this.terminate();
        return;
      default:
        this.respond(request);
        return;
    }
  }

  private async launch(request: DapRequest): Promise<void> {
    const outcome = await this.lifecycle.start((signal) =>
      this.launcher.start(this.target, this.build, (text) => this.output(text), signal),
    );

    switch (outcome.kind) {
      case 'started':
        this.respond(request);
        return;
      case 'refused':
        this.log.warn(outcome.message);
        this.fail(request, outcome.message);
        this.terminate();
        return;
      case 'failed':
        this.log.error(messageOf(outcome.error));
        this.fail(request, messageOf(outcome.error));
        this.terminate();
        return;
      case 'busy':
        this.fail(
          request,
          'Another Enfusion launch is already starting or running in this workspace.',
        );
        this.terminate();
        return;
      case 'already-started':
        this.fail(request, 'This debug session has already started its launch.');
        return;
      case 'inactive':
        this.fail(request, 'The launch was stopped before it finished starting.');
        return;
    }
  }

  /**
   * A second client, added to this launch.
   *
   * It joins the session rather than starting one of its own: Stop takes it down with everything
   * else and its log arrives in the same console under its own prefix. Its own exit only frees the
   * second-client slot; a later press can add a replacement to the launch that is still running.
   */
  private async second(request: DapRequest): Promise<void> {
    const outcome = await this.lifecycle.addSecond((signal) =>
      this.launcher.startSecond(this.target, this.build, (text) => this.output(text), signal),
    );

    switch (outcome.kind) {
      case 'started':
      case 'inactive':
        this.respond(request);
        return;
      case 'refused':
        await this.refuseSecond(request, outcome.message);
        return;
      case 'failed':
        await this.refuseSecond(request, messageOf(outcome.error));
        return;
      case 'busy':
      case 'already-started':
        await this.refuseSecond(request, 'A second client is already starting or running.');
        return;
    }
  }

  /** A second client is optional, so refusing it must leave the primary launch alone. */
  private async refuseSecond(request: DapRequest, message: string): Promise<void> {
    this.log.warn(message);
    this.output(message);
    try {
      await vscode.window.showWarningMessage(message);
    } catch (error: unknown) {
      // The notification is optional UI around an optional client. A host failure showing it must
      // not reach `failed`, whose deliberately terminal cleanup belongs to protocol failures.
      this.log.error(`Could not show the second-client warning: ${messageOf(error)}`);
    }
    this.respond(request);
  }

  /** An unexpected primary protocol failure still travels through the terminal cleanup path. */
  private async failed(request: DapRequest, error: unknown): Promise<void> {
    const message = messageOf(error);
    this.log.error(message);

    if (request.command === SECOND_CLIENT_REQUEST) {
      // A second client remains optional even when a future integration failure escapes `second`.
      this.output(message);
      this.fail(request, message);
      return;
    }

    await this.lifecycle.stop();
    this.fail(request, message);
    this.terminate();
  }

  private terminate(): void {
    if (this.terminated || this.disposed) {
      return;
    }

    this.terminated = true;
    this.event('terminated');
  }

  private respond(request: DapRequest, body?: object): void {
    this.send({ type: 'response', request_seq: request.seq, success: true, command: request.command, body });
  }

  /** A failed `launch` is how a refusal reaches the developer: the editor shows it as it is. */
  private fail(request: DapRequest, message: string): void {
    this.send({
      type: 'response',
      request_seq: request.seq,
      success: false,
      command: request.command,
      message,
    });
  }

  private event(event: string, body?: object): void {
    this.send({ type: 'event', event, body });
  }

  private output(text: string): void {
    this.event('output', { category: 'console', output: `${text}\n` });
  }

  private send(message: object): void {
    if (this.disposed) {
      return;
    }

    this.messages.fire({ ...message, seq: this.sequence++ });
  }
}

function messageOf(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

function exitMessageOf(exit: LaunchExit): string {
  const role = exit.role === 'client2' ? 'second client' : exit.role;

  switch (exit.outcome.kind) {
    case 'code':
      return `The ${role} exited with ${exit.outcome.code}.`;
    case 'signal':
      return `The ${role} exited after ${exit.outcome.signal}.`;
    case 'unknown':
      return `The ${role} is gone.`;
  }
}
