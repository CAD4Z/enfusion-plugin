/** Browser-only rendering for source-image authoring. Every write remains an explicit host request. */

import type { EddsPreview } from '../mods/edds';
import { TEXTURE_PROFILE_FIELDS, type TextureAuthoringState } from '../mods/textureAuthoring';
import type { TextureProfile } from '../mods/textureConversion';
import type {
  TextureAuthoringRequest,
  TextureAuthoringStateMessage,
} from './textureConversionProtocol';
import './textureConversion.css';

declare function acquireVsCodeApi(): { postMessage(message: TextureAuthoringRequest): void };

const host = acquireVsCodeApi();
const root = document.body.appendChild(element('main', 'conversion-editor'));
let state: TextureAuthoringState | undefined;
let channel: 'rgba' | 'red' | 'green' | 'blue' | 'alpha' = 'rgba';
let zoom = 1;

window.addEventListener('message', (event: MessageEvent<TextureAuthoringStateMessage>) => {
  if (event.data.type === 'state') {
    state = event.data.state;
    render(event.data.state);
  }
});
host.postMessage({ type: 'ready' });

function render(next: TextureAuthoringState): void {
  if (next.kind === 'loading') {
    root.replaceChildren(message('Loading source, output ownership, and metadata…'));
    return;
  }
  if (next.kind === 'refused') {
    root.replaceChildren(message(next.reason, 'error'));
    return;
  }

  const rendered =
    next.kind === 'authoring'
      ? next.preview.kind === 'ready'
        ? next.preview.rendered
        : undefined
      : next.rendered;
  const profile = next.kind === 'authoring' ? next.draft : next.profile;
  const plan = next.plan;
  const locked = next.kind === 'running' || next.kind === 'result';
  const body = element('section', 'workbench');
  body.append(toolbar(next, rendered?.inspection.mips ?? []));

  if (rendered === undefined) {
    body.append(
      message(
        next.kind === 'authoring' && next.preview.kind === 'failed'
          ? next.preview.reason
          : 'Rendering a temporary native preview…',
        next.kind === 'authoring' && next.preview.kind === 'failed' ? 'error' : '',
      ),
    );
  } else {
    const comparisons = element('div', 'comparisons');
    comparisons.append(
      previewPane('Source', rendered.source),
      previewPane(`Result · mip ${rendered.result.level}`, rendered.result),
    );
    body.append(comparisons);
  }

  const properties = element('aside', 'properties');
  properties.append(heading('Texture profile'), profileForm(profile, locked));
  const action = document.createElement('button');
  action.type = 'button';
  action.className = 'primary-action';
  action.textContent = next.kind === 'running' ? 'Converting…' : next.kind === 'result' ? 'Converted' : plan.label;
  action.disabled = next.kind !== 'authoring' || next.preview.kind !== 'ready';
  action.addEventListener('click', () => host.postMessage({ type: 'run' }));
  properties.append(
    heading('Destination'),
    facts([
      ['Action', plan.label],
      ['Scope', plan.scope],
      ['Channels', rendered?.inspection.channels ?? 'Loading'],
      ['EDDS', plan.output],
      ['Metadata', plan.metadata ?? 'Skipped'],
      ['GUID', plan.identity?.guid ?? 'Not registered'],
    ]),
    ...(plan.notice === undefined ? [] : [message(plan.notice, 'notice')]),
    action,
  );
  if (next.kind === 'result') {
    properties.append(
      message(
        `${next.conversion.width} × ${next.conversion.height}, ${next.conversion.mipCount} mip level(s), ${next.conversion.pixelFormat}.`,
        'success',
      ),
    );
  }
  root.replaceChildren(body, properties);
}

function toolbar(
  current: Exclude<TextureAuthoringState, { kind: 'loading' | 'refused' }>,
  mips: readonly { readonly level: number; readonly width: number; readonly height: number }[],
): HTMLElement {
  const bar = element('div', 'toolbar');
  const mip = document.createElement('select');
  mip.ariaLabel = 'Mip level';
  const selected = current.kind === 'authoring' ? current.selectedMip : current.rendered.result.level;
  for (const item of mips) {
    const option = document.createElement('option');
    option.value = String(item.level);
    option.selected = item.level === selected;
    option.textContent = `Mip ${item.level} · ${item.width} × ${item.height}`;
    mip.append(option);
  }
  mip.disabled = current.kind !== 'authoring' || current.preview.kind !== 'ready';
  mip.addEventListener('change', () => host.postMessage({ type: 'select-mip', mip: Number(mip.value) }));

  const channels = element('div', 'button-row');
  for (const [value, title] of [
    ['rgba', 'RGBA'], ['red', 'R'], ['green', 'G'], ['blue', 'B'], ['alpha', 'A'],
  ] as const) {
    const button = buttonOf(title, () => {
      channel = value;
      if (state !== undefined) render(state);
    });
    button.classList.toggle('active', value === channel);
    channels.append(button);
  }
  const zooms = element('div', 'button-row');
  for (const value of [0.25, 0.5, 1, 2, 4]) {
    const button = buttonOf(`${value * 100}%`, () => {
      zoom = value;
      if (state !== undefined) render(state);
    });
    button.classList.toggle('active', value === zoom);
    zooms.append(button);
  }
  bar.append(labelled('Mip', mip), labelled('Channels', channels), labelled('Zoom', zooms));
  return bar;
}

