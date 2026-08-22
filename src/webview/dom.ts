/**
 * The handful of DOM both webviews build everything else out of.
 *
 * The Mods panel and the `.enf` form are rendered by hand — no template, no framework — so both
 * want the same few makers, and a problem out of a `.enf` should read and behave the same wherever
 * it is shown. Everything here is markup and none of it is protocol: what a click means belongs to
 * whichever webview is showing the row, which is why the row takes what to do rather than what to
 * send.
 */

import '@vscode-elements/elements/dist/vscode-badge/index.js';
import type { ManifestProblem } from '../mods/enf';

export function div(className: string): HTMLElement {
  const div = document.createElement('div');
  div.className = className;
  return div;
}

export function span(className: string, text: string, title?: string): HTMLElement {
  const span = document.createElement('span');
  span.className = className;
  span.textContent = text;
  if (title !== undefined) {
    span.title = title;
  }
  return span;
}

export function paragraph(text: string): HTMLElement {
  const paragraph = document.createElement('p');
  paragraph.textContent = text;
  return paragraph;
}

export function badge(text: string, title: string, className = ''): HTMLElement {
  const badge = document.createElement('vscode-badge');
  badge.className = className;
  badge.textContent = text;
  badge.title = title;
  return badge;
}

/**
 * A mistake in a `.enf`, at the line and column it sits on. A button, so that the keyboard reaches
 * the same place the mouse does — the place being the point of it: a problem worth showing is a
 * problem worth landing on.
 */
export function problemRow(problem: ManifestProblem, open: () => void): HTMLElement {
  const row = document.createElement('button');
  row.className = 'row problem';
  row.title = 'Open the file here as text';
  row.addEventListener('click', open);

  row.append(span('where', `${problem.line}:${problem.column}`), span('message', problem.message));
  return row;
}
