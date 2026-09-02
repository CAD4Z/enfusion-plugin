/**
 * The two build commands, which the palette and the panel's buttons both go through.
 *
 * One addon, or every addon of the workspace in the order the `requiredAddons` graph puts them.
 * Neither asks whether a rebuild is needed: there is nothing here that tracks what has changed
 * since the last one, and a button that argues about whether it should do what it says is worse
 * than one that always does it.
 *
 * A press is never turned away either. It either starts a build or joins the line behind the one
 * running — `src/mods/buildQueue.ts` is where that line and the way it folds are decided.
 *
 * One press, one build, one notification — and that last one is per *run of the queue*, not per
 * build in it and not per addon. A run puts up a single progress notification when the line starts
 * moving, says what it is packing as it goes, and says once at the end what came of the lot. Which
 * matters more than tidiness: the builder is started in a console of its own, that console takes
 * the focus off the editor and gives it back a moment later, and what came back with it was a
 * press on the button that still held the focus. The panel is where that is actually fixed (see
 * `src/webview/main.ts`); the settle window here is the belt to its braces, and a run that keeps
 * its one notification is what makes a build feeding itself visible instead of bewildering.
 *
 * Everything worth deciding is decided before anything runs — `buildPlanOf` turns the mods, the
 * machine and the work drive into an ordered list of steps — so what is left here is showing the
 * plan being carried out and saying what came of it, with whatever the builder complained about in
 * Problems, on the line it complained about.
 */

import * as vscode from 'vscode';
import {
  type BuildJob,
  type BuildRefusal,
  type BuildSource,
  type PackStep,
  buildPlanOf,
  configOf,
  jobsOf,
  subjectOf,
} from '../mods/build';
import { type BuildRequest, isSameRequest, nameOf, queued } from '../mods/buildQueue';
import type { MachineSettings } from '../mods/machine';
import { fileOf, problemsOf } from '../mods/packingLog';
import { type Link, isUnlinked } from '../mods/workDrive';
import { type StepOutcome, runBuild } from '../platform/build';
import { readMachineSettings } from '../platform/machine';
import { readLinks, readWorkDrive } from '../platform/workDrive';
import { type Discovery, findMods, ownedOf, prefixesOf } from '../platform/workspace';
import { WORK_DRIVE_COMMAND } from './workDrive';

/** The command ids, which are also what the panel's buttons ask for. */
export const BUILD_COMMAND = {
  addon: 'enfusion.build',
  all: 'enfusion.build.all',
} as const;

/**
 * How long after a build starts a press for that same thing is taken to be the same press.
 *
 * The presses this catches arrive about a sixth of a second after a builder console appears, and
 * nobody who meant a rebuild asks for it before the one they asked for has drawn its first step.
 */
const SETTLE_MS = 1000;

/** What the panel names an addon by: the two names between them are one addon of one workspace. */
export interface BuildTarget {
  readonly mod: string;
  readonly addon: string;
}

/** Registers both, with the Problems they fill in living as long as the extension does. */
export function registerBuildCommands(log: vscode.LogOutputChannel): vscode.Disposable {
  const problems = vscode.languages.createDiagnosticCollection('enfusion');
  const builds = new BuildCommands(log, problems);

  return vscode.Disposable.from(
    problems,
    vscode.commands.registerCommand(BUILD_COMMAND.addon, (target?: BuildTarget) =>
      builds.addon(target),
    ),
    vscode.commands.registerCommand(BUILD_COMMAND.all, () => {
      builds.all();
    }),
  );
}

/** The notification's own handle on what it is saying, which a press behind it writes through. */
type Report = vscode.Progress<{ message?: string; increment?: number }>;

class BuildCommands {
  /** What was last put in Problems for each addon, so a rebuild replaces its own and no more. */
  private readonly reported = new Map<string, vscode.Uri[]>();

  /** The presses standing behind the build that is running, folded as they land. */
  private waiting: readonly BuildRequest[] = [];

  /** Whether the loop that empties that line is going. There is at most one of it. */
  private draining = false;

  /** What is being built and when it started, which is what the settle window is measured from. */
  private running: { readonly request: BuildRequest; readonly at: number } | undefined;

