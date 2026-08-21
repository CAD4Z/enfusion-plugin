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
import type { LinkState, WorkDriveAction } from '../mods/workDrive';
import type {
  AddonView,
  EnvironmentView,
  LinkView,
  ManifestFileView,
  ModsMessage,
  ModView,
  PanelRequest,
  WorkDriveView,
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
    workDriveOf(message.workDrive),
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
  builder: 'Builder',
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
 * The work drive: the letter, the folder behind it, and the three buttons. A button that would
 * only fail is disabled and says why, so the reason is there before it is pressed rather than in
 * a message box after.
 */
function workDriveOf(view: WorkDriveView): HTMLElement {
  const card = document.createElement('vscode-collapsible');
  card.heading = 'Work drive';
  card.description = describeDrive(view);
  // Quiet while the drive is up and every mod is on it, and open on whatever is not.
  card.open = view.state !== 'mounted' || view.unlinked > 0;

  const decorations = div('decorations');
  decorations.slot = 'decorations';
  if (view.warning !== undefined) {
    // Which folder it is mounted from is the usual warning; anything else is the drive being
    // out of reach altogether, and calling that "wrong folder" would name the wrong problem.
    const label = view.state === 'elsewhere' ? 'wrong folder' : 'unavailable';
    decorations.append(badge(label, view.warning, 'warning'));
  }
  if (view.unlinked > 0) {
    decorations.append(
      badge(`${view.unlinked} not linked`, 'Mods that are not on the work drive', 'warning'),
    );
  }

  const rows = div('rows');
  rows.append(driveRow(view));
  if (view.warning !== undefined) {
    rows.append(warningRow(view.warning));
  }
  rows.append(buttons(view));

  card.append(...(decorations.hasChildNodes() ? [decorations] : []), rows);
  return card;
}

/** The letter and the folder it is set to, which is what a mount would use. */
function driveRow(view: WorkDriveView): HTMLElement {
  const row = rowButton('Open this setting');
  row.addEventListener('click', () => {
    host.postMessage({ type: 'settings', id: view.setting });
  });

  row.append(span('name', view.letter));
  row.append(
    view.source === ''
      ? span('unset', 'no folder set to mount from')
      : span('value', view.source, 'The folder the work drive is mounted from'),
  );

  if (view.state === 'unmounted') {
    row.append(span('unset', 'not mounted'));
  }

  return row;
}

function warningRow(message: string): HTMLElement {
  const row = staticRow('problem');
  row.append(span('message', message));
  return row;
}

const ACTION_LABELS: Record<WorkDriveAction, string> = {
  mount: 'Mount',
  unmount: 'Unmount',
  link: 'Link mods',
};

const ACTION_TITLES: Record<WorkDriveAction, string> = {
  mount: 'Put the folder the settings name up under the drive letter',
  unmount: 'Take the work drive down and free the letter',
  link: 'Put the prefix root of every mod of this workspace onto the work drive',
};

function buttons(view: WorkDriveView): HTMLElement {
  const row = div('buttons');

  for (const { action, refusal } of view.actions) {
    const button = document.createElement('vscode-button');
    button.textContent = ACTION_LABELS[action];
    button.secondary = action !== 'link';
    button.disabled = refusal !== undefined;
    button.addEventListener('click', () => {
      host.postMessage({ type: 'workDrive', action });
    });

    // On the wrapper rather than the button: a disabled one takes no pointer events, and a
    // tooltip nobody can hover is no way to say why the button is disabled.
    const holder = span('holds', '', refusal ?? ACTION_TITLES[action]);
    holder.append(button);
    row.append(holder);
  }

  return row;
}

function describeDrive(view: WorkDriveView): string {
  switch (view.state) {
    case 'unset':
      return 'no folder set';
    case 'unmounted':
      return `${view.letter} not mounted`;
    case 'mounted':
      return `${view.letter} ${view.source}`;
    case 'elsewhere':
      return `${view.letter} ${view.at}`;
  }
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
  // Only when it is not linked: that is the one that explains a build failing before it runs.
  if (mod.link !== undefined && mod.link.state !== 'linked' && mod.link.state !== 'unavailable') {
    const { label, title } = describeLink(mod.link);
    decorations.append(badge(label, title, 'warning'));
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
  if (mod.link !== undefined) {
    rows.append(linkRow(mod.link));
  }
  rows.append(...mod.addons.map((addon) => addonOf(addon, mod.name)));

  card.append(...(decorations.hasChildNodes() ? [decorations] : []), rows);
  return card;
}

/**
 * One addon: what it packs into, what it is called by whoever requires it, and the button that
 * packs it. The row is not itself a button any more — it holds two, because an addon is both a
 * file to open and a thing to build, and one click cannot mean both.
 */
function addonOf(addon: AddonView, mod: string): HTMLElement {
  const row = staticRow('addon');

  const open = document.createElement('button');
  open.className = 'open';
  open.title = `Open ${addon.name}/config.cpp`;
  open.addEventListener('click', () => {
    host.postMessage({ type: 'open', path: addon.config });
  });

  open.append(span('name', addon.name));
  if (addon.main) {
    open.append(span('tag', 'main', 'Carries CfgMods, so it declares the mod itself'));
  }

  // The class name is the addon's own and has nothing to do with the folder, so it is only worth
  // showing where the two say different things.
  for (const patch of addon.patches.filter((name) => name !== addon.name)) {
    open.append(span('patch', patch, 'The CfgPatches class other addons require it by'));
  }

  for (const required of addon.unresolved) {
    open.append(span('unresolved', required, 'Required, and declared by no addon of this workspace'));
  }

  const build = document.createElement('button');
  build.className = 'action';
  build.textContent = 'Build';
  build.title = `Pack ${addon.name} into ${addon.name}.pbo`;
  build.addEventListener('click', () => {
    host.postMessage({ type: 'build', mod, addon: addon.name });
  });

  row.append(open, build);
  return row;
}

/**
 * Where the mod sits on the work drive. Shown whichever way it went: a build reads the sources
 * through this link and nothing else, so "linked" is as much worth seeing as "not linked".
 */
function linkRow(link: LinkView): HTMLElement {
  const { label, title } = describeLink(link);
  const row = staticRow();

  row.append(span('name', link.path), span('patch', label, title));
  if (link.state === 'elsewhere') {
    row.append(span('unresolved', link.at, 'Where it points now, which is not this mod'));
  }

  return row;
}

const LINK_LABELS: Record<LinkState, { label: string; title: string }> = {
  linked: { label: 'linked', title: 'This mod is on the work drive, where a build reads it from' },
  unlinked: { label: 'not linked', title: 'Nothing is at this path: a build would not find the mod' },
  elsewhere: {
    label: 'links elsewhere',
    title: 'A link, but to another folder: a build would read sources that are not this mod',
  },
  occupied: {
    label: 'in the way',
    title: 'Something that is not a link is at this path, and the extension will not remove it',
  },
  unavailable: { label: 'drive not mounted', title: 'Mount the work drive to link this mod' },
};

function describeLink(link: LinkView): { label: string; title: string } {
  return LINK_LABELS[link.state];
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

/** The same row, for what has nothing behind it to open: a drive letter is not a file. */
function staticRow(kind = ''): HTMLElement {
  return div(kind === '' ? 'row static' : `row static ${kind}`);
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
