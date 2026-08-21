/**
 * The Mods panel, on the browser side of the webview.
 *
 * It renders what it is sent and reports what was clicked; every decision — which folders are
 * mods, in what order, what is wrong with them — was made in `src/mods/` before it got here.
 * Components come from `@vscode-elements/elements`, so the panel follows the editor's theme
 * through the `--vscode-*` variables VS Code puts on the document.
 */

import '@vscode-elements/elements/dist/vscode-badge/index.js';
import '@vscode-elements/elements/dist/vscode-button/index.js';
import '@vscode-elements/elements/dist/vscode-collapsible/index.js';
import type { Problem } from '../mods/model';
import type { AddonView, ModsMessage, ModView, PanelRequest } from './protocol';
import './main.css';

declare function acquireVsCodeApi(): { postMessage(message: PanelRequest): void };

const host = acquireVsCodeApi();
const root = document.body.appendChild(div('mods'));

window.addEventListener('message', (event: MessageEvent<ModsMessage>) => {
  if (event.data.type === 'mods') {
    render(event.data.mods);
  }
});

// The panel is built from scratch every time it becomes visible, so it asks rather than waits.
host.postMessage({ type: 'ready' });

function render(mods: readonly ModView[]): void {
  root.replaceChildren(...(mods.length === 0 ? [nothingFound()] : mods.map(modOf)));
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
  card.heading = mod.name;
  card.description = mod.location;
  card.open = true;

  const decorations = div('decorations');
  decorations.slot = 'decorations';
  if (!mod.configured) {
    decorations.append(badge('not configured', 'No mod.enf: this mod was found by its config.cpp'));
  }
  for (const problem of mod.problems) {
    const { label, title } = describe(problem);
    decorations.append(badge(label, title, 'warning'));
  }

  const addons = div('addons');
  addons.append(...mod.addons.map(addonOf));

  card.append(...(decorations.hasChildNodes() ? [decorations] : []), addons);
  return card;
}

/** One addon: what it packs into, and what it is called by whoever requires it. */
function addonOf(addon: AddonView): HTMLElement {
  const row = document.createElement('button');
  row.className = 'addon';
  row.title = `Open ${addon.name}/config.cpp`;
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
