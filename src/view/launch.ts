/**
 * Run and Debug, the way a developer already knows it: F5 puts the game up, Stop takes it down.
 *
 * The configurations are handed over **dynamically**, out of the targets in the `.enf`, and no
 * `launch.json` is ever written. One written by hand is no use for configuring anything either: a
 * configuration of ours takes `type`, `request` and `target`, and any other field is refused with
 * a sentence pointing at the manifest. Stopping a file from being written is not something an
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
import { MANIFEST_FILE } from '../mods/model';
import { windowsName } from '../mods/paths';
import { scriptDebugNoteOf, scriptDebugSaidOf } from '../mods/scriptDebug';
import { gamePrefixOf, sandboxPlanOf } from '../mods/sandbox';
import {
  type GameProcess,
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
  openScriptDebugPort,
} from '../platform/scriptDebug';
import { readWorkDrive } from '../platform/workDrive';
import { findMods, launchModsOf, targetSourcesOf } from '../platform/workspace';

/** The debug type contributed in `package.json`; a configuration names it as `"type"`. */
export const LAUNCH_TYPE = 'enfusion';

export const LAUNCH_COMMAND = {
  select: 'enfusion.selectTarget',
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
const CONFIGURATION_FIELDS: readonly string[] = ['type', 'request', 'name', 'target', 'noDebug'];

/** Where the chosen target is remembered, so a reopened workspace opens on the same one. */
const CHOSEN_KEY = 'enfusion.launch.target';

/** Registered as one thing, and told to look again whenever the mods can have changed. */
export interface Launching extends vscode.Disposable {
  refresh(): void;
}

export function registerLaunch(
  memento: vscode.Memento,
  log: vscode.LogOutputChannel,
): Launching {
  const launcher = new Launcher(log);
  const bar = new TargetBar(memento, launcher);
  const configurations = new Configurations(launcher, bar);
  const started = new Started(log);

  const disposable = vscode.Disposable.from(
    vscode.debug.onDidStartDebugSession((session) => started.began(session)),
    vscode.debug.onDidTerminateDebugSession((session) => started.ended(session)),
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
          new GameSession(targetOf(session.configuration), launcher, log),
        ),
    }),
    vscode.commands.registerCommand(LAUNCH_COMMAND.select, () => bar.choose()),
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
 * Run and Debug is left alone: a developer who starts a second session from the debug toolbar has
 * said so twice, and stopping them is not this button's business.
 */
class Started {
  private readonly sessions = new Set<string>();
  /** The moment between asking for a session and being told it began, where nothing is up yet. */
  private starting = false;
  /** Whether the refusal has been shown, so a held key is not answered with a wall of them. */
  private refused = false;

  constructor(private readonly log: vscode.LogOutputChannel) {}

  began(session: vscode.DebugSession): void {
    if (session.type === LAUNCH_TYPE) {
      this.sessions.add(session.id);
    }
  }

  ended(session: vscode.DebugSession): void {
    this.sessions.delete(session.id);
    if (this.sessions.size === 0) {
      this.refused = false;
    }
  }

