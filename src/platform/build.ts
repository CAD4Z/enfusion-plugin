/**
 * Running a build plan. Every decision behind it — what to run, what has to appear, how many
 * times — was made in `src/mods/build.ts`; this only does what the plan says and reports what
 * happened.
 *
 * Two habits are what the whole thing is built around, and neither is negotiable with the tools:
 *
 * The builder is started through `start`, in a console of its own, and its exit code is ignored.
 * pboProject without a console exits 1 immediately having done nothing; both builders exit 0 on a
 * build that failed. So the pbo is taken off first and looked for afterwards, and that — the file
 * being there — is the whole of what success means.
 *
 * A run that produced nothing is given one more go before it is called a failure, with a pause in
 * between: a builder that could not write its pbo because something still had a handle on it is
 * the case that pause is for, and it is the case the retry has always caught.
 */

import { execFile, exec as execWithShell } from 'node:child_process';
import { copyFile, mkdir, open, readdir, rm, stat } from 'node:fs/promises';
import { promisify } from 'node:util';
import type { BuildPlan, BuildStep, CopyStep, PackStep, SignStep } from '../mods/build';
import { windowsFolder, windowsName, windowsPath } from '../mods/paths';

const run = promisify(execFile);
const shell = promisify(execWithShell);

/** What became of one step of the plan. */
export interface StepOutcome {
  readonly step: BuildStep;
  readonly state: 'done' | 'failed' | 'skipped';
  /** What the builder wrote this run; empty for anything that is not a pack. */
  readonly log: string;
  /** What went wrong, said the way the developer is told it; undefined where nothing did. */
  readonly failure: string | undefined;
}

/** Told the step that is starting, so that a progress bar can say what is being waited on. */
export type OnStep = (step: BuildStep, index: number) => void;

/**
 * Every step in order, carrying on with the next addon after one fails: the addons are packed in
 * dependency order because that is the order they are loaded in, not because one packs out of
 * another's pbo.
 *
 * What a failed pack takes with it is the signing of that same pbo, and only that. Copying the
 * mod's root files does not need a pbo to have been produced, and hanging them off the packing
 * would mean a mod of one addon losing its `mod.cpp` where a mod of three would keep it — the same
 * requirement behaving two ways depending on a layout that has nothing to do with it.
 */
export async function runBuild(
  plan: BuildPlan,
  onStep: OnStep,
  cancelled: () => boolean,
): Promise<StepOutcome[]> {
  const outcomes: StepOutcome[] = [];
  const unpacked = new Set<string>();

  for (const [index, step] of plan.steps.entries()) {
    if (cancelled() || (step.kind === 'sign' && unpacked.has(step.subject))) {
      outcomes.push({ step, state: 'skipped', log: '', failure: undefined });
      continue;
    }

    onStep(step, index);
    const outcome = await runStep(step);

    if (outcome.state === 'failed' && step.kind === 'pack') {
      unpacked.add(step.subject);
    }
    outcomes.push(outcome);
  }

  return outcomes;
}

async function runStep(step: BuildStep): Promise<StepOutcome> {
  switch (step.kind) {
    case 'pack':
      return pack(step);
    case 'sign':
      return sign(step);
    case 'copy':
      return copy(step);
  }
}

/**
 * The build itself: the folders made, the old pbo taken off, the builder run — and then the one
 * question worth asking, which is whether the pbo is there.
 */
async function pack(step: PackStep): Promise<StepOutcome> {
  try {
    for (const folder of step.folders) {
      await mkdir(folder, { recursive: true });
    }
    await remove(step.stale);
  } catch (error: unknown) {
    // A mods directory on a disk that is not there, or one nobody may write to. Worth saying as
    // itself: running the builder into it would fail for a reason nothing would name.
    return {
      step,
      state: 'failed',
      log: '',
      failure: `${step.subject}: could not make ready ${step.folders[0] ?? ''} — ${messageOf(error)}`,
    };
  }

  let log = '';

  for (let attempt = 1; attempt <= step.attempts; attempt += 1) {
    if (attempt > 1) {
      await pause(step.pauseMs);
    }

    // Only what the builder appends this run is this addon's, where it appends at all — and where
    // it rewrites the file instead, only a file it has actually rewritten is this run's at all.
    // A builder that never started leaves the log some earlier run wrote, and reading that would
    // report a build that never happened by the words of one that did.
    const from = step.log.appends ? await sizeOf(step.log.path) : 0;
    const before = await writtenAt(step.log.path);

    await runBuilder(step.command);

    log = (await writtenAt(step.log.path)) === before ? '' : await readFrom(step.log.path, from);

    if (await exists(step.pbo)) {
      return { step, state: 'done', log, failure: undefined };
    }
  }

  return {
    step,
    state: 'failed',
    log,
    // A builder that wrote nothing did not run, and sending a developer to a log it never touched
    // is sending them to somebody else's answer.
    failure:
      log === ''
        ? `${step.subject} was not built, and the builder wrote nothing to ${step.log.path}. ` +
          'Check that the builder setting names the program and not the folder holding it.'
        : `${step.subject} was not built. See ${step.log.path}`,
  };
}

