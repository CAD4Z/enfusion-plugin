/**
 * The Mods panel, on the browser side of the webview.
 *
 * It renders what it is sent and reports what was clicked; every decision — which folders are
 * mods, in what order, what is wrong with them, what of the machine resolved — was made in
 * `src/mods/` before it got here. Components come from `@vscode-elements/elements`, so the panel
 * follows the editor's theme through the `--vscode-*` variables VS Code puts on the document.
 */

import '@vscode-elements/elements/dist/vscode-badge/index.js';
import '@vscode-elements/elements/dist/vscode-button/index.js';
import '@vscode-elements/elements/dist/vscode-collapsible/index.js';
import type { ManifestProblem } from '../mods/enf';
import type { EnvironmentEntry, EnvironmentKind } from '../mods/machine';
import type { Problem } from '../mods/model';
import type {
  AddonView,
  EnvironmentView,
  ManifestFileView,
  ModsMessage,
  ModView,
  PanelRequest,
} from './protocol';
import './main.css';

declare function acquireVsCodeApi(): { postMessage(message: PanelRequest): void };

const host = acquireVsCodeApi();
const root = document.body.appendChild(div('mods'));

window.addEventListener('message', (event: MessageEvent<ModsMessage>) => {
  if (event.data.type === 'mods') {
    render(event.data);
  }
});

// The panel is built from scratch every time it becomes visible, so it asks rather than waits.
host.postMessage({ type: 'ready' });

function render(message: ModsMessage): void {
  root.replaceChildren(
    environmentOf(message.environment),
    ...message.workspaces.map(workspaceOf),
    ...(message.mods.length === 0 ? [nothingFound()] : message.mods.map(modOf)),
  );
}

/** What the machine answered for, and what it did not: a refusal seen before the first build. */
function environmentOf(environment: EnvironmentView): HTMLElement {
  const card = document.createElement('vscode-collapsible');
  card.heading = 'Environment';
  card.description = environment.wanting === 0 ? 'ready' : `${environment.wanting} to set`;
  // Quiet while everything resolved, and open on the settings that did not.
  card.open = environment.wanting > 0;

  const entries = div('rows');
  entries.append(...environment.entries.map(entryOf));

  card.append(entries);
  return card;
}

const LABELS: Record<EnvironmentKind, string> = {
  dayz: 'DayZ',
  dayzTools: 'DayZ Tools',
  privateKey: 'Private key',
  workDrive: 'Work drive',
};

function entryOf(entry: EnvironmentEntry): HTMLElement {
  const row = rowButton('Open this setting');
  row.addEventListener('click', () => {
    host.postMessage({ type: 'settings', id: entry.setting });
  });

  row.append(span('name', LABELS[entry.kind]));

  switch (entry.state) {
    case 'ok':
      row.append(span('value', entry.path));
      break;
    case 'missing':
      row.append(
        span('value', entry.path),
        badge('not there', 'The setting points at something that does not exist', 'warning'),
      );
      break;
    case 'unset':
      row.append(
        entry.optional
          ? span('unset', 'not set — pbo go unsigned')
          : span('unset', 'not set, and not in the registry either'),
      );
      break;
  }

  return row;
}

/**
 * The workspace file, and the mods whose launch block it owns: named rather than implied, because
 * a nearer workspace.enf takes the mods under it.
 */
function workspaceOf(file: ManifestFileView): HTMLElement {
  const owns =
    file.owns.length === 0
      ? 'The launch block for the mods under it; none of them are here'
      : `Owns the launch of ${file.owns.join(', ')}`;

  const block = div('workspace');
  block.append(
    fileRow('workspace.enf', file.location, file.path, owns),
    ...file.problems.map((problem) => problemRow(file.path, problem)),
  );

  return block;
}

function nothingFound(): HTMLElement {
  const refresh = document.createElement('vscode-button');
  refresh.textContent = 'Refresh';
  refresh.addEventListener('click', () => {
    host.postMessage({ type: 'refresh' });
  });

  const empty = div('empty');
  empty.append(
    paragraph('No Enfusion mod was found in this workspace.'),
    paragraph('A mod is a folder with a mod.enf, holding the prefix root the work drive links to.'),
    refresh,
  );

  return empty;
}