function profileForm(profile: TextureProfile, locked: boolean): HTMLElement {
  const form = element('div', 'profile-form');
  for (const field of TEXTURE_PROFILE_FIELDS) {
    let control: HTMLInputElement | HTMLSelectElement;
    if (field.key === 'FormatCompress') {
      const select = document.createElement('select');
      for (const value of ['Copy', 'Fastest', 'Medium', 'Best'] as const) {
        const option = document.createElement('option');
        option.value = value;
        option.textContent = value;
        option.selected = value === profile.FormatCompress;
        select.append(option);
      }
      select.addEventListener('change', () => {
        const value = select.value;
        if (value === 'Copy' || value === 'Fastest' || value === 'Medium' || value === 'Best') {
          host.postMessage({ type: 'change-compression', value });
        }
      });
      control = select;
    } else {
      const input = document.createElement('input');
      if (field.key === 'GenerateMips' || field.key === 'TiledTexture') {
        input.type = 'checkbox';
        input.checked = Boolean(profile[field.key]);
        if (field.key === 'GenerateMips') {
          input.addEventListener('change', () => host.postMessage({ type: 'change-mips', value: input.checked }));
        }
      } else if (field.key === 'CompressTreshold') {
        input.type = 'number';
        input.min = '0';
        input.max = '100';
        input.value = String(profile.CompressTreshold);
        input.addEventListener('change', () => host.postMessage({ type: 'change-threshold', value: Number(input.value) }));
      } else {
        input.type = 'text';
        input.value = String(profile[field.key]);
      }
      control = input;
    }
    control.disabled = locked || !field.editable;
    control.title = locked
      ? `${field.key}: properties are locked while or after this immutable run.`
      : `${field.key}${field.reason === undefined ? '' : `: ${field.reason}`}`;
    form.append(labelled(field.key, control));
  }
  return form;
}

function previewPane(title: string, preview: EddsPreview): HTMLElement {
  const pane = element('section', 'preview-pane');
  pane.append(heading(title));
  const viewport = element('div', 'viewport checkerboard');
  draggable(viewport);
  const canvas = document.createElement('canvas');
  canvas.width = preview.width;
  canvas.height = preview.height;
  canvas.style.width = `${preview.width * zoom}px`;
  canvas.style.height = `${preview.height * zoom}px`;
  draw(canvas, preview);
  viewport.append(canvas);
  pane.append(viewport);
  return pane;
}

function draw(canvas: HTMLCanvasElement, preview: EddsPreview): void {
  const context = canvas.getContext('2d');
  if (context === null) return;
  const shown = new Uint8ClampedArray(preview.rgba.length);
  for (let at = 0; at < preview.rgba.length; at += 4) {
    if (channel === 'rgba') {
      shown.set(preview.rgba.subarray(at, at + 4), at);
    } else {
      const component = channel === 'red' ? 0 : channel === 'green' ? 1 : channel === 'blue' ? 2 : 3;
      const value = preview.rgba[at + component] ?? 0;
      shown.set([value, value, value, 255], at);
    }
  }
  context.putImageData(new ImageData(shown, preview.width, preview.height), 0, 0);
}

function draggable(viewport: HTMLElement): void {
  let origin: { x: number; y: number; left: number; top: number } | undefined;
  viewport.addEventListener('pointerdown', (event) => {
    origin = { x: event.clientX, y: event.clientY, left: viewport.scrollLeft, top: viewport.scrollTop };
    viewport.setPointerCapture(event.pointerId);
  });
  viewport.addEventListener('pointermove', (event) => {
    if (origin !== undefined) {
      viewport.scrollLeft = origin.left - event.clientX + origin.x;
      viewport.scrollTop = origin.top - event.clientY + origin.y;
    }
  });
  const release = (): void => { origin = undefined; };
  viewport.addEventListener('pointerup', release);
  viewport.addEventListener('pointercancel', release);
}

function facts(rows: readonly (readonly [string, string])[]): HTMLElement {
  const list = document.createElement('dl');
  for (const [key, value] of rows) {
    const term = document.createElement('dt');
    const description = document.createElement('dd');
    term.textContent = key;
    description.textContent = value;
    list.append(term, description);
  }
  return list;
}

function labelled(name: string, control: HTMLElement): HTMLLabelElement {
  const label = document.createElement('label');
  label.className = 'control';
  const caption = document.createElement('span');
  caption.textContent = name;
  label.append(caption, control);
  return label;
}

function buttonOf(text: string, action: () => void): HTMLButtonElement {
  const button = document.createElement('button');
  button.type = 'button';
  button.textContent = text;
  button.addEventListener('click', action);
  return button;
}

function message(text: string, kind = ''): HTMLElement {
  const result = element('div', `message ${kind}`.trim());
  result.textContent = text;
  return result;
}

function heading(text: string): HTMLHeadingElement {
  const result = document.createElement('h2');
  result.textContent = text;
  return result;
}

function element<K extends keyof HTMLElementTagNameMap>(kind: K, className: string): HTMLElementTagNameMap[K] {
  const result = document.createElement(kind);
  result.className = className;
  return result;
}
