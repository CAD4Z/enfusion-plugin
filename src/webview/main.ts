/**
 * The Mods panel, on the browser side of the webview.
 *
 * Two things, one above the other. A row of buttons: the game, the build, and the work drive —
 * everything that acts on the whole workspace rather than on one thing in it. Below it the
 * workspace and its mods, each mod a bar that opens its `mod.enf` and, under it, the addons it
 * packs into pbo.
 *
 * It renders what it is sent and reports what was clicked; every decision — which folders are
 * mods, what they are called, in what order, what is wrong with them, which button would refuse
 * and why — was made in `src/mods/` before it got here.
 */

import '@vscode-elements/elements/dist/vscode-button/index.js';
import type { ManifestProblem } from '../mods/enf';
import type { Problem } from '../mods/model';
import type { LinkState } from '../mods/workDrive';
import { badge, div, icon, paragraph, problemRow as problemOf, span } from './dom';
import type { IconName } from './icons';
import type {
  ActionView,
  AddonView,
  LinkView,
  ManifestFileView,
  ModView,
  ModsMessage,
  PanelRequest,
  ToolsView,
} from './protocol';
import './main.css';

declare function acquireVsCodeApi(): { postMessage(message: PanelRequest): void };

const host = acquireVsCodeApi();
const root = document.body.appendChild(div('mods'));

/**
 * What actually arrives, which is whatever the extension behind this page sent. The two are
 * separate files updated separately: installing a new version over a running one leaves this
 * script new and the extension host still the old one, until the window is reloaded.
 */
type Incoming = Partial<ModsMessage> & { readonly type?: string };

window.addEventListener('message', (event: MessageEvent<Incoming>) => {
  if (event.data.type === 'mods') {
    render(event.data);
  }
});

// The panel is built from scratch every time it becomes visible, so it asks rather than waits.
host.postMessage({ type: 'ready' });

function render(message: Incoming): void {
  const tools = message.tools;

  // A message with nothing this page can read is the extension being older than the page, and
  // saying so is worth more than the blank panel that reading it anyway would leave.
  if (tools === undefined) {
    root.replaceChildren(stale());
    return;
  }

  const mods = message.mods ?? [];
  root.replaceChildren(
    toolsOf(tools),
    ...(message.workspaces ?? []).map(workspaceOf),
    ...(mods.length === 0 ? [nothingFound()] : mods.map(modOf)),
  );
}

/** The one thing that settles a page and an extension host of different ages. */
function stale(): HTMLElement {
  const reload = document.createElement('vscode-button');
  reload.textContent = 'Reload Window';
  reload.title = 'Restart the extension host, so that it is the same version as this panel';
  reload.addEventListener('click', () => {
    host.postMessage({ type: 'reload' });
  });

  const buttons = div('buttons');
  buttons.append(reload);

  const empty = div('empty');
  empty.append(
    paragraph('This panel is newer than the extension running behind it.'),
    // The words as well as the button: the extension that is too old to read this page may be too
    // old to have been told what the button asks for, and then the palette is the way through.
    paragraph('Reload the window — “Developer: Reload Window” — and the mods come back.'),
    buttons,
  );

  return empty;
}

/**
 * The buttons everything else is done with, in the order they are reached for: put the game up,
 * build what it would load, and — off to the side, because they are done once and then forgotten
 * — the three that the work drive is made of.
 */
function toolsOf(tools: ToolsView): HTMLElement {
  const row = div('tools');

  row.append(
    tool('start', tools.start, 'Start', { type: 'launch' }),
    tool('secondClient', tools.secondClient, undefined, { type: 'launchSecondClient' }),
    tool('build', tools.build, undefined, { type: 'buildAll' }),
    div('spacer'),
    ...tools.workDrive.map((action) =>
      tool(action.action, action, undefined, { type: 'workDrive', action: action.action }),
    ),
  );

  return row;
}

/**
 * One button of that row. A label makes it the wide one — there is a single action a panel is
 * mostly opened for, and it should not be a square the same size as the rest.
 *
 * The reason it would refuse rides on the wrapper rather than on the button: a disabled button
 * takes no pointer events, and a tooltip nobody can hover is no way to say why it is disabled.
 */
