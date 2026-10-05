/**
 * The Mods panel, on the browser side of the webview.
 *
 * Two things, one above the other. Grouped controls for launch/build and workspace tools —
 * everything that acts on the whole workspace rather than on one thing in it. Below it the
 * workspace and its mods, each mod a bar that opens its `mod.enf` and, under it, the addons it
 * packs into pbo.
 *
 * It renders what it is sent and reports what was clicked; every decision — which folders are
 * mods, what they are called, in what order, what is wrong with them, which button would refuse
 * and why — was made in `src/mods/` before it got here.
 */

import '@vscode-elements/elements/dist/vscode-button/index.js';
import '@vscode-elements/elements/dist/vscode-single-select/index.js';
import type { ManifestProblem } from '../../mods/enf';
import type { Problem } from '../../mods/model';
import type { LinkState } from '../../mods/workDrive';
import { badge, div, icon, paragraph, problemRow as problemOf, span } from '../dom';
import type { IconName } from '../icons';
import { isStolenPress } from '../press';
import type {
  ActionView,
  AddonView,
  EmptyAction,
  EmptyView,
  LinkView,
  ManifestFileView,
  ModView,
  ModsMessage,
  PanelRequest,
  PickerView,
  ToolsView,
} from './protocol';
import './main.css';

declare function acquireVsCodeApi(): { postMessage(message: PanelRequest): void };

const host = acquireVsCodeApi();
const root = document.body.appendChild(div('mods'));

// Until the first answer comes the panel has nothing to show, and a blank one reads as broken: the
// search can take a while over a large folder — over a minute on the root of a drive.
root.append(centred(['Looking for mods…'], []));

/**
 * When this page last got the focus back, and how long after that a press is not believed.
 *
 * A build starts the builder in a console of its own, once per addon, and that console takes the
 * focus off the editor for as long as it is up. What comes back with the focus is a press on the
 * button that still holds it: three addons packed, three presses nobody made, each landing about
 * a sixth of a second after a console appeared. With a queue behind the button that is a build
 * feeding itself for as long as the developer lets it run.
 *
 * So a synthetic press within a moment of the focus returning is not one, while a pointer press is
 * always believed. A button clicked with the mouse also lets go of the focus rather than sitting
 * there waiting to be pressed by the next console. A click made with the keyboard keeps it: that
 * focus is the one being navigated with.
 */
let focusedAt = 0;

window.addEventListener('focus', () => {
  focusedAt = Date.now();
});

