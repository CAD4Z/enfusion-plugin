/** Decisions made before a font command writes anything; native code owns recipes and generation. */
import { samePath, windowsFolder } from '../paths';
import type { EnfusionRoot } from '../texture/textureConversion';

type Metadata = { readonly kind: 'missing' | 'present' } | { readonly kind: 'unreadable'; readonly reason: string };

interface CommandContext {
  readonly platform: string;
  readonly scheme: string;
  readonly file: string;
  readonly roots: readonly EnfusionRoot[];
  readonly existing: readonly string[];
  readonly metadata: Metadata;
}

export type FontRequest =
  | { readonly kind: 'generate'; readonly name: string; readonly size: number; readonly characters?: string }
  | { readonly kind: 'regenerate' };

export type FontCommandInput = CommandContext & FontRequest;

export type FontCommandPlan =
  | { readonly kind: 'refused'; readonly reason: string }
  | {
      readonly kind: 'ready';
      readonly action: 'generate' | 'replace' | 'regenerate';
      readonly output: string;
      readonly atlas: string;
      readonly metadata: string;
      readonly resourceName: string;
      readonly source?: string;
      readonly size?: number;
      readonly characters?: string;
      readonly replace: readonly string[];
    };

export function fontCommandPlanOf(input: FontCommandInput): FontCommandPlan {
  const refusal = fontCommandRefusalOf(input);
  if (refusal !== undefined) return { kind: 'refused', reason: refusal };
  if (input.metadata.kind === 'unreadable') {
    return { kind: 'refused', reason: `The font recipe could not be read: ${input.metadata.reason}` };
  }
  if (input.kind === 'regenerate' && input.metadata.kind === 'missing') {
    return { kind: 'refused', reason: 'This font has no .fnt.meta recipe. Generate it from a .ttf first.' };
  }
  if (input.kind === 'generate') {
    const nameProblem = fontNameProblemOf(input.name);
    if (nameProblem !== undefined) return { kind: 'refused', reason: nameProblem };
    if (!Number.isInteger(input.size) || input.size < 8 || input.size > 40) {
      return { kind: 'refused', reason: 'Font size must be a whole number from 8 to 40.' };
    }
  }
  const targets = fontTargetsOf(input.file, input);
  const root = input.roots.filter((root) => within(input.file, root.root))
    .sort((a, b) => samePath(b.root).length - samePath(a.root).length)[0];
  if (root === undefined) return { kind: 'refused', reason: 'The file is outside every discovered Enfusion root in this window.' };
  const resourceRoot = root.prefixRoot !== undefined && within(targets.output, root.prefixRoot) ? root.prefixRoot : root.root;
  return {
    kind: 'ready', action: input.kind === 'regenerate' ? 'regenerate' : input.existing.length > 0 ? 'replace' : 'generate',
    ...targets, resourceName: targets.output.slice(windowsFolder(resourceRoot).length + 1),
    source: input.kind === 'generate' ? input.file : undefined, size: input.kind === 'generate' ? input.size : undefined,
    characters: input.kind === 'generate' ? input.characters : undefined, replace: input.kind === 'regenerate' ? [] : input.existing,
  };
}

/** The same sibling paths are inspected by the host and handed to native publication. */
export function fontTargetsOf(file: string, request: FontRequest): { output: string; atlas: string; metadata: string } {
  if (request.kind === 'generate') {
    const problem = fontNameProblemOf(request.name);
    if (problem !== undefined) throw new Error(problem);
  }
  const output = request.kind === 'generate'
    ? `${windowsFolder(file).replace(/\\/g, '/')}/${request.name}.fnt`
    : file.replace(/\\/g, '/');
  return { output, atlas: output.slice(0, -4) + '.edds', metadata: `${output}.meta` };
}

export function fontNameProblemOf(name: string): string | undefined {
  if (name.length === 0 || name.trim() !== name || name.endsWith('.') || /[<>:"/\\|?*]/.test(name) ||
    [...name].some((character) => character.charCodeAt(0) < 32)) {
    return 'Enter a filename without separators, control characters, leading/trailing spaces or a trailing dot.';
  }
  if (/^(CON|PRN|AUX|NUL|COM[1-9¹²³]|LPT[1-9¹²³])(?:\.|$)/i.test(name)) return 'This filename is reserved by Windows.';
  // The longest sibling appends .fnt.meta to the chosen stem.
  if (name.length + '.fnt.meta'.length > 255) return 'The font filename is too long.';
  return undefined;
}

export function defaultFontName(family: string, style: string, size: number): string {
  const clean = (value: string) => [...value].filter((character) => character.charCodeAt(0) > 32 && !/[<>:"/\\|?*]/.test(character)).join('');
  return `SDF_${(clean(family) + clean(style)).slice(0, 230)}${size}`;
}

/** Also used before inspection or prompting, so an Explorer menu is never the authority. */
export function fontCommandRefusalOf(input: Pick<FontCommandInput, 'kind' | 'file' | 'scheme' | 'platform' | 'roots'>): string | undefined {
  if (input.platform !== 'win32') return 'Font generation is available on Windows x64.';
  const extension = input.kind === 'generate' ? '.ttf' : '.fnt';
  if (input.scheme !== 'file' || !input.file.toLowerCase().endsWith(extension)) return `Choose one local ${extension} file.`;
  if (!input.roots.some((root) => within(input.file, root.root))) {
    return 'The file is outside every discovered Enfusion root in this window.';
  }
  return undefined;
}

function within(file: string, root: string): boolean {
  return samePath(file).startsWith(`${samePath(root)}/`);
}