function tool(
  name: IconName,
  action: ActionView,
  label: string | undefined,
  request: PanelRequest,
): HTMLElement {
  const button = document.createElement('button');
  button.className = label === undefined ? 'tool' : 'tool wide';
  button.disabled = action.refusal !== undefined;
  button.addEventListener('click', () => {
    host.postMessage(request);
  });

  button.append(icon(name));
  if (label !== undefined) {
    button.append(span('label', label));
  }

  const holder = span('holds', '', action.refusal ?? action.title);
  holder.append(button);

  return holder;
}

/**
 * The workspace file, and the mods whose launch block it owns: named rather than implied, because
 * a nearer `workspace.enf` takes the mods under it.
 */
function workspaceOf(file: ManifestFileView): HTMLElement {
  const owns =
    file.owns.length === 0
      ? `${file.location} — the launch block for the mods under it; none of them are here`
      : `${file.location} — owns the launch of ${file.owns.join(', ')}`;

  const block = div('workspace');
  block.append(
    fileRow('workspace.enf', file.path, owns),
    ...file.problems.map((problem) => problemRow(file.path, problem)),
  );

  return block;
}

/**
 * What an empty workspace is shown: the one thing worth doing about it, and the way to look again.
 * Making a mod is the first button rather than a command to be found in the palette — a developer
 * who has not made one yet is the developer least likely to know what it is called.
 */
function nothingFound(): HTMLElement {
  const create = document.createElement('vscode-button');
  create.textContent = 'Create Mod';
  create.title = 'Make a mod in this folder: a mod.enf, a prefix root and an addon that builds';
  create.addEventListener('click', () => {
    host.postMessage({ type: 'init' });
  });

  const refresh = document.createElement('vscode-button');
  refresh.textContent = 'Refresh';
  refresh.secondary = true;
  refresh.addEventListener('click', () => {
    host.postMessage({ type: 'refresh' });
  });

  const buttons = div('buttons');
  buttons.append(create, refresh);

  const empty = div('empty');
  empty.append(
    paragraph('No Enfusion mod was found in this workspace.'),
    paragraph('A mod is a folder with a mod.enf, holding the prefix root the work drive links to.'),
    buttons,
  );

  return empty;
}

/**
 * One mod: its name, whatever is wrong with it, and the addons it packs into pbo.
 *
 * The bar is the manifest — one click, one file, the way a file in the explorer opens — so nothing
 * else is written on it. Its name is the whole of what the mod is called: `mod.enf` says it, and
 * that same name is `P:\<name>` and `@<name>`, so there is no second one to show beside it.
 */
function modOf(mod: ModView): HTMLElement {
  const block = div('mod');
  const manifest = mod.manifest;

  block.append(
    manifest === undefined
      ? unconfiguredRow(mod)
      : modRow(mod, manifest),
    ...(manifest === undefined
      ? [adoptRow(mod.name)]
      : mod.manifestProblems.map((problem) => problemRow(manifest, problem))),
    ...mod.addons.map((addon) => addonOf(addon, mod.name)),
    addAddonRow(mod.name),
  );

  return block;
}

function modRow(mod: ModView, manifest: string): HTMLElement {
  const row = rowButton(`Open the mod.enf of ${mod.name}`, 'bar');
  row.addEventListener('click', () => {
    host.postMessage({ type: 'open', path: manifest });
  });

  row.append(span('name', mod.name), ...marksOf(mod));
  return row;
}

/** A mod with no `mod.enf` has no bar to open one: the row says so, and the line below writes it. */
function unconfiguredRow(mod: ModView): HTMLElement {
  const row = staticRow('bar');
  row.append(span('name', mod.name), ...marksOf(mod));

  return row;
}