  async start(): Promise<void> {
    if (this.starting || this.sessions.size > 0) {
      this.log.warn('the game is already up; this launch was not started');
      await this.sayBusy();
      return;
    }

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
 * on a workspace with one target means, and what the status bar's choice is for.
 */
class Configurations implements vscode.DebugConfigurationProvider {
  constructor(
    private readonly launcher: Launcher,
    private readonly bar: TargetBar,
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
        `An Enfusion debug configuration takes "type", "request" and "target", and this one also ` +
          `has ${extra.map((field) => `"${field}"`).join(', ')}. Everything about a launch is ` +
          `configured in ${MANIFEST_FILE}, not in launch.json.`,
      );
      return undefined;
    }

    const target = await this.targetFor(configuration);
    if (target === undefined) {
      return undefined;
    }

    this.bar.remember(target);

    return { type: LAUNCH_TYPE, request: 'launch', name: target.id, target: target.id };
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
 * The chosen target, on the status bar and remembered between sessions.
 *
 * It is shown rather than only offered because the one mistake worth catching here is launching
 * the wrong map — which costs a full load of the game to find out about.
 */
class TargetBar implements vscode.Disposable {
  private readonly item: vscode.StatusBarItem;
  private targets: readonly LaunchTarget[] = [];

  constructor(
    private readonly memento: vscode.Memento,
    private readonly launcher: Launcher,
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

  remember(target: LaunchTarget): void {
    void this.memento.update(CHOSEN_KEY, target.id);
    this.refresh();
  }

  /** The command behind the status bar: which target the next F5 puts up. */
  async choose(): Promise<void> {
    const targets = await this.launcher.targets();
    this.targets = targets;

    if (targets.length === 0) {
      this.show();
      await noTargets();
      return;
    }

    const picked = await pick(targets);
    if (picked !== undefined) {
      this.remember(picked);
    }
  }

  private show(): void {
    if (this.targets.length === 0) {
      this.item.hide();
      return;
    }

    const target = this.current(this.targets);
    this.item.text = `$(rocket) ${target?.id ?? 'Select target'}`;
    this.item.tooltip =
      target === undefined
        ? 'Pick the Enfusion target to launch'
        : `Enfusion: ${target.mod}${target.map === undefined ? '' : `, ${target.map}`} — ` +
          `${describe(target.run)}`;
    this.item.show();
  }
}

function describe(run: LaunchTarget['run']): string {
  switch (run) {
    case 'client':
      return 'the client alone';
    case 'server':
      return 'the server alone';
    case 'both':
      return 'the server and a client';
  }
}

async function pick(targets: readonly LaunchTarget[]): Promise<LaunchTarget | undefined> {
  const items = targets.map((target) => ({
    label: target.id,
    description: target.mod,
    detail: `${describe(target.run)}${target.map === undefined ? '' : ` · ${target.map}`}`,
    target,
  }));

  return (await vscode.window.showQuickPick(items, { placeHolder: 'Target to launch' }))?.target;
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

  /** The processes of the launch, running — or the sentence saying why none of them is. */
  async start(id: string, say: (text: string) => void): Promise<Launched | string> {
    // A launch is one of the two things that puts a path out of a `mod.enf` on a command line, so
    // like a build it waits until the developer has said the folder is theirs.
    if (!vscode.workspace.isTrusted) {
      return (
        `Launching starts the game with paths out of this workspace’s ${MANIFEST_FILE}, so it ` +
        'needs the workspace to be trusted.'
      );
    }

    const [discovery, settings] = await Promise.all([findMods(), readMachineSettings()]);
    const target = targetById(targetsOf(targetSourcesOf(discovery)), id);
    if (target === undefined) {
      return `No launch target is called "${id}" any more.`;
    }

    const mods = launchModsOf(discovery);
    const runRoot = runRootOf(
      settings.filePatchingRoot,
      localAppData(),
      vscode.workspace.name ?? '',
    );
    const [drive, game, present, found] = await Promise.all([
      readWorkDrive(settings),
      readGameRoot(settings),
      readLinkFacts(filePatchingRootOf(runRoot)),
      // What the plan wants a yes or a no about — the pbo, the `server.cfg`, the mission — asked
      // for by the plan itself, so that the two can never go looking at different paths.
      readFound(launchPathsOf(target, mods)),
    ]);

    // Opened before the plan rather than after it, because the plan puts their numbers on the
    // command lines it writes. A launch that is then refused closes them again.
    const listening = await this.listen(say);
    const plan = launchPlanOf({
      target,
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
      close(listening);
      return plan.refusals.join(' ');
    }

    for (const warning of plan.warnings) {
      this.log.warn(warning);
      say(warning);
    }

    await prepareLaunch(plan);
    this.log.info(
      `launch: ${plan.filePatching.junctions.length} link(s) made, ` +
        `${plan.filePatching.remove.length} taken off, ${plan.copies.length} layer(s) laid down, ` +
        `in ${runRoot}`,
    );

    if (plan.processes.length === 0) {
      close(listening);
      return `${target.id} puts nothing up.`;
    }

    const games = plan.processes.map((process_) => {
      const command = `${process_.program} ${process_.arguments.join(' ')}`;
      this.log.info(command);
      say(command);

      return startGame(process_);
    });

    return { games, listening };
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
  async startSecond(id: string, say: (text: string) => void): Promise<Launched | string> {
    if (!vscode.workspace.isTrusted) {
      return `Starting a second client puts paths out of this workspace’s ${MANIFEST_FILE} on a
        command line, so it needs the workspace to be trusted.`.replace(/\s+/g, ' ');
    }

    const [discovery, settings] = await Promise.all([findMods(), readMachineSettings()]);
    const target = targetById(targetsOf(targetSourcesOf(discovery)), id);
    if (target === undefined) {
      return `No launch target is called "${id}" any more.`;
    }

    // Where a machine is set up for two accounts, the box has to exist and its Steam has to be
    // signed in before there is anywhere to start a client; where it is not, a second client is
    // simply another client and there is nothing to put in front of it.
    const sandbox = sandboxPlanOf(settings.secondClient);
    if (sandbox.kind === 'wanting') {
      return sandbox.said;
    }

    if (sandbox.kind === 'box') {
      const failed = await openSandbox(sandbox.sandbox, say);
      if (failed !== undefined) {
        return failed;
      }
    }

    const prefix = sandbox.kind === 'box' ? gamePrefixOf(sandbox.sandbox) : [];
    const mods = launchModsOf(discovery);
    const runRoot = runRootOf(settings.filePatchingRoot, localAppData(), vscode.workspace.name ?? '');
    const [drive, game, present, found] = await Promise.all([
      readWorkDrive(settings),
      readGameRoot(settings),
      readLinkFacts(filePatchingRootOf(runRoot)),
      readFound(launchPathsOf(target, mods)),
    ]);

    const listening = await this.listen(say, ['client2']);
    const plan = launchPlanOf(
      { target, mods, settings, drive, runRoot, game, present, found, debugPorts: portsOf(listening) },
      ['client2'],
    );

    if (plan.refusals.length > 0) {
      close(listening);
      return plan.refusals.join(' ');
    }

    const process_ = plan.processes[0];
    if (process_ === undefined) {
      close(listening);
      return `${target.id} puts no second client up.`;
    }

    await prepareLaunch(plan);

    const command = [...prefix, process_.program, ...process_.arguments].join(' ');
    this.log.info(command);
    say(command);

    const started = startGame(process_, prefix);

    return {
      games: [
        sandbox.kind === 'box'
          ? sandboxedGame(started, sandbox.sandbox, windowsName(process_.program))
          : started,
      ],
      listening,
    };
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
    const opened = await Promise.all(
      roles.map(async (role) => openScriptDebugPort(role, handler)),
    );
    const up: ScriptDebugPort[] = [];

    for (const outcome of opened) {
      if (typeof outcome === 'string') {
        this.log.warn(outcome);
        say(outcome);
      } else {
        up.push(outcome);
      }
    }

    return up;
  }
}

/** Everything one launch put up: the games, and the listeners their script logs come in on. */
interface Launched {
  readonly games: readonly GameProcess[];
  readonly listening: readonly ScriptDebugPort[];
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

function close(listening: readonly ScriptDebugPort[]): void {
  for (const port of listening) {
    port.close();
  }
}

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
 * A launch is one session however many processes it put up, so Stop takes down every one of them.
 * And when any one of them goes, so do the rest: a client whose server has gone is a client with
 * nothing to talk to, and a server nobody is joining any more would otherwise be left running with
 * nothing in the editor to show it.
 */
class GameSession implements vscode.DebugAdapter {
  private readonly messages = new vscode.EventEmitter<vscode.DebugProtocolMessage>();
  readonly onDidSendMessage = this.messages.event;

  private sequence = 1;
  private games: readonly GameProcess[] = [];
  private listening: readonly ScriptDebugPort[] = [];
  private over = false;

  constructor(
    private readonly target: string,
    private readonly launcher: Launcher,
    private readonly log: vscode.LogOutputChannel,
  ) {}

  handleMessage(message: vscode.DebugProtocolMessage): void {
    const request = message as DapRequest;
    if (request.type !== 'request') {
      return;
    }

    void this.handle(request).catch((error: unknown) => {
      this.log.error(error instanceof Error ? error : String(error));
      this.fail(request, error instanceof Error ? error.message : String(error));
      // Whatever it was, the session has no game to show for it and nothing more to do.
      this.event('terminated');
    });
  }

  dispose(): void {
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
        this.over = true;
        await this.stop();
        this.respond(request);
        this.event('terminated');
        return;
      default:
        this.respond(request);
        return;
    }
  }

  private async launch(request: DapRequest): Promise<void> {
    const outcome = await this.launcher.start(this.target, (text) => {
      this.output(text);
    });

    if (typeof outcome === 'string') {
      this.log.warn(outcome);
      this.fail(request, outcome);
      this.event('terminated');
      return;
    }

    this.games = outcome.games;
    this.listening = outcome.listening;
    this.respond(request);

    // Stop, pressed while the run folder was still being made: the session is already over, and
    // these were started after it ended. Detached processes with nothing left to stop them is
    // exactly the task manager this exists to save a developer from.
    if (this.over) {
      await this.stop();
      return;
    }

    // The session lasts as long as the launch does, so that Stop stays a way of ending it and the
    // toolbar stops showing one after the game has been closed from inside.
    for (const game of outcome.games) {
      void game.exited.then((code) => {
        void this.finish(
          code === undefined
            ? `The ${game.role} is gone.`
            : `The ${game.role} exited with ${code}.`,
        );
      });
    }
  }

  /**
   * A second client, added to this launch.
   *
   * It joins the session rather than starting one of its own: Stop takes it down with everything
   * else, its log arrives in the same console under its own prefix, and — like every other process
   * of the launch — the first one to go ends them all.
   */
  private async second(request: DapRequest): Promise<void> {
    if (this.over) {
      this.respond(request);
      return;
    }

    // Nothing about a second client is worth ending the launch over, and an error thrown out of a
    // request is: the adapter would say the session is over, the editor would disconnect, and Stop
    // would take the server and the first client with it. So it is caught here rather than there.
    const outcome = await this.launcher
      .startSecond(this.target, (text) => {
        this.output(text);
      })
      .catch((error: unknown) => (error instanceof Error ? error.message : String(error)));

    if (typeof outcome === 'string') {
      this.log.warn(outcome);
      this.output(outcome);
      await vscode.window.showWarningMessage(outcome);
      this.respond(request);
      return;
    }

    this.games = [...this.games, ...outcome.games];
    this.listening = [...this.listening, ...outcome.listening];
    this.respond(request);

    // Stop, pressed while the box was being brought up: bringing it up waits for a person to sign
    // in, which is as long as a person takes, and the launch this was joining can be over by the
    // time there is a client to join it with. A detached game with nothing left to stop it is
    // exactly the task manager this exists to save a developer from.
    if (this.over) {
      await this.stop();
      return;
    }

    // Said rather than acted on. A second client is an addition to a launch, so its going is not
    // the launch ending — least of all when it went because it could not start, which is exactly
    // when taking the server and the first client down with it does the most damage.
    for (const game of outcome.games) {
      void game.exited.then((code) => {
        this.output(
          code === undefined
            ? 'The second client is gone. The launch it joined is still up.'
            : `The second client exited with ${code}. The launch it joined is still up.`,
        );
      });
    }
  }

  /** The first process to go ends the launch, and takes whatever else it started with it. */
  private async finish(said: string): Promise<void> {
    if (this.over) {
      return;
    }

    this.over = true;
    this.output(said);
    await this.stop();
    this.event('terminated');
  }

  private async stop(): Promise<void> {
    close(this.listening);
    this.listening = [];
    await Promise.all(this.games.map((game) => game.kill()));
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
    this.messages.fire({ ...message, seq: this.sequence++ });
  }
}