/** A button that does something, rather than one that opens a file: pressed once per press. */
function acts(button: HTMLElement, request: PanelRequest): void {
  button.addEventListener('click', (event) => {
    if (event instanceof MouseEvent && isStolenPress(event.detail, Date.now() - focusedAt)) {
      return;
    }

    if (event instanceof MouseEvent && event.detail > 0) {
      button.blur();
    }

    host.postMessage(request);
  });
}

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
  const workspaces = (message.workspaces ?? []).map(workspaceOf);

  // Every button over the list acts on a mod, so a panel without one shows the way to make one
  // instead — in the middle of it, rather than under a row of buttons that would all be disabled.
  if (mods.length === 0) {
    root.replaceChildren(...workspaces, nothingFound(message.empty));
    return;
  }

  // An addon's own Build refuses for the same reasons the one above every addon does.
  const build = tools.build.refusal;
  root.replaceChildren(
    toolsOf(tools, message.restricted),
    ...workspaces,
    ...mods.map((mod) => modOf(mod, build)),
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
 * The buttons everything else is done with, in the order they are reached for: say what the next
 * launch is, put the game up, build what it would load, and manage the work drive or Workbench.
 *
 * The two lists and their buttons share one named group. They stand in the buttons' own columns — the
 * target over Start and Add client, the build over Build — so that the two rows read as one block
 * at every width the panel is dragged to.
 */
function toolsOf(tools: ToolsView, restricted: string | undefined): HTMLElement {
  const primary = div('tool-group');
  const primaryActions = div('tool-row primary-actions');
  const choices = div('choices');
  choices.append(
    picker('target', tools.target, (id) => ({ type: 'selectTarget', id })),
    picker('game-build', tools.gameBuild, (build) => ({ type: 'selectGameBuild', build })),
  );
  primary.append(span('tool-heading', 'Launch & build'), choices, primaryActions);
  primaryActions.append(
    tool('start', tools.start, 'Start', { type: 'launch' }, true),
    tool('secondClient', tools.secondClient, 'Add client', { type: 'launchSecondClient' }),
    tool('build', tools.build, 'Build', { type: 'buildAll' }),
  );

  const workDrive = div('tool-group');
  const driveActions = div('tool-row drive-actions');
  workDrive.append(span('tool-heading', 'Workspace tools'), driveActions);
  driveActions.append(
    ...tools.workDrive.map((action) =>
      tool(
        action.action,
        action,
        action.action === 'link' ? 'Link mods' : titleCase(action.action),
        { type: 'workDrive', action: action.action },
      ),
    ),
    tool('workbench', tools.workbench, 'Workbench', { type: 'workbench' }),
  );

  const toolsRoot = div('tools');
  toolsRoot.append(...(restricted === undefined ? [] : [restrictedOf(restricted)]), primary, workDrive);
  return toolsRoot;
}

/**
 * Why most of the buttons under it are grey, in a folder the editor does not trust: said over them,
 * with the one press that settles it, rather than left to tooltips nobody hovers. It stands in the
 * same sticky block as the buttons, so it is there wherever the list is scrolled to.
 */
function restrictedOf(text: string): HTMLElement {
  const trust = document.createElement('vscode-button');
  trust.textContent = 'Trust Folder';
  trust.title = 'Open the editor’s own question of whether this folder is trusted';
  trust.addEventListener('click', () => {
    host.postMessage({ type: 'trust' });
  });

  const notice = div('notice');
  notice.append(span('text', text), trust);

  return notice;
}

/**
 * One of the two lists over the buttons.
 *
 * A `change` rather than a press, so it costs one click rather than a click and a pick — and it is
 * never a stolen press: the focus a returning console steals lands on whatever it was on, and
 * `change` fires when a value is chosen rather than when a control is touched.
 *
 * A list with nothing in it is disabled and says why, the way the buttons under it do. It is never
 * left blank: an empty control that gives no reason is the one thing worse than a missing one.
 */
function picker(
  name: string,
  view: PickerView,
  chose: (id: string) => PanelRequest,
): HTMLElement {
  const select = document.createElement('vscode-single-select');
  select.className = 'choice';
  select.disabled = view.refusal !== undefined || view.options.length === 0;
  select.title = view.refusal ?? view.title;
  select.append(
    ...view.options.map((option) => {
      const item = document.createElement('vscode-option');
      item.value = option.id;
      item.textContent = option.label;
      item.selected = option.id === view.chosen;
      return item;
    }),
  );

  select.addEventListener('change', () => {
    // An empty value is the row that stands for nothing having been chosen, and choosing nothing
    // is not a choice. The comparison is what makes a render that settles on its own value quiet.
    if (select.value !== '' && select.value !== view.chosen) {
      host.postMessage(chose(select.value));
    }
  });

  const holder = span(`holds ${name}`, '', view.refusal ?? view.title);
  holder.append(select);

  return holder;
}

/**
 * The reason it would refuse rides on the wrapper rather than on the button: a disabled button
 * takes no pointer events, and a tooltip nobody can hover is no way to say why it is disabled.
 */
function tool(
  name: IconName,
  action: ActionView,
  label: string,
  request: PanelRequest,
  primary = false,
): HTMLElement {
  const button = document.createElement('button');
  button.className = primary ? 'tool primary' : 'tool';
  button.disabled = action.refusal !== undefined;
  button.title = action.refusal ?? action.title;
  acts(button, request);

  button.append(icon(name), span('label', label));

  const holder = span('holds', '', action.refusal ?? action.title);
  holder.append(button);

  return holder;
}

function titleCase(value: string): string {
  return value.charAt(0).toUpperCase() + value.slice(1);
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
 * What an empty panel is shown: why it is empty, and what can be made of it. Making a mod is a
 * button rather than a command to be found in the palette — a developer who has not made one yet
 * is the developer least likely to know what it is called. Which buttons, and in what words, is
 * the extension's to say; an extension too old to say it gets the one button every version has.
 */
function nothingFound(view: EmptyView | undefined): HTMLElement {
  const shown = view ?? { lines: ['No Enfusion mod was found in this workspace.'], actions: ['init'] };

  return centred(shown.lines, shown.actions);
}

/** The words and the buttons of an empty panel, in the middle of it, the first button foremost. */
function centred(lines: readonly string[], actions: readonly EmptyAction[]): HTMLElement {
  const buttons = div('buttons');
  buttons.append(
    ...actions.map((action, at) => {
      const said = EMPTY_ACTIONS[action];
      const button = document.createElement('vscode-button');
      button.textContent = said.label;
      button.title = said.title;
      button.secondary = at > 0;
      button.addEventListener('click', () => {
        host.postMessage(said.request);
      });
      return button;
    }),
  );

  const block = div('welcome');
  block.append(...lines.map(paragraph), ...(actions.length === 0 ? [] : [buttons]));

  return block;
}

const EMPTY_ACTIONS: Record<EmptyAction, { label: string; title: string; request: PanelRequest }> = {
  init: {
    label: 'Create Mod',
    title: 'Make a mod: a mod.enf, a prefix root with a config.cpp and scripts, and a Workbench project',
    request: { type: 'init' },
  },
  initWorkspace: {
    label: 'Create Workspace',
    title: 'Make this folder a workspace of several mods: a workspace.enf that launches them all, and their Workbench project',
    request: { type: 'initWorkspace' },
  },
  openFolder: {
    label: 'Open Folder',
    title: 'Open the folder to make a mod or a workspace in',
    request: { type: 'openFolder' },
  },
};

/**
 * One mod: its name, whatever is wrong with it, and the addons it packs into pbo.
 *
 * The bar is the manifest — one click, one file, the way a file in the explorer opens — so nothing
 * else is written on it. Its name is the whole of what the mod is called: `mod.enf` says it, and
 * that same name is `P:\<name>` and `@<name>`, so there is no second one to show beside it.
 */
function modOf(mod: ModView, buildRefusal: string | undefined): HTMLElement {
  const block = div('mod');
  const manifest = mod.manifest;

  block.append(
    manifest === undefined
      ? unconfiguredRow(mod)
      : modRow(mod, manifest),
    ...(manifest === undefined
      ? [adoptRow(mod.name)]
      : mod.manifestProblems.map((problem) => problemRow(manifest, problem))),
    ...mod.addons.map((addon) => addonOf(addon, mod.name, buildRefusal)),
    addAddonRow(mod.name),
  );

  return block;
}

function modRow(mod: ModView, manifest: string): HTMLElement {
  const label = modLabelOf(mod);
  const row = rowButton(`Open the mod.enf of ${label}`, 'bar');
  row.addEventListener('click', () => {
    host.postMessage({ type: 'open', path: manifest });
  });

  row.append(span('name', label), ...marksOf(mod));
  return row;
}

/** A mod with no `mod.enf` has no bar to open one: the row says so, and the line below writes it. */
function unconfiguredRow(mod: ModView): HTMLElement {
  const row = staticRow('bar');
  row.append(span('name', modLabelOf(mod)), ...marksOf(mod));

  return row;
}

/** Everything worth putting next to a mod's name, and nothing that is merely true of it. */
function marksOf(mod: ModView): HTMLElement[] {
  const marks: HTMLElement[] = [];
  const nameProblemShown = mod.problems.some((problem) => problem.kind === 'invalid-name');

  if (mod.manifest === undefined) {
    marks.push(
      badge(
        'not configured',
        'No mod.enf: this mod was found by its config.cpp, and can be given one from what it says',
      ),
    );
  }
  // Only when it is not linked: that is the one that explains a build failing before it runs.
  if (
    mod.link !== undefined &&
    mod.link.state !== 'linked' &&
    mod.link.state !== 'unavailable' &&
    !(mod.link.state === 'invalid' && nameProblemShown)
  ) {
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

/** Keep an explicitly empty invalid name visible without changing the value sent back in commands. */
function modLabelOf(mod: ModView): string {
  return mod.name.trim() === '' ? '(unnamed mod)' : mod.name;
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
function addonOf(addon: AddonView, mod: string, buildRefusal: string | undefined): HTMLElement {
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
  build.disabled = buildRefusal !== undefined;
  build.title = buildRefusal ?? `Pack ${addon.name} into its pbo`;
  acts(build, { type: 'build', mod, addon: addon.name });

  // A disabled button takes no pointer events, so the reason rides on what holds it.
  const holder = span('holds', '', build.title);
  holder.append(build);

  row.append(open, holder);
  return row;
}

const LINK_LABELS: Record<LinkState, { label: string; title: string }> = {
  invalid: {
    label: 'invalid name',
    title: 'No work-drive path is read or changed until the mod has a usable name',
  },
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

  if (link.state === 'invalid') {
    return { label: said.label, title: link.problem ?? said.title };
  }

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
    case 'invalid-name':
      return { label: 'invalid name', title: problem.reason };
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
