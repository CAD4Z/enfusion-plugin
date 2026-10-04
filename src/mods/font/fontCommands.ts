/** Decisions made before a font command writes anything; native code owns recipes and generation. */
import { samePath, windowsFolder } from '../paths';
import type { EnfusionRoot } from '../texture/textureConversion';

/** The atlas font sizes the engine takes, and the size a new font starts with. */
export const FONT_SIZE_MIN = 8;
export const FONT_SIZE_MAX = 40;
export const FONT_SIZE_DEFAULT = 32;

/** Whether the font's `.fnt.meta` recipe exists; native code reads it and refuses one it cannot. */
interface Metadata {
  readonly kind: 'missing' | 'present';
}

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

/** The three sibling files a font command writes. */
export interface FontTargets {
  readonly output: string;
  readonly atlas: string;
  readonly metadata: string;
}

/** A regeneration takes everything from the recipe; a generation names its source and size. */
export type FontCommandPlan =
  | { readonly kind: 'refused'; readonly reason: string }
  | (FontTargets & { readonly kind: 'ready'; readonly resourceName: string; readonly replace: readonly string[] } & (
      | { readonly action: 'regenerate' }
      | {
          readonly action: 'generate' | 'replace';
          readonly source: string;
          readonly size: number;
          readonly characters: string | undefined;
        }
    ));

export function fontCommandPlanOf(input: FontCommandInput): FontCommandPlan {
  const refusal = fontCommandRefusalOf(input);
  if (refusal !== undefined) return { kind: 'refused', reason: refusal };
  if (input.kind === 'regenerate' && input.metadata.kind === 'missing') {
    return { kind: 'refused', reason: 'This font has no .fnt.meta recipe. Generate it from a .ttf first.' };
  }
  if (input.kind === 'generate') {
    const nameProblem = fontNameProblemOf(input.name);
    if (nameProblem !== undefined) return { kind: 'refused', reason: nameProblem };
    if (!Number.isInteger(input.size) || input.size < FONT_SIZE_MIN || input.size > FONT_SIZE_MAX) {
      return { kind: 'refused', reason: `Font size must be a whole number from ${FONT_SIZE_MIN} to ${FONT_SIZE_MAX}.` };
    }
  }
  const targets = fontTargetsOf(input.file, input);
  const root = input.roots.filter((root) => within(input.file, root.root))
    .sort((a, b) => samePath(b.root).length - samePath(a.root).length)[0];
  if (root === undefined) return { kind: 'refused', reason: 'The file is outside every discovered Enfusion root in this window.' };
  const resourceRoot = root.prefixRoot !== undefined && within(targets.output, root.prefixRoot) ? root.prefixRoot : root.root;
  const ready = { kind: 'ready', ...targets, resourceName: targets.output.slice(windowsFolder(resourceRoot).length + 1) } as const;
  if (input.kind === 'regenerate') return { ...ready, action: 'regenerate', replace: [] };
  return {
    ...ready, action: input.existing.length > 0 ? 'replace' : 'generate', replace: input.existing,
    source: input.file, size: input.size, characters: input.characters,
  };
}

/** The same sibling paths are inspected by the host and handed to native publication. */
export function fontTargetsOf(file: string, request: FontRequest): FontTargets {
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

/** What a generated font reports, as far as the user is told about it. */
export interface FontGenerationOutcome {
  readonly glyphCount: number;
  readonly missing: readonly number[];
  readonly thin: readonly number[];
}

/**
 * The line a finished generation ends with, and whether it warns. Thin glyphs can run to hundreds,
 * so at most `thinShown` of them are named.
 */
export function fontGenerationSummaryOf(file: string, outcome: FontGenerationOutcome, thinShown = Infinity): {
  readonly message: string;
  readonly warning: boolean;
} {
  const named = (code: number) => `${String.fromCodePoint(code)} (U+${code.toString(16).toUpperCase().padStart(4, '0')})`;
  const shown = outcome.thin.slice(0, thinShown).map(named);
  const more = outcome.thin.length - shown.length;
  const message = `${file}: ${outcome.glyphCount} glyphs generated.` +
    (outcome.missing.length === 0 ? '' : ` Skipped characters: ${outcome.missing.map(named).join(', ')}.`) +
    (outcome.thin.length === 0 ? '' : ` ${outcome.thin.length} glyphs have strokes thinner than an atlas pixel and break up ` +
      `when drawn larger than the atlas size: ${shown.join(', ')}${more > 0 ? ` and ${more} more` : ''}. ` +
      'A heavier weight or a larger size keeps them whole.');
  return { message, warning: outcome.missing.length > 0 || outcome.thin.length > 0 };
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