  /**
   * What the run's notification is saying, so that a press landing mid-step is answered by the
   * count on it at once rather than at the next step — a pack is a wait long enough for "nothing
   * happened" to be the honest reading of a notification that has not changed.
   */
  private showing: { readonly report: Report; readonly what: string } | undefined;

  constructor(
    private readonly log: vscode.LogOutputChannel,
    private readonly problems: vscode.DiagnosticCollection,
  ) {}

  /** One addon: the panel names it, and the palette asks which. */
  async addon(target: BuildTarget | undefined): Promise<void> {
    const request =
      target === undefined
        ? await ask()
        : ({ kind: 'addon', mod: target.mod, addon: target.addon } as const);

    if (request !== undefined) {
      this.press(request);
    }
  }

  /** And the lot, which is what a first run of a fresh clone needs. */
  all(): void {
    this.press({ kind: 'all' });
  }

  /**
   * One press. It starts a run of the queue, or it joins the line — and where it folded into a
   * press already waiting, or into the build that has only just started, it does neither.
   */
  private press(request: BuildRequest): void {
    if (this.settling(request)) {
      this.log.info(`${nameOf(request)} has only just started; this press folded into it`);
      return;
    }

    const line = queued(this.waiting, request);
    this.waiting = line.waiting;

    this.log.info(
      !line.added
        ? `${nameOf(request)} is already coming; this press folded into it`
        : this.draining
          ? `${nameOf(request)} queued, ${this.waiting.length} waiting`
          : `${nameOf(request)}: starting`,
    );

    this.say();

    if (!this.draining) {
      void this.drain();
    }
  }

  /**
   * Whether this press is the one already being carried out. A rebuild asked for while a build is
   * running is a real request — the sources have changed since it read them — but not in the first
   * moment of it, when nothing can have changed and a console has just taken the focus.
   */
  private settling(request: BuildRequest): boolean {
    const running = this.running;

    return (
      running !== undefined &&
      isSameRequest(running.request, request) &&
      Date.now() - running.at < SETTLE_MS
    );
  }

  /**
   * The line, emptied one build at a time and in the order it was pressed, under one notification
   * for the whole of it. A cancel takes the line with it: a developer stopping a build is not
   * asking for the next one.
   */
  private async drain(): Promise<void> {
    this.draining = true;
    const tally = emptyTally();

    try {
      // A build is the one thing here that runs a program with what a `mod.enf` says in its command
      // line, so it is the one thing that waits until the developer has said the folder is theirs.
      if (!vscode.workspace.isTrusted) {
        this.waiting = [];
        this.tell(
          vscode.window.showErrorMessage(
            'Building runs the builder with paths out of this workspace’s mod.enf, so it needs ' +
              'the workspace to be trusted.',
          ),
        );
        return;
      }

      // Titled off the press that opened the run, because a title cannot be changed once the
      // notification is up. What is being packed right now goes in the message, which can.
      const first = this.waiting[0];

      await vscode.window.withProgress(
        {
          location: vscode.ProgressLocation.Notification,
          title: first === undefined ? 'Building' : `Building ${nameOf(first)}`,
          cancellable: true,
        },
        async (progress, token) => {
          this.showing = { report: progress, what: 'Reading the workspace' };
          this.say();

          for (let next = this.next(); next !== undefined; next = this.next()) {
            this.running = { request: next, at: Date.now() };
            await this.attempt(next, progress, token, tally);
            this.running = undefined;

            if (token.isCancellationRequested) {
              tally.cancelled = true;
              tally.dropped = this.waiting.length;
              this.waiting = [];
              return;
            }
          }
        },
      );
    } finally {
      this.draining = false;
      this.showing = undefined;
      this.running = undefined;
    }

    // Once the notification is down, and once for the run however many builds it took.
    this.tell(this.announce(tally));
  }

  private next(): BuildRequest | undefined {
    const [next, ...rest] = this.waiting;
    this.waiting = rest;

    return next;
  }

