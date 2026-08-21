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
  type LaunchTarget,
  type TargetSource,
  launchPlanOf,
  runRootOf,
  targetById,
  targetsOf,
} from '../mods/launch';
import { MANIFEST_FILE } from '../mods/model';
import {
  type GameProcess,
  localAppData,
  prepareLaunch,
  readGameRoot,
  readRunRoot,
  startGame,
} from '../platform/launch';
import { readMachineSettings } from '../platform/machine';
import { readWorkDrive } from '../platform/workDrive';
import { type Discovery, findMods, ownedOf, prefixesOf } from '../platform/workspace';

/** The debug type contributed in `package.json`; a configuration names it as `"type"`. */
export const LAUNCH_TYPE = 'enfusion';

export const LAUNCH_COMMAND = { select: 'enfusion.selectTarget' } as const;

/** The fields a configuration of ours takes. Anything else is the manifest's business. */
const CONFIGURATION_FIELDS: readonly string[] = ['type', 'request', 'name', 'target'];

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
          new GameSession(targetOf(session.configuration), launcher, log),
        ),
    }),
    vscode.commands.registerCommand(LAUNCH_COMMAND.select, () => bar.choose()),
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
    return targetsOf(sourcesOf(await findMods()));
  }

  /** The game, running — or the sentence saying why it is not. */
  async start(id: string, say: (text: string) => void): Promise<GameProcess | string> {
    // A launch is one of the two things that puts a path out of a `mod.enf` on a command line, so
    // like a build it waits until the developer has said the folder is theirs.
    if (!vscode.workspace.isTrusted) {
      return (
        `Launching starts the game with paths out of this workspace’s ${MANIFEST_FILE}, so it ` +
        'needs the workspace to be trusted.'
      );
    }

    const [found, settings] = await Promise.all([findMods(), readMachineSettings()]);
    const target = targetById(targetsOf(sourcesOf(found)), id);
    if (target === undefined) {
      return `No launch target is called "${id}" any more.`;
    }

    const runRoot = runRootOf(
      settings.filePatchingRoot,
      localAppData(),
      vscode.workspace.name ?? '',
    );
    const [drive, game, present] = await Promise.all([
      readWorkDrive(settings),
      readGameRoot(settings),
      readRunRoot(runRoot),
    ]);

    const plan = launchPlanOf({
      target,
      mods: prefixesOf(found).map((prefix) => ({ name: prefix.name, prefixRoot: prefix.target })),
      settings,
      drive,
      runRoot,
      game,
      present,
    });

    if (plan.refusals.length > 0) {
      return plan.refusals.join(' ');
    }

    for (const warning of plan.warnings) {
      this.log.warn(warning);
      say(warning);
    }

    await prepareLaunch(plan);
    this.log.info(
      `launch: ${plan.filePatching.junctions.length} link(s) made, ` +
        `${plan.filePatching.remove.length} taken off, in ${runRoot}`,
    );

    const process_ = plan.processes[0];
    if (process_ === undefined) {
      return `${target.id} puts nothing up.`;
    }

    const command = `${process_.program} ${process_.arguments.join(' ')}`;
    this.log.info(command);
    say(command);

    return startGame(process_);
  }
}

/** The launch block of every mod, with the file that owns it — which is what targets come from. */
function sourcesOf(found: Discovery): TargetSource[] {
  return ownedOf(found).map((owned) => ({
    mod: owned.mod.name,
    owner: owned.owner,
    configuredBy: owned.configuredBy,
    configuredIn: owned.configuredIn,
    launch: owned.launch,
  }));
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
 */
class GameSession implements vscode.DebugAdapter {
  private readonly messages = new vscode.EventEmitter<vscode.DebugProtocolMessage>();
  readonly onDidSendMessage = this.messages.event;

  private sequence = 1;
  private game: GameProcess | undefined;

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
        });
        this.event('initialized');
        return;
      case 'launch':
        await this.launch(request);
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
        await this.game?.kill();
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

    this.game = outcome;
    this.respond(request);

    // The session lasts as long as the game does, so that Stop stays a way of ending it and the
    // toolbar stops showing one after the game has been closed from inside.
    void outcome.exited.then((code) => {
      this.output(code === undefined ? 'The game is gone.' : `The game exited with ${code}.`);
      this.event('terminated');
    });
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
