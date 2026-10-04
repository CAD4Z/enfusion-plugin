import { textureSurfaceControls } from './textureSurface';
/** Compact batch authoring UI: one item list, one common profile, one active viewport. */

import { profileFormControls } from './profileForm';
import {
  textureBatchProgress,
  textureBatchRetryCount,
  type TextureBatchAuthoringState,
} from '../../mods/texture/textureBatchAuthoring';
import type { TextureProfile, TextureSourceFormat } from '../../mods/texture/textureConversion';
import type { TextureBatchRequest, TextureBatchStateMessage } from './textureBatchProtocol';
import './textureBatch.css';

declare function acquireVsCodeApi(): { postMessage(message: TextureBatchRequest): void };

const host = acquireVsCodeApi();
const root = document.body.appendChild(element('main', 'batch-editor'));

/** The rendering's result, drawn once when its pixels arrive and moved, not redrawn, after that. */
let canvas: HTMLCanvasElement | undefined;

window.addEventListener('message', (event: MessageEvent<TextureBatchStateMessage>) => {
  if (event.data.type !== 'state') return;
  if (event.data.rendering !== undefined) canvas = canvasOf(event.data.rendering?.result);
  render(event.data.state);
});
host.postMessage({ type: 'ready' });

function canvasOf(result: { readonly width: number; readonly height: number; readonly rgba: Uint8Array } | undefined): HTMLCanvasElement | undefined {
  if (result === undefined) return undefined;
  const drawn = document.createElement('canvas');
  drawn.width = result.width;
  drawn.height = result.height;
  drawn.style.width = `${result.width}px`;
  drawn.style.height = `${result.height}px`;
  drawn.getContext('2d')?.putImageData(new ImageData(
    new Uint8ClampedArray(result.rgba),
    result.width,
    result.height,
  ), 0, 0);
  return drawn;
}

