/**
 * Talking to `subst`, the program that puts a folder up under a drive letter.
 *
 * It is the whole of what mounting a work drive is on Windows, and the one thing a developer
 * should not have to remember the syntax of — so the syntax is written down here, once, and both
 * the button and the command line come out of it.
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
