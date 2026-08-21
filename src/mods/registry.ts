/**
 * Reading what `reg query` printed.
 *
 * The Windows registry is where the installers recorded the paths this extension would otherwise
 * ask a developer to type, so it is the default behind every path setting. Only the shape of the
 * output is read here — never its words: `reg` speaks whatever language the machine does, and an
 * error message in Russian has to mean the same as one in English, which is "nothing".
 */

/**
 * The value printed under the name asked for, or an empty string when the key, the value, or the
 * whole output is not there. Names are matched without regard for case, because the registry keeps
 * them the way they were written — DayZ's own is `MAIN` under one key and `main` under another.
 */
export function registryValue(output: string, name: string): string {
  for (const line of output.split('\n')) {
    const found = VALUE.exec(line);

    if (found && found[1]?.toLowerCase() === name.toLowerCase()) {
      return (found[2] ?? '').trim();
    }
  }

  return '';
}

/** `    <name>    REG_SZ    <data>`, the shape every line of a value in `reg query` output has. */
const VALUE = /^ {4}(.*?) {4}REG_[A-Z_]+ {4}(.*)$/;
