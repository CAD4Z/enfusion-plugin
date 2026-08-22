/**
 * The `.enf` editor, on the browser side of the webview.
 *
 * A form over a text file, not a replacement for it: every box is a field of the manifest, and
 * every change to one goes back as a move — this field, this value — for the extension to work out
 * as an edit of the document. The form holds no state of its own and never composes a manifest;
 * what it shows is what it was last sent, which is the file as the editor has it this instant.
 *
 * That is also why the whole of it is rebuilt on every message rather than patched: the file may
 * have been edited as text, and a form that patched only what it thought had changed would be
 * arguing with the editor about a file the editor owns. What a rebuild costs is the focus, so the
 * focus is put back where it was.
 *
 * Components come from `@vscode-elements/elements`, so the form follows the editor's theme through
 * the `--vscode-*` variables VS Code puts on the document.
 */

import '@vscode-elements/elements/dist/vscode-button/index.js';
import '@vscode-elements/elements/dist/vscode-form-group/index.js';
import '@vscode-elements/elements/dist/vscode-form-helper/index.js';
import '@vscode-elements/elements/dist/vscode-label/index.js';
import '@vscode-elements/elements/dist/vscode-option/index.js';
import '@vscode-elements/elements/dist/vscode-single-select/index.js';
import '@vscode-elements/elements/dist/vscode-textfield/index.js';
import type { ManifestProblem, Run, Target } from '../mods/enf';
import type { FormPath } from '../mods/form';
import { badge, div, paragraph, problemRow, span } from './dom';
import type { FormRequest, ManifestMessage } from './formProtocol';
import './form.css';

declare function acquireVsCodeApi(): { postMessage(message: FormRequest): void };

const host = acquireVsCodeApi();
const root = document.body.appendChild(div('form'));

window.addEventListener('message', (event: MessageEvent<ManifestMessage>) => {
  if (event.data.type === 'manifest') {
    render(event.data);
  }
});

// The form is built from scratch every time it becomes visible, so it asks rather than waits.
host.postMessage({ type: 'ready' });

function render(message: ManifestMessage): void {
  const focused = focusedField();
  // A file the form refuses is still a file shown: the boxes hold what was read and take nothing.
  const writable = message.refusal === undefined;

  root.replaceChildren(
    header(message),
    ...(message.refusal === undefined ? [] : [refusal(message.refusal)]),
    ...(message.problems.length === 0 ? [] : [problemsOf(message.problems)]),
    ...(message.mod === undefined ? [] : [modSection(message.mod, writable)]),
    launchSection(message, writable),
  );

  restore(focused);
}

/** The file being edited, and the way back to it as text, which is what it goes on being. */
function header(message: ManifestMessage): HTMLElement {
  const text = document.createElement('vscode-button');
  text.textContent = 'Edit as Text';
  text.secondary = true;
  text.title = `Open ${message.file} in the text editor, where it is JSONC with a schema behind it`;
  text.addEventListener('click', () => {
    host.postMessage({ type: 'text' });
  });

  const row = div('header');
  row.append(span('file', message.file), text);
  return row;
}

/**
 * Why the form is showing the file and not writing into it. The way to fix it is the text editor,
 * so that is the button — a form that offered to write over a file it could not read whole would
 * be offering to lose the half of it that parsed.
 */
function refusal(message: string): HTMLElement {
  const open = document.createElement('vscode-button');
  open.textContent = 'Edit as Text';
  open.addEventListener('click', () => {
    host.postMessage({ type: 'text' });
  });

  const banner = div('banner');
  banner.append(paragraph(message), open);
  return banner;
}

/** What the file got wrong, each opening the text editor on the very place it is wrong. */
function problemsOf(problems: readonly ManifestProblem[]): HTMLElement {
  const rows = div('problems');
  rows.append(...problems.map(problemAt));
  return rows;
}

function problemAt(problem: ManifestProblem): HTMLElement {
  return problemRow(problem, () => {
    host.postMessage({ type: 'text', line: problem.line, column: problem.column });
  });
}

/** What the mod says about itself, which is the half of `mod.enf` a `workspace.enf` has not got. */
function modSection(mod: NonNullable<ManifestMessage['mod']>, writable: boolean): HTMLElement {
  return section(
    'Mod',
    undefined,
    field(
      {
        path: ['name'],
        label: 'Name',
        help: 'How the mod is shown, in the panel and in the launcher.',
        value: mod.name,
        placeholder: "The prefix root's name",
      },
      writable,
    ),
    field(
      {
        path: ['description'],
        label: 'Description',
        help: 'What the mod does, in a sentence.',
        value: mod.description,
      },
      writable,
    ),
    field({ path: ['author'], label: 'Author', help: 'Who made it.', value: mod.author }, writable),
    field(
      {
        path: ['version'],
        label: 'Version',
        help: 'The version the launcher shows.',
        value: mod.version,
      },
      writable,
    ),
    list(
      {
        path: ['exclude'],
        label: 'Exclude',
        help:
          'Masks the builder is not to pack: model sources, texture sources, whatever a pbo has ' +
          'no business carrying. Naming any replaces the default list whole.',
        items: mod.exclude,
        placeholder: '*.psd',
      },
      writable,
    ),
  );
}