  /**
   * One build of the run, with whatever it threw said out loud. A build that ends in the log alone
   * is a silence nobody can act on, and the line has to carry on regardless.
   */
  private async attempt(
    request: BuildRequest,
    progress: Report,
    token: vscode.CancellationToken,
    tally: Tally,
  ): Promise<void> {
    try {
      await this.run(request, progress, token, tally);
    } catch (error: unknown) {
      this.log.error(error instanceof Error ? error : String(error));
      tally.failures.push(`${nameOf(request)} was not built: ${messageOf(error)}`);
    }
  }

  /** The workspace read, the plan made, the plan run, and what came of it added to the run's tally. */
  private async run(
    request: BuildRequest,
    progress: Report,
    token: vscode.CancellationToken,
    tally: Tally,
  ): Promise<void> {
    const input = await readBuildInput();
    tally.links = input.links;

    const jobs = wantedOf(jobsOf(input.sources), request);

    if (jobs.length === 0) {
      // Something to say when the panel named an addon that has since stopped being one; the
      // palette asked its question of a workspace read a moment ago, so this is the panel's case.
      tally.notes.add(
        request.kind === 'all'
          ? 'No addon of this workspace can be built.'
          : `${nameOf(request)} is no longer an addon of this workspace.`,
      );
      return;
    }

    const plan = buildPlanOf(jobs, input.settings);
    this.log.info(`build: ${plan.steps.length} step(s), ${plan.refusals.length} refusal(s)`);
    for (const warning of plan.warnings) {
      this.log.warn(warning);
      tally.warnings.add(warning);
    }
    for (const refusal of plan.refusals) {
      tally.refusals.add(sentenceOf(refusal));
    }

    if (plan.steps.length === 0) {
      return;
    }

    const outcomes = await runBuild(
      plan,
      (step) => {
        this.log.info(step.kind === 'pack' ? step.command : step.what);
        this.showing = { report: progress, what: step.what };
        progress.report({ message: this.saying(step.what) });
      },
      () => token.isCancellationRequested,
    );

    this.report(outcomes, jobs, input.links, input.letter);
    gather(tally, outcomes);
  }

  /** What the run is saying, said again with the line behind it as it stands now. */
  private say(): void {
    const showing = this.showing;

    if (showing !== undefined) {
      showing.report.report({ message: this.saying(showing.what) });
    }
  }

  private saying(what: string): string {
    const waiting = this.waiting.length;

    return waiting === 0 ? what : `${what} — ${waiting} more queued`;
  }

  /** Said to the developer and not waited on, with a failure to say it left in the log. */
  private tell(work: Thenable<unknown>): void {
    Promise.resolve(work).catch((error: unknown) => {
      this.log.error(error instanceof Error ? error : String(error));
    });
  }

  /**
   * What a whole run of the queue came to, said once. Which is the one sentence a developer reads
   * after pressing the button, so it names what was built rather than counting it: three addons
   * are three names, and a workspace of twenty is a number.
   */
  private async announce(tally: Tally): Promise<void> {
    const built = [...tally.built];
    const asides = [...tally.notes, ...tally.refusals, ...tally.warnings];

    if (tally.cancelled) {
      const said = [
        built.length === 0 ? 'Build cancelled.' : `Build cancelled after ${listOf(built)}.`,
        tally.dropped === 0 ? '' : `${tally.dropped} queued build(s) dropped with it.`,
        ...asides,
      ].filter((part) => part !== '');

      await vscode.window.showWarningMessage(said.join(' '));
      return;
    }

    if (tally.failures.length > 0) {
      const said = [...tally.failures, ...asides].join(' ');
      const packingLog = tally.log;

      if (packingLog === undefined) {
        await vscode.window.showErrorMessage(said);
        return;
      }

      const open = 'Open Log';
      if ((await vscode.window.showErrorMessage(said, open)) === open) {
        await vscode.window.showTextDocument(vscode.Uri.file(packingLog));
      }
      return;
    }

    if (built.length === 0) {
      // Nothing packed and nothing failed: it was all refused, or there was nothing to pack. The
      // refusal is the one that carries a button, because the work drive is what it is usually about.
      await refuse(asides, tally.links);
      return;
    }

    await vscode.window.showInformationMessage([`Built ${listOf(built)}.`, ...asides].join(' '));
  }

