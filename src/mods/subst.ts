/**
 * Talking to the two programs that put a folder up under a drive letter.
 *
 * DayZ's `WorkDrive.exe` repeats the mapping in ordinary and elevated Windows token contexts;
 * that is what keeps P: visible whether Workbench was started normally or through an elevated
 * script. A bare `subst` is the fallback where DayZ Tools is absent, and remains the implementation
 * for a nonstandard letter because the shipped helper's elevated unmount always defaults to P:.
 * The syntax of both is written down here once.
 *
 * Only the shape of the output is read, never its words: `subst` speaks whatever language the
 * machine does, and a refusal in Russian has to mean what one in English means, which is
 * "nothing is mounted here".
 */

/** What `subst` is called with to put the drive up. */
export function mountArguments(letter: string, source: string): string[] {
  return [letter, source];
}

/** And to take it back down, freeing the letter. */
export function unmountArguments(letter: string): string[] {
  return [letter, '/D'];
}

/** One program invocation, kept plain so the platform only has to execute it. */
export interface DriveCommand {
  readonly file: string;
  readonly args: string[];
}

/**
 * The command that mounts a work drive. The DayZ helper launches its own elevated child and makes
 * P: in both Windows token contexts; `subst` alone makes it only in the extension host's context.
 */
export function mountCommandOf(letter: string, source: string, workDriveTool: string): DriveCommand {
  return useWorkDriveTool(letter, workDriveTool)
    ? {
        file: workDriveTool,
        args: ['/y', '/Silent', '/nowarnings', '/mount', letter, source],
      }
    : { file: 'subst', args: mountArguments(letter, source) };
}

/** The matching unmount command, so neither Windows token context keeps a stale P: mapping. */
export function unmountCommandOf(letter: string, workDriveTool: string): DriveCommand {
  return useWorkDriveTool(letter, workDriveTool)
    ? {
        file: workDriveTool,
        args: ['/y', '/Silent', '/nowarnings', '/unmount', letter],
      }
    : { file: 'subst', args: unmountArguments(letter) };
}

/**
 * WorkDrive's elevated unmount child does not carry its caller's letter and falls back to P:, so
 * using it for another letter would leave half of that mapping behind.
 */
function useWorkDriveTool(letter: string, workDriveTool: string): boolean {
  return workDriveTool !== '' && letter.toUpperCase() === 'P:';
}

/**
 * The folder the letter is mounted from, out of what `subst` printed with no arguments. A letter
 * that is not in the output is mounted nowhere, which is an empty string rather than an absence:
 * every caller here asks "where", and "nowhere" is an answer to that question.
 */
export function mountedAt(output: string, letter: string): string {
  for (const line of output.split('\n')) {
    const found = MAPPING.exec(line.trim());

    if (found && found[1]?.toLowerCase() === letter.toLowerCase()) {
      return (found[2] ?? '').trim();
    }
  }

  return '';
}

/** `P:\: => F:\DayZ\Workdrive`, which is the shape every mapping `subst` prints has. */
const MAPPING = /^([A-Za-z]:)\\:\s*=>\s*(.+)$/;
