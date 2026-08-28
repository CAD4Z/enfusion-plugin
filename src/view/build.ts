/**
 * The two build commands, which the palette and the panel's buttons both go through.
 *
 * One addon, or every addon of the workspace in the order the `requiredAddons` graph puts them.
 * Neither asks whether a rebuild is needed: there is nothing here that tracks what has changed
 * since the last one, and a button that argues about whether it should do what it says is worse
 * than one that always does it.
 *
 * Everything worth deciding is decided before anything runs — `buildPlanOf` turns the mods, the
 * machine and the work drive into an ordered list of steps — so what is left here is showing the
 * plan being carried out and saying what came of it: progress while it runs, a sentence when it
 * ends, and whatever the builder complained about in Problems, on the line it complained about.
 */

import * as vscode from 'vscode';
import {
  type BuildJob,
  type BuildPlan,
  type BuildRefusal,
  type BuildSource,
  type PackStep,
  buildPlanOf,
  configOf,
  jobsOf,
  subjectOf,
} from '../mods/build';
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
    vscode.commands.registerCommand(BUILD_COMMAND.all, () => builds.all()),
  );
}

class BuildCommands {
  /** What was last put in Problems for each addon, so a rebuild replaces its own and no more. */
  private readonly reported = new Map<string, vscode.Uri[]>();

  /**
   * Whether a build is running, which is the one thing that stops a second one.
   *
   * Two builds of the same workspace do not share the work — they race for the same pbo, one
   * taking the file off while the other writes it — and every one of them puts a builder process
   * per addon on the machine. Which is not a theoretical objection: a button that keeps the
   * keyboard focus is a button a held key presses again and again, and the machine that ran into
   * that ended up with five hundred builders on it and stopped responding.
   *
   * So a second request is refused rather than queued. A queue is the same pile of processes,
   * only later, and nobody asking twice wants the same build twice.
   */
  private running = false;

  /** Whether the refusal has been shown, so a held key is not answered with a wall of them. */
  private refused = false;

  constructor(
    private readonly log: vscode.LogOutputChannel,
    private readonly problems: vscode.DiagnosticCollection,
  ) {}

  /** One addon: the panel names it, and the palette asks which. */
  async addon(target: BuildTarget | undefined): Promise<void> {
    await this.once(async () => {
      const input = await readBuildInput();
      const jobs = jobsOf(input.sources);
      const wanted = target === undefined ? await choose(jobs) : named(jobs, target);

      if (wanted === undefined) {
        // Nothing to say when the palette's list was dismissed; something to say when the panel
        // named an addon that has since stopped being one.
        if (target !== undefined) {
          await vscode.window.showWarningMessage(
            `${target.mod}\\${target.addon} is no longer an addon of this workspace.`,
          );
        }
        return;
      }

      await this.build([wanted], input);
    });
  }

  /** And the lot, which is what a first run of a fresh clone needs. */
  async all(): Promise<void> {
    await this.once(async () => {
      const input = await readBuildInput();
      const jobs = jobsOf(input.sources);

      if (jobs.length === 0) {
        await vscode.window.showWarningMessage('No addon of this workspace can be built.');
        return;
      }

      await this.build(jobs, input);
    });
  }

  /**
   * One build at a time, whichever button or command asked for it. The guard is here rather than
   * on each entry point so that building one addon and building the lot cannot overlap either.
   */
  private async once(work: () => Promise<void>): Promise<void> {
    if (this.running) {
      this.log.warn('a build is already running; this one was not started');
      await this.sayBusy();
      return;
    }

    this.running = true;
    this.refused = false;

    try {
      await work();
    } finally {
      this.running = false;
    }
  }

  /** Said once per build, however many times it was asked for while that build was running. */
  private async sayBusy(): Promise<void> {
    if (this.refused) {
      return;
    }

    this.refused = true;
    await vscode.window.showWarningMessage(
      'A build is already running. Wait for it to finish, or stop it from the progress notification.',
    );
  }