  /**
   * What the builder said, on the line it said it about, in the file the developer has open —
   * a work drive path is a junction away from the workspace, and landing in a second copy of the
   * same file would be its own kind of confusing.
   *
   * An addon that failed with nothing to point at still gets a mark on its `config.cpp`: the
   * Problems list is where a developer looks, and "not built, and here is the log" belongs there
   * rather than only in a message box that is one click from being gone.
   */
  private report(
    outcomes: readonly StepOutcome[],
    jobs: readonly BuildJob[],
    links: readonly Link[],
    letter: string,
  ): void {
    const bySubject = new Map(jobs.map((job) => [subjectOf(job), job] as const));

    for (const outcome of outcomes) {
      if (outcome.step.kind !== 'pack') {
        continue;
      }

      const job = bySubject.get(outcome.step.subject);
      const found = problemsOf(outcome.log).map((problem) => ({
        uri: vscode.Uri.file(fileOf(problem.file, letter, links)),
        diagnostic: new vscode.Diagnostic(
          lineAt(problem.line),
          problem.message,
          outcome.state === 'failed'
            ? vscode.DiagnosticSeverity.Error
            : vscode.DiagnosticSeverity.Warning,
        ),
      }));

      if (outcome.state === 'failed' && found.length === 0 && job !== undefined) {
        found.push({
          uri: vscode.Uri.file(configOf(job)),
          diagnostic: new vscode.Diagnostic(
            lineAt(1),
            outcome.failure ?? `${outcome.step.subject} was not built.`,
            vscode.DiagnosticSeverity.Error,
          ),
        });
      }

      this.replace(outcome.step, found);
    }
  }

  /** One addon's marks, in place of the ones it left last time and of nobody else's. */
  private replace(
    step: PackStep,
    found: readonly { uri: vscode.Uri; diagnostic: vscode.Diagnostic }[],
  ): void {
    for (const uri of this.reported.get(step.subject) ?? []) {
      this.problems.delete(uri);
    }

    const byFile = new Map<string, vscode.Diagnostic[]>();
    const uris = new Map<string, vscode.Uri>();

    for (const { uri, diagnostic } of found) {
      uris.set(uri.toString(), uri);
      byFile.set(uri.toString(), [...(byFile.get(uri.toString()) ?? []), diagnostic]);
    }

    for (const [key, diagnostics] of byFile) {
      const uri = uris.get(key);
      if (uri !== undefined) {
        this.problems.set(uri, diagnostics);
      }
    }

    this.reported.set(step.subject, [...uris.values()]);
  }
}

/**
 * What a run of the queue has come to so far. Gathered rather than counted: a run that packed the
 * same addon twice — a press that landed while it was being packed — built one addon, not two.
 */
interface Tally {
  /** The subjects packed, by name, in the order they first came out. */
  readonly built: Set<string>;
  /** What went wrong, in the words the runner said it in. */
  readonly failures: string[];
  /** Sentences that are neither: an addon that is no longer one, a mod nothing could build. */
  readonly notes: Set<string>;
  readonly refusals: Set<string>;
  readonly warnings: Set<string>;
  /** The packing log of the first failure, which is the one the message offers to open. */
  log: string | undefined;
  /** The links as the last build read them, for the button a refusal carries. */
  links: readonly Link[];
  cancelled: boolean;
  /** How many presses were still waiting when the cancel took them. */
  dropped: number;
}

function emptyTally(): Tally {
  return {
    built: new Set(),
    failures: [],
    notes: new Set(),
    refusals: new Set(),
    warnings: new Set(),
    log: undefined,
    links: [],
    cancelled: false,
    dropped: 0,
  };
}

/** One build's outcomes folded into the run's tally. */
function gather(tally: Tally, outcomes: readonly StepOutcome[]): void {
  for (const outcome of outcomes) {
    if (outcome.step.kind === 'pack' && outcome.state === 'done') {
      tally.built.add(outcome.step.subject);
    }

    if (outcome.failure !== undefined) {
      tally.failures.push(outcome.failure);

      if (tally.log === undefined && outcome.step.kind === 'pack') {
        tally.log = outcome.step.log.path;
      }
    }
  }
}

/** Names while there are few enough to read, a count once there are not. */
function listOf(names: readonly string[]): string {
  if (names.length > 4) {
    return `${names.length} addons`;
  }

  const last = names[names.length - 1] ?? '';

  return names.length === 1 ? last : `${names.slice(0, -1).join(', ')} and ${last}`;
}