function render(state: TextureBatchAuthoringState): void {
  if (state.kind === 'loading') {
    root.replaceChildren(message('Capturing selection, ownership, and revisions…'));
    return;
  }
  if (state.kind === 'refused') {
    root.replaceChildren(message(state.reason, 'error'));
    return;
  }

  const list = element('section', 'item-list');
  const title = document.createElement('h1');
  title.textContent = `${state.plan.items.length} selected textures`;
  list.append(title);
  for (const planned of state.plan.items) {
    const runtime = state.kind === 'authoring'
      ? undefined
      : state.items.find(({ source }) => source === planned.source);
    const row = document.createElement('button');
    row.type = 'button';
    row.className = `item-row${planned.source === state.activeSource ? ' active' : ''}`;
    row.disabled = state.kind !== 'authoring';
    row.addEventListener('click', () => host.postMessage({ type: 'select-item', source: planned.source }));
    const name = document.createElement('span');
    name.className = 'item-name';
    name.textContent = leaf(planned.source);
    name.title = planned.source;
    const status = document.createElement('span');
    status.className = planned.kind === 'refused' || runtime?.status === 'Failed' ? 'item-status error' : 'item-status';
    status.textContent = planned.kind === 'refused'
      ? `Refused · ${planned.reason}`
      : runtime === undefined
        ? planned.label
        : `${runtime.status}${runtime.status === 'Converting' ? ` · ${Math.round(runtime.progress * 100)}%` : ''}${runtime.reason === undefined ? '' : ` · ${runtime.reason}`}`;
    const progress = document.createElement('progress');
    progress.max = 1;
    progress.value = runtime?.progress ?? 0;
    row.append(name, status, progress);
    list.append(row);
  }

  const workspace = element('section', 'active-workspace');
  const activeTitle = document.createElement('h2');
  activeTitle.textContent = leaf(state.activeSource);
  workspace.append(activeTitle);
  const rendered = state.kind === 'authoring'
    ? state.preview.kind === 'ready' ? state.preview.rendered : undefined
    : state.rendered;
  if (rendered === undefined || canvas === undefined) {
    workspace.append(message(
      state.kind === 'authoring' && state.preview.kind === 'unavailable'
        ? state.preview.reason
        : 'Rendering one active preview…',
      state.kind === 'authoring' && state.preview.kind === 'unavailable' ? 'error' : '',
    ));
  } else {
    const viewport = element('div', 'viewport');
    viewport.append(canvas);
    workspace.append(textureSurfaceControls(rendered.result, (surface) => {
      const selected = canvasOf(surface);
      if (selected !== undefined) { canvas = selected; viewport.replaceChildren(selected); }
    }));
    workspace.append(viewport);
  }

  const sidebar = element('aside', 'batch-sidebar');
  const heading = document.createElement('h2');
  heading.textContent = 'Common texture profile';
  sidebar.append(
    heading,
    profileForm(state.draft, state.plan.jobs.map((job) => job.sourceFormat), state.kind !== 'authoring'),
  );
  if (state.kind === 'running' || state.kind === 'result') {
    const overall = document.createElement('progress');
    overall.className = 'overall-progress';
    overall.max = 1;
    overall.value = textureBatchProgress(state);
    sidebar.append(overall);
  }
  const action = document.createElement('button');
  action.className = 'primary-action';
  action.type = 'button';
  if (state.kind === 'authoring') {
    action.textContent = `Convert ${state.plan.jobs.length} textures`;
    action.disabled = state.plan.jobs.length === 0;
    action.addEventListener('click', () => host.postMessage({ type: 'run' }));
  } else if (state.kind === 'running') {
    action.textContent = state.cancelling ? 'Cancelling…' : 'Cancel';
    action.disabled = state.cancelling;
    action.addEventListener('click', () => host.postMessage({ type: 'cancel' }));
  } else {
    const failed = textureBatchRetryCount(state);
    action.textContent = state.checkingRetry
      ? 'Checking failed textures…'
      : failed === 0 ? 'Batch complete' : `Retry Failed (${failed})`;
    action.disabled = state.checkingRetry || failed === 0;
    action.addEventListener('click', () => host.postMessage({ type: 'retry-failed' }));
    if (state.retryReason !== undefined) sidebar.append(message(state.retryReason, 'error'));
  }
  sidebar.append(action);
  root.replaceChildren(list, workspace, sidebar);
}

function profileForm(
  profile: TextureProfile,
  sourceFormat: readonly TextureSourceFormat[],
  locked: boolean,
): HTMLElement {
  const form = element('div', 'profile-form');
  form.append(...profileFormControls(profile, sourceFormat, locked, {
    compression: (value) => host.postMessage({ type: 'change-compression', value }),
    threshold: (value) => host.postMessage({ type: 'change-threshold', value }),
    removeMips: (value) => host.postMessage({ type: 'change-remove-mips', value }),
    containsMips: (value) => host.postMessage({ type: 'change-contains-mips', value }),
    mips: (value) => host.postMessage({ type: 'change-mips', value }),
    cubemap: (value) => host.postMessage({ type: 'change-cubemap', value }),
    tiled: (value) => host.postMessage({ type: 'change-tiled-texture', value }),
    normalize: (value) => host.postMessage({ type: 'change-normalize', value }),
    mipFunction: (value) => host.postMessage({ type: 'change-mipmap-function', value }),
    mipFilter: (value) => host.postMessage({ type: 'change-mipmap-filter', value }),
    swizzling: (value) => host.postMessage({ type: 'change-swizzling', value }),
    conversion: (value) => host.postMessage({ type: 'change-conversion', value }),
    quality: (value) => host.postMessage({ type: 'change-quality', value }),
  }));
  return form;
}


function leaf(source: string): string {
  return source.split(/[\\/]/).at(-1) ?? source;
}

function message(text: string, kind = ''): HTMLElement {
  const result = element('div', `message ${kind}`.trim());
  result.textContent = text;
  return result;
}

function element<K extends keyof HTMLElementTagNameMap>(kind: K, className: string): HTMLElementTagNameMap[K] {
  const result = document.createElement(kind);
  result.className = className;
  return result;
}