/**
 * Everything about getting the game up. Shown for both files, because both may write it — and
 * where a `workspace.enf` owns this mod's block, that is said here rather than left to be found
 * out by a launch that ignored what was typed.
 */
function launchSection(message: ManifestMessage, writable: boolean): HTMLElement {
  const owned = message.ownedBy;
  const note =
    owned === undefined
      ? undefined
      : `${owned} owns the launch of this mod, so this block is ignored whole — no level of it ` +
        'is merged in. Edit it there, or take that file away.';

  return section(
    'Launch',
    note,
    field(
      {
        path: ['launch', 'modsDirectory'],
        label: 'Mods directory',
        help:
          'Where the @<Mod> folders live: the builder writes them here and the game is pointed ' +
          'at them here. A relative path is counted from this file.',
        value: message.launch.modsDirectory,
        placeholder: 'Addons',
      },
      writable,
    ),
    list(
      {
        path: ['launch', 'clientMods'],
        label: 'Client mods',
        help: 'Third-party mods to hand the client, by the name of their folder.',
        items: message.launch.clientMods,
        placeholder: '@CF',
      },
      writable,
    ),
    list(
      {
        path: ['launch', 'serverMods'],
        label: 'Server mods',
        help: 'Third-party mods to hand the server alone, by the name of their folder.',
        items: message.launch.serverMods,
        placeholder: '@Expansion-Core',
      },
      writable,
    ),
    targetsOf(message.launch.targets, writable),
  );
}

/** The entries of the Run and Debug list, one card each. */
function targetsOf(targets: readonly Target[], writable: boolean): HTMLElement {
  const add = document.createElement('vscode-button');
  add.textContent = '+ Add target';
  add.secondary = true;
  add.disabled = !writable;
  add.title = 'Add an entry to the Run and Debug list, named so that no other target is';
  add.addEventListener('click', () => {
    host.postMessage({ type: 'edit', edit: { kind: 'addTarget' } });
  });

  const block = div('targets');
  const nothing =
    targets.length === 0 ? [empty('No target, so nothing of this is offered to Run and Debug.')] : [];

  block.append(
    label('Targets'),
    helper(
      'The entries of the Run and Debug list. Each says what to put up, and where its profile ' +
        'and its mission come from.',
    ),
    ...targets.map((target, at) => targetOf(target, at, writable)),
    ...nothing,
    buttons(add),
  );

  return block;
}

function targetOf(target: Target, at: number, writable: boolean): HTMLElement {
  const path: FormPath = ['launch', 'targets', at];

  const remove = document.createElement('vscode-button');
  remove.textContent = 'Remove';
  remove.secondary = true;
  remove.disabled = !writable;
  remove.title = `Take ${target.name} out of the Run and Debug list`;
  remove.addEventListener('click', () => {
    host.postMessage({ type: 'edit', edit: { kind: 'clear', path } });
  });

  const card = div('card');
  card.append(
    field(
      {
        path: [...path, 'name'],
        label: 'Name',
        help: 'What the Run and Debug list shows this target as.',
        value: target.name,
      },
      writable,
    ),
    field(
      {
        path: [...path, 'mod'],
        label: 'Mod',
        help: 'The mod the profile, the mission and the server.cfg come from.',
        value: target.mod,
        placeholder: 'The mod being launched',
      },
      writable,
    ),
    field(
      {
        path: [...path, 'map'],
        label: 'Map',
        help: 'The world this target is about, which is the tail of the mission folder name.',
        value: target.map,
        placeholder: 'ChernarusPlus',
      },
      writable,
    ),
    choice(
      {
        path: [...path, 'run'],
        label: 'Run',
        help: 'What this target puts up.',
        value: target.run,
      },
      writable,
    ),
    field(
      {
        path: [...path, 'serverConfig'],
        label: 'Server config',
        help: 'The server.cfg the dev server is started with, relative to the target’s mod.',
        value: target.serverConfig,
        placeholder: "The mod's own server.cfg",
      },
      writable,
    ),
    buttons(remove),
  );

  return card;
}

interface FieldView {
  readonly path: FormPath;
  readonly label: string;
  readonly help: string;
  readonly value: string | undefined;
  /** What the field means when nobody answered it, which is not the same as what it holds. */
  readonly placeholder?: string;
}

/**
 * One field of the manifest. Emptying it is what takes the field out of the file, so the
 * placeholder says what the manifest falls back to rather than showing an example of a value.
 */
function field(view: FieldView, writable: boolean): HTMLElement {
  const id = idOf(view.path);
  const input = document.createElement('vscode-textfield');
  input.id = id;
  input.value = view.value ?? '';
  input.placeholder = view.placeholder ?? '';
  input.disabled = !writable;
  input.addEventListener('change', () => {
    host.postMessage({
      type: 'edit',
      edit: { kind: 'set', path: view.path, value: input.value.trim() },
    });
  });

  return group(label(view.label, id), input, helper(view.help));
}