/** The workspace as one build reads it, all of it asked for once rather than per step. */
interface BuildInput {
  readonly sources: readonly BuildSource[];
  readonly links: readonly Link[];
  readonly settings: MachineSettings;
  /** The work drive's letter as the tools take it, for reading a builder's paths back. */
  readonly letter: string;
}

/**
 * Everything a build is planned from, read afresh: the mods, the machine, and where the work drive
 * is right now. What the panel last showed is not trusted for the same reason it is not trusted by
 * the work drive commands — a developer who mounted the drive in a terminal a second ago is
 * precisely the case a remembered answer gets wrong.
 *
 * Read per build rather than per run, which is the whole worth of queueing a press instead of
 * refusing it: what gets packed is the workspace as it stands when the build's turn comes, not as
 * it stood when the button was pressed.
 */
async function readBuildInput(): Promise<BuildInput> {
  const [found, settings] = await Promise.all([findMods(), readMachineSettings()]);
  const drive = await readWorkDrive(settings);
  const links = await readLinks(drive, prefixesOf(found));

  return { sources: sourcesOf(found, links), links, settings, letter: drive.letter };
}

/**
 * A mod as a build sees it. Which file its launch block came from is the cascade's answer, and it
 * is that file's folder a relative mods directory is counted from — the path in a `.enf` means
 * what it means where it is written.
 */
function sourcesOf(found: Discovery, links: readonly Link[]): BuildSource[] {
  return ownedOf(found).map((owned) => ({
    mod: owned.mod,
    link: links.find((made) => made.prefixRoot === owned.mod.prefixRoot),
    modsDirectory: owned.launch.modsDirectory ?? '',
    exclude: owned.exclude,
    configuredIn: owned.configuredIn,
    configuredBy: owned.configuredBy,
  }));
}

/** The addons one press is for: the one it named, or every one there is. */
function wantedOf(jobs: readonly BuildJob[], request: BuildRequest): BuildJob[] {
  return request.kind === 'all'
    ? [...jobs]
    : jobs.filter((job) => job.link.name === request.mod && job.addon === request.addon);
}

/** From the palette there is nothing to click, so the list of addons is the question asked. */
async function ask(): Promise<BuildRequest | undefined> {
  const input = await readBuildInput();
  const jobs = jobsOf(input.sources);

  if (jobs.length === 0) {
    await vscode.window.showWarningMessage('No addon of this workspace can be built.');
    return undefined;
  }

  const items = jobs.map((job) => ({ label: subjectOf(job), description: job.link.path, job }));
  const picked = await vscode.window.showQuickPick(items, { placeHolder: 'Addon to build' });

  return picked === undefined
    ? undefined
    : { kind: 'addon', mod: picked.job.link.name, addon: picked.job.addon };
}

function lineAt(line: number): vscode.Range {
  const at = new vscode.Position(Math.max(0, line - 1), 0);

  return new vscode.Range(at, at);
}

/**
 * Nothing was built and here is why, with the one press that would settle it where the work drive
 * is what is wrong. Which command that is comes off the state of the links rather than out of the
 * words of the refusal: the sentence is the domain's to reword, and a button that goes missing
 * when somebody rephrases one would go missing quietly.
 */
async function refuse(said: readonly string[], links: readonly Link[]): Promise<void> {
  const message = said.length === 0 ? 'Nothing to build.' : said.join(' ');

  const action = links.some((link) => link.state === 'unavailable')
    ? { label: 'Mount Work Drive', command: WORK_DRIVE_COMMAND.mount }
    : links.some(isUnlinked)
      ? { label: 'Link Mods', command: WORK_DRIVE_COMMAND.link }
      : undefined;

  if (action === undefined) {
    await vscode.window.showErrorMessage(message);
    return;
  }

  if ((await vscode.window.showErrorMessage(message, action.label)) === action.label) {
    await vscode.commands.executeCommand(action.command);
  }
}

function sentenceOf(refusal: BuildRefusal): string {
  return refusal.subject === '' ? refusal.reason : `${refusal.subject}: ${refusal.reason}`;
}

function messageOf(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}