async function sign(step: SignStep): Promise<StepOutcome> {
  try {
    await run(step.program, [...step.arguments]);
    return { step, state: 'done', log: '', failure: undefined };
  } catch (error: unknown) {
    // Unlike the builder, DSSignFile answers honestly: a non-zero code is a signature not made.
    return { step, state: 'failed', log: '', failure: `${step.what} failed: ${messageOf(error)}` };
  }
}

/**
 * A root file that is not there is not a failure: a mod with no `mod.cpp` is a mod, it just does
 * not name itself in the launcher, and a developer who set no key has no public key to copy.
 */
async function copy(step: CopyStep): Promise<StepOutcome> {
  if (!(await exists(step.from))) {
    return { step, state: 'skipped', log: '', failure: undefined };
  }

  try {
    await mkdir(windowsFolder(step.to), { recursive: true });
    await copyFile(step.from, step.to);
    return { step, state: 'done', log: '', failure: undefined };
  } catch (error: unknown) {
    return { step, state: 'failed', log: '', failure: `${step.what} failed: ${messageOf(error)}` };
  }
}

/**
 * Through the shell, because `start` is a shell built-in and it is `start` that gives the builder
 * the console it will not work without. What it exits with is not read: both builders answer 0 for
 * a build that failed, so the exit code carries no information at all.
 */
async function runBuilder(command: string): Promise<void> {
  try {
    await shell(command);
  } catch {
    // Which is what a builder does on a bad day and on a good one alike.
  }
}

/** Takes off what the plan named, including the signatures a `*` in a name stands for. */
async function remove(paths: readonly string[]): Promise<void> {
  for (const path of paths) {
    const name = windowsName(path);

    if (!name.includes('*')) {
      await rm(path, { force: true });
      continue;
    }

    const folder = windowsFolder(path);
    const matches = matcher(name);

    for (const entry of await entriesOf(folder)) {
      if (matches.test(entry)) {
        await rm(windowsPath(folder, entry), { force: true });
      }
    }
  }
}

/** A name with a `*` in it, as a pattern over the names in one folder. Windows ignores case. */
function matcher(name: string): RegExp {
  const escaped = name.replace(/[.*+?^${}()|[\]\\]/g, '\\$&').replace(/\\\*/g, '.*');

  return new RegExp(`^${escaped}$`, 'i');
}

async function entriesOf(folder: string): Promise<string[]> {
  try {
    return await readdir(folder);
  } catch {
    return [];
  }
}

/**
 * What the log holds past the offset given. Read as it comes: a builder writes in whatever the
 * machine's code page is, and the part that matters — a path, a line number — is plain ASCII
 * either way.
 */
async function readFrom(path: string, from: number): Promise<string> {
  let file;

  try {
    file = await open(path, 'r');
    const { size } = await file.stat();
    const length = Math.max(0, size - from);

    if (length === 0) {
      return '';
    }

    const buffer = Buffer.alloc(length);
    await file.read(buffer, 0, length, from);

    return buffer.toString('utf8');
  } catch {
    // A builder that wrote no log at all is a builder that said nothing, which is an empty string.
    return '';
  } finally {
    await file?.close();
  }
}

/** When the file was last written, or 0 where there is none: what tells this run's log from a past one. */
async function writtenAt(path: string): Promise<number> {
  try {
    return (await stat(path)).mtimeMs;
  } catch {
    return 0;
  }
}

async function sizeOf(path: string): Promise<number> {
  try {
    return (await stat(path)).size;
  } catch {
    return 0;
  }
}

async function exists(path: string): Promise<boolean> {
  try {
    await stat(path);
    return true;
  } catch {
    return false;
  }
}

function pause(ms: number): Promise<void> {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function messageOf(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}