/** Everything worth putting next to a mod's name, and nothing that is merely true of it. */
function marksOf(mod: ModView): HTMLElement[] {
  const marks: HTMLElement[] = [];

  if (mod.manifest === undefined) {
    marks.push(
      badge(
        'not configured',
        'No mod.enf: this mod was found by its config.cpp, and can be given one from what it says',
      ),
    );
  }
  // Only when it is not linked: that is the one that explains a build failing before it runs.
  if (mod.link !== undefined && mod.link.state !== 'linked' && mod.link.state !== 'unavailable') {
    marks.push(badgeFor(describeLink(mod.link)));
  }
  for (const problem of mod.problems) {
    marks.push(badgeFor(describe(problem)));
  }
  if (mod.manifestProblems.length > 0) {
    marks.push(
      badge(`${mod.manifestProblems.length} in mod.enf`, 'What the manifest got wrong', 'warning'),
    );
  }

  return marks;
}

function badgeFor({ label, title }: { label: string; title: string }): HTMLElement {
  return badge(label, title, 'warning');
}

/**
 * The one thing an unconfigured mod can do, offered where its `mod.enf` would be shown if it had
 * one. The fields come out of the mod's own `config.cpp`, so the row promises no questions — the
 * command shows what it read and asks only whether to write it down.
 */
function adoptRow(mod: string): HTMLElement {
  return actionRow(
    '+ Create mod.enf',
    `Write a mod.enf for ${mod}, filled in with what its config.cpp already says`,
    { type: 'adopt', mod },
  );
}

/**
 * The way to add an addon to the mod, under the addons it already has. Offered whatever the mod's
 * layout is: a mod that packs into one pbo cannot take one, and being told why by the command that
 * would do it is worth more than a button that is not there.
 */
function addAddonRow(mod: string): HTMLElement {
  return actionRow(
    '+ Add addon',
    `Add an addon to ${mod}: a folder of its own, packed into its own pbo`,
    { type: 'addon', mod },
  );
}

/** A row that does something rather than opening something: the `+` lines under a mod. */
function actionRow(label: string, title: string, request: PanelRequest): HTMLElement {
  const row = rowButton(title, 'add');
  row.addEventListener('click', () => {
    host.postMessage(request);
  });

  row.append(span('name', label));
  return row;
}

/**
 * One addon: what it packs into, what it is called by whoever requires it, and the button that
 * packs it alone — the one above builds the lot. The row is not itself a button: an addon is both
 * a file to open and a thing to build, and one click cannot mean both.
 *
 * What it requires and nothing here declares is not shown. Every mod requires `DZ_Scripts`, so the
 * mark was on every row of every mod, and a mark that is always there says nothing.
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

  const build = document.createElement('button');
  build.className = 'action';
  build.textContent = 'Build';
  build.title = `Pack ${addon.name} into its pbo`;
  build.addEventListener('click', () => {
    host.postMessage({ type: 'build', mod, addon: addon.name });
  });

  row.append(open, build);
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
  const said = LINK_LABELS[link.state];

  return link.state === 'elsewhere'
    ? { label: said.label, title: `${said.title}: ${link.path} points at ${link.at}` }
    : { label: said.label, title: `${said.title}: ${link.path}` };
}

/** A mistake in a `.enf`, which opens the file on the very place it is. */
function problemRow(path: string, problem: ManifestProblem): HTMLElement {
  return problemOf(problem, () => {
    host.postMessage({ type: 'open', path, line: problem.line, column: problem.column });
  });
}

function fileRow(name: string, path: string, title: string): HTMLElement {
  const row = rowButton(title, 'bar');
  row.addEventListener('click', () => {
    host.postMessage({ type: 'open', path });
  });

  row.append(span('name', name));
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

/** A row is a button so that the keyboard reaches everything the mouse does. */
function rowButton(title: string, kind = ''): HTMLElement {
  const row = document.createElement('button');
  row.className = kind === '' ? 'row' : `row ${kind}`;
  row.title = title;
  return row;
}

/** The same row, for what has nothing behind it to open: a mod with no manifest yet. */
function staticRow(kind = ''): HTMLElement {
  return div(kind === '' ? 'row static' : `row static ${kind}`);
}