  private async build(jobs: readonly BuildJob[], input: BuildInput): Promise<void> {
    // A build is the one thing here that runs a program with what a `mod.enf` says in its command
    // line, so it is the one thing that waits until the developer has said the folder is theirs.
    if (!vscode.workspace.isTrusted) {
      await vscode.window.showErrorMessage(
        'Building runs the builder with paths out of this workspace’s mod.enf, so it needs ' +
          'the workspace to be trusted.',
      );
      return;
    }

    const plan = buildPlanOf(jobs, input.settings);
    this.log.info(`build: ${plan.steps.length} step(s), ${plan.refusals.length} refusal(s)`);
    for (const warning of plan.warnings) {
      this.log.warn(warning);
    }

    if (plan.steps.length === 0) {
      await refuse(plan.refusals, input.links);
      return;
    }

    const outcomes = await this.carryOut(plan);
    this.report(outcomes, jobs, input.links, input.letter);
    await announce(outcomes, plan);
  }

  private async carryOut(plan: BuildPlan): Promise<StepOutcome[]> {
    // Named off the steps rather than off the jobs: a refused mod leaves the jobs and the addons
    // actually being packed out of step with each other, and the title would name the wrong one.
    const packs = plan.steps.filter((step) => step.kind === 'pack');
    const title = packs.length === 1 ? `Building ${packs[0]?.subject}` : `Building ${packs.length} addons`;

    return vscode.window.withProgress(
      { location: vscode.ProgressLocation.Notification, title, cancellable: true },
      async (progress, token) => {
        const share = 100 / plan.steps.length;

        return runBuild(
          plan,
          (step) => {
            this.log.info(step.kind === 'pack' ? step.command : step.what);
            progress.report({ message: step.what, increment: share });
          },
          () => token.isCancellationRequested,
        );
      },
    );
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

function named(jobs: readonly BuildJob[], target: BuildTarget): BuildJob | undefined {
  return jobs.find((job) => job.link.name === target.mod && job.addon === target.addon);
}

/** From the palette there is nothing to click, so the list of addons is the question asked. */
async function choose(jobs: readonly BuildJob[]): Promise<BuildJob | undefined> {
  if (jobs.length === 0) {
    await vscode.window.showWarningMessage('No addon of this workspace can be built.');
    return undefined;
  }

  const items = jobs.map((job) => ({ label: subjectOf(job), description: job.link.path, job }));
  const picked = await vscode.window.showQuickPick(items, { placeHolder: 'Addon to build' });

  return picked?.job;
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
async function refuse(refusals: readonly BuildRefusal[], links: readonly Link[]): Promise<void> {
  const message = refusals.map(sentenceOf).join(' ');
  const said = message === '' ? 'Nothing to build.' : message;

  const action = links.some((link) => link.state === 'unavailable')
    ? { label: 'Mount Work Drive', command: WORK_DRIVE_COMMAND.mount }
    : links.some(isUnlinked)
      ? { label: 'Link Mods', command: WORK_DRIVE_COMMAND.link }
      : undefined;

  if (action === undefined) {
    await vscode.window.showErrorMessage(said);
    return;
  }

  if ((await vscode.window.showErrorMessage(said, action.label)) === action.label) {
    await vscode.commands.executeCommand(action.command);
  }
}

/** What came of a build that did run: what was built, what was not, and what was left out. */
async function announce(outcomes: readonly StepOutcome[], plan: BuildPlan): Promise<void> {
  const refusals = plan.refusals;
  const packs = outcomes.filter((outcome) => outcome.step.kind === 'pack');
  const built = packs.filter((outcome) => outcome.state === 'done');
  const failures = outcomes.flatMap((outcome) => (outcome.failure === undefined ? [] : [outcome]));

  if (failures.length === 0) {
    const said = [
      built.length === 1
        ? `Built ${built[0]?.step.subject}.`
        : `Built ${built.length} of ${packs.length} addons.`,
      ...refusals.map(sentenceOf),
      ...plan.warnings,
    ];

    await vscode.window.showInformationMessage(said.join(' '));
    return;
  }

  const first = failures[0];
  const message = [
    failures.map((outcome) => outcome.failure).join(' '),
    ...refusals.map(sentenceOf),
    ...plan.warnings,
  ].join(' ');
  const log = first?.step.kind === 'pack' ? first.step.log.path : undefined;

  if (log === undefined) {
    await vscode.window.showErrorMessage(message);
    return;
  }

  const open = 'Open Log';
  if ((await vscode.window.showErrorMessage(message, open)) === open) {
    await vscode.window.showTextDocument(vscode.Uri.file(log));
  }
}

function sentenceOf(refusal: BuildRefusal): string {
  return refusal.subject === '' ? refusal.reason : `${refusal.subject}: ${refusal.reason}`;
}