/**
 * What a target puts up, with what each answer means. Written here rather than imported so that
 * the form carries no runtime of the domain's; `Record<Run, …>` is what keeps the two from
 * drifting, a run this list has not got being a compile error.
 */
const RUNS: Record<Run, string> = {
  client: 'The client alone.',
  server: 'The server alone.',
  both: 'The server, and a client that joins it.',
};

/** The one field with a closed set of answers, so it is offered rather than typed. */
function choice(view: FieldView, writable: boolean): HTMLElement {
  const id = idOf(view.path);
  const select = document.createElement('vscode-single-select');
  select.id = id;
  select.disabled = !writable;
  select.append(
    ...Object.entries(RUNS).map(([run, means]) => {
      const option = document.createElement('vscode-option');
      option.value = run;
      option.description = means;
      option.textContent = run;
      option.selected = run === view.value;
      return option;
    }),
  );
  select.addEventListener('change', () => {
    host.postMessage({
      type: 'edit',
      edit: { kind: 'set', path: view.path, value: select.value },
    });
  });

  return group(label(view.label, id), select, helper(view.help));
}

interface ListView {
  readonly path: FormPath;
  readonly label: string;
  readonly help: string;
  readonly items: readonly string[];
  readonly placeholder: string;
}

/**
 * A list of plain strings: a box per item, and an empty one under them to add by. The empty box is
 * the form's own and nothing else — the file learns of it only once something is typed in it — so
 * a list nobody has added to costs the file no `[]` it never asked for.
 */
function list(view: ListView, writable: boolean): HTMLElement {
  const rows = view.items.map((item, at) => itemRow(view, item, at, writable));

  const adding = document.createElement('vscode-textfield');
  adding.id = idOf([...view.path, 'add']);
  adding.placeholder = view.placeholder;
  adding.title = `Type here to add to ${view.label.toLowerCase()}`;
  adding.disabled = !writable;
  adding.addEventListener('change', () => {
    host.postMessage({
      type: 'edit',
      edit: { kind: 'append', path: view.path, value: adding.value.trim() },
    });
  });

  return group(label(view.label), ...rows, adding, helper(view.help));
}

function itemRow(view: ListView, item: string, at: number, writable: boolean): HTMLElement {
  const path: FormPath = [...view.path, at];

  const input = document.createElement('vscode-textfield');
  input.id = idOf(path);
  input.value = item;
  input.disabled = !writable;
  input.addEventListener('change', () => {
    host.postMessage({
      type: 'edit',
      edit: { kind: 'set', path, value: input.value.trim() },
    });
  });

  const remove = document.createElement('button');
  remove.className = 'remove';
  remove.textContent = '×';
  remove.disabled = !writable;
  remove.title = `Take ${item} off the list`;
  remove.addEventListener('click', () => {
    host.postMessage({ type: 'edit', edit: { kind: 'clear', path } });
  });

  const row = div('item');
  row.append(input, remove);
  return row;
}

function section(
  heading: string,
  note: string | undefined,
  ...content: readonly HTMLElement[]
): HTMLElement {
  const block = document.createElement('section');
  block.append(
    title(heading),
    ...(note === undefined ? [] : [ignored(note)]),
    ...content,
  );

  return block;
}

/** Said where a block is written but obeyed elsewhere, which is a trap worth heading off. */
function ignored(note: string): HTMLElement {
  const row = div('ignored');
  row.append(badge('ignored here', note, 'warning'), span('message', note));
  return row;
}

function group(...content: readonly HTMLElement[]): HTMLElement {
  const form = document.createElement('vscode-form-group');
  form.variant = 'vertical';
  form.append(...content);
  return form;
}

function label(text: string, forId?: string): HTMLElement {
  const label = document.createElement('vscode-label');
  if (forId !== undefined) {
    label.htmlFor = forId;
  }
  label.textContent = text;
  return label;
}

function helper(text: string): HTMLElement {
  const helper = document.createElement('vscode-form-helper');
  helper.append(paragraph(text));
  return helper;
}

function buttons(...content: readonly HTMLElement[]): HTMLElement {
  const row = div('buttons');
  row.append(...content);
  return row;
}

function title(text: string): HTMLElement {
  const heading = document.createElement('h2');
  heading.textContent = text;
  return heading;
}

function empty(text: string): HTMLElement {
  const line = paragraph(text);
  line.className = 'nothing';
  return line;
}

/** An id a `for` can be looked up by, so every box has a label pointing at it and a way back. */
function idOf(path: FormPath): string {
  return ['field', ...path].join('-');
}

/**
 * The box the developer was in when the message arrived, so a rebuild does not throw them out of
 * it. Only the caret's place is lost, and a change reaches this form on leaving a box anyway.
 */
function focusedField(): string {
  const active = document.activeElement;
  return active instanceof HTMLElement ? active.id : '';
}

function restore(id: string): void {
  if (id === '') {
    return;
  }

  document.getElementById(id)?.focus();
}