function modOf(mod: ModView): HTMLElement {
  const card = document.createElement('vscode-collapsible');
  card.heading = mod.title ?? mod.name;
  card.description = mod.description ?? mod.location;
  card.open = true;

  const decorations = div('decorations');
  decorations.slot = 'decorations';
  // The name the mod is linked and loaded under, shown where mod.enf calls the mod something else.
  if (mod.title !== undefined && mod.title !== mod.name) {
    decorations.append(badge(mod.name, 'The prefix root: what the mod is linked and loaded as'));
  }
  if (mod.manifest === undefined) {
    decorations.append(badge('not configured', 'No mod.enf: this mod was found by its config.cpp'));
  }
  for (const problem of mod.problems) {
    const { label, title } = describe(problem);
    decorations.append(badge(label, title, 'warning'));
  }
  if (mod.manifestProblems.length > 0) {
    decorations.append(
      badge(`${mod.manifestProblems.length} in mod.enf`, 'What the manifest got wrong', 'warning'),
    );
  }

  const rows = div('rows');
  const manifest = mod.manifest;
  if (manifest !== undefined) {
    rows.append(fileRow('mod.enf', mod.location, manifest, 'What configures this mod'));
    rows.append(...mod.manifestProblems.map((problem) => problemRow(manifest, problem)));
  }
  rows.append(...mod.addons.map(addonOf));

  card.append(...(decorations.hasChildNodes() ? [decorations] : []), rows);
  return card;
}

/** One addon: what it packs into, and what it is called by whoever requires it. */
function addonOf(addon: AddonView): HTMLElement {
  const row = rowButton(`Open ${addon.name}/config.cpp`);
  row.addEventListener('click', () => {
    host.postMessage({ type: 'open', path: addon.config });
  });

  row.append(span('name', addon.name));
  if (addon.main) {
    row.append(span('tag', 'main', 'Carries CfgMods, so it declares the mod itself'));
  }

  // The class name is the addon's own and has nothing to do with the folder, so it is only worth
  // showing where the two say different things.
  for (const patch of addon.patches.filter((name) => name !== addon.name)) {
    row.append(span('patch', patch, 'The CfgPatches class other addons require it by'));
  }

  for (const required of addon.unresolved) {
    row.append(span('unresolved', required, 'Required, and declared by no addon of this workspace'));
  }

  return row;
}

/** A mistake in a `.enf`, which opens the file on the very place it is. */
function problemRow(path: string, problem: ManifestProblem): HTMLElement {
  const row = rowButton('Open the file here', 'problem');
  row.addEventListener('click', () => {
    host.postMessage({ type: 'open', path, line: problem.line, column: problem.column });
  });

  row.append(span('where', `${problem.line}:${problem.column}`), span('message', problem.message));
  return row;
}

function fileRow(name: string, location: string, path: string, title: string): HTMLElement {
  const row = rowButton(title);
  row.addEventListener('click', () => {
    host.postMessage({ type: 'open', path });
  });

  row.append(span('name', name), span('patch', location));
  return row;
}

function describe(problem: Problem): { label: string; title: string } {
  switch (problem.kind) {
    case 'no-addons':
      return {
        label: 'no addons',
        title: 'Nothing here packs into a pbo: no config.cpp under the mod root',
      };
    case 'cycle':
      return {
        label: 'cycle',
        title: `These require each other in a ring, so no build order holds: ${problem.patches.join(', ')}`,
      };
  }
}

function badge(text: string, title: string, className = ''): HTMLElement {
  const badge = document.createElement('vscode-badge');
  badge.className = className;
  badge.textContent = text;
  badge.title = title;
  return badge;
}

/** A row is a button so that the keyboard reaches everything the mouse does. */
function rowButton(title: string, kind = ''): HTMLElement {
  const row = document.createElement('button');
  row.className = kind === '' ? 'row' : `row ${kind}`;
  row.title = title;
  return row;
}

function span(className: string, text: string, title?: string): HTMLElement {
  const span = document.createElement('span');
  span.className = className;
  span.textContent = text;
  if (title !== undefined) {
    span.title = title;
  }
  return span;
}

function paragraph(text: string): HTMLElement {
  const paragraph = document.createElement('p');
  paragraph.textContent = text;
  return paragraph;
}

function div(className: string): HTMLElement {
  const div = document.createElement('div');
  div.className = className;
  return div;
}
