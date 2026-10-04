import { byteAnalysisApplies, textureSurfaceControls } from './textureSurface';
/** Browser rendering for the standalone EDDS preview. No file or converter access lives here. */

import type { EddsInspection, EddsPreview } from '../../mods/texture/edds';
import type { TextureChannel, TextureEditorState } from '../../mods/texture/textureEditor';
import { textureChannelViewsOf } from '../../mods/texture/textureConversions';
import type { TextureRequest, TextureStateMessage } from './textureProtocol';
import './texture.css';
import { textureAnalysisView, type TextureAnalysisView } from './textureAnalysisView';

declare function acquireVsCodeApi(): { postMessage(message: TextureRequest): void };

const host = acquireVsCodeApi();
const root = document.body.appendChild(element('main', 'texture-editor'));
let state: TextureEditorState | undefined;
let zoom = 1;

window.addEventListener('message', (event: MessageEvent<TextureStateMessage>) => {
  if (event.data.type === 'state') {
    state = event.data.state;
    render(event.data.state);
  }
});

host.postMessage({ type: 'ready' });

function render(next: TextureEditorState): void {
  if (next.kind === 'loading') {
    root.replaceChildren(message('Inspecting the EDDS header and ENF1 payload table…'));
    return;
  }
  if (next.kind === 'failed') {
    root.replaceChildren(message(next.reason, 'error'));
    return;
  }

  const preview = next.preview.kind === 'ready' ? next.preview.preview : undefined;
  const analysis = textureAnalysisView({
    inspection: next.inspection, result: preview, selectedMip: next.selectedMip,
    reason: next.preview.kind === 'failed' || next.preview.kind === 'unsupported-format'
      ? next.preview.reason : 'The selected mip is still decoding.',
  }, { side: 'result', channel: next.channel });
  const content = element('section', 'content');
  content.append(toolbar(next.channel, next.selectedMip, next.inspection, preview !== undefined));
  content.append(viewer(next, preview, analysis));

  const details = element('aside', 'details');
  details.append(
    heading('Texture'),
    facts([
      ['Dimensions', `${next.inspection.width} × ${next.inspection.height}`],
      ['Pixel format', next.inspection.pixelFormat],
      ['Channels', next.inspection.channels],
      ['Mip levels', String(next.inspection.mips.length)],
      ['Access', 'Read-only'],
    ]),
    analysis.element,
    heading('DDS header'),
    facts(ddsFacts(next.inspection)),
    heading('ENF1 mip table'),
    mipTable(next.inspection),
    reconversion(next.reconvert),
  );

  root.replaceChildren(content, details);
}

function toolbar(
  channel: TextureChannel,
  selectedMip: number,
  inspection: EddsInspection,
  hasPixels: boolean,
): HTMLElement {
  const bar = element('div', 'toolbar');
  const mip = document.createElement('select');
  mip.ariaLabel = 'Mip level';
  for (const item of inspection.mips) {
    const option = document.createElement('option');
    option.value = String(item.level);
    option.selected = item.level === selectedMip;
    option.textContent = `Mip ${item.level} · ${item.width} × ${item.height}`;
    mip.append(option);
  }
  mip.addEventListener('change', () => {
    host.postMessage({ type: 'select-mip', mip: Number(mip.value) });
  });

  const channels = element('div', 'channels');
  for (const { view, label } of textureChannelViewsOf(inspection.channels)) {
    const button = document.createElement('button');
    button.type = 'button';
    button.textContent = label;
    button.disabled = !hasPixels;
    button.classList.toggle('active', view === channel);
    button.addEventListener('click', () => {
      host.postMessage({ type: 'select-channel', channel: view });
    });
    channels.append(button);
  }

  const zooms = element('div', 'zooms');
  for (const [value, label] of [
    [0.25, '25%'],
    [0.5, '50%'],
    [1, '100%'],
    [2, '200%'],
    [4, '400%'],
  ] as const) {
    const button = document.createElement('button');
    button.type = 'button';
    button.textContent = label;
    button.classList.toggle('active', value === zoom);
    button.addEventListener('click', () => {
      zoom = value;
      if (state !== undefined) {
        render(state);
      }
    });
    zooms.append(button);
  }

  bar.append(labelled('Mip', mip), labelled('Channels', channels), labelled('Zoom', zooms));
  return bar;
}

function viewer(current: Extract<TextureEditorState, { kind: 'inspect-only' }>, decoded: EddsPreview | undefined, analysis: TextureAnalysisView): HTMLElement {
  const viewport = element('div', 'viewport checkerboard');
  draggable(viewport);
  if (decoded !== undefined) {
    const canvas = document.createElement('canvas');
    canvas.width = decoded.width;
    canvas.height = decoded.height;
    canvas.style.width = `${decoded.width * zoom}px`;
    canvas.style.height = `${decoded.height * zoom}px`;
    canvas.title = 'Drag to pan';
    draw(canvas, decoded, current.channel);
    viewport.append(textureSurfaceControls(decoded, (surface) => draw(canvas, surface, current.channel)));
    viewport.append(canvas);
    if (byteAnalysisApplies(decoded)) analysis.bind(viewport, canvas, { ...decoded, channels: current.inspection.channels }, 'Result');
    return viewport;
  }

  switch (current.preview.kind) {
    case 'loading':
      viewport.append(message(`Decoding mip ${current.preview.mip}…`));
      break;
    case 'unsupported-format':
      viewport.append(message(current.preview.reason, 'unsupported'));
      break;
    case 'failed':
      viewport.append(message(current.preview.reason, 'error'));
      break;
    case 'ready':
      break;
  }
  return viewport;
}

function draw(canvas: HTMLCanvasElement, preview: EddsPreview, channel: TextureChannel): void {
  const context = canvas.getContext('2d');
  if (context === null) {
    return;
  }
  const source = preview.rgba;
  const shown = new Uint8ClampedArray(source.length);
  for (let at = 0; at < source.length; at += 4) {
    if (channel === 'rgba') {
      shown[at] = source[at] ?? 0;
      shown[at + 1] = source[at + 1] ?? 0;
      shown[at + 2] = source[at + 2] ?? 0;
      shown[at + 3] = source[at + 3] ?? 0;
    } else {
      const component = channelIndex(channel);
      const value = source[at + component] ?? 0;
      shown[at] = value;
      shown[at + 1] = value;
      shown[at + 2] = value;
      shown[at + 3] = 255;
    }
  }
  context.putImageData(new ImageData(shown, preview.width, preview.height), 0, 0);
}

function channelIndex(channel: Exclude<TextureChannel, 'rgba'>): number {
  switch (channel) {
    case 'red': return 0;
    case 'green': return 1;
    case 'blue': return 2;
    case 'alpha': return 3;
  }
}

function draggable(viewport: HTMLElement): void {
  let origin: { x: number; y: number; left: number; top: number } | undefined;
  viewport.addEventListener('pointerdown', (event) => {
    origin = { x: event.clientX, y: event.clientY, left: viewport.scrollLeft, top: viewport.scrollTop };
    viewport.setPointerCapture(event.pointerId);
    viewport.classList.add('dragging');
  });
  viewport.addEventListener('pointermove', (event) => {
    if (origin !== undefined) {
      viewport.scrollLeft = origin.left - (event.clientX - origin.x);
      viewport.scrollTop = origin.top - (event.clientY - origin.y);
    }
  });
  const release = (event: PointerEvent): void => {
    if (viewport.hasPointerCapture(event.pointerId)) {
      viewport.releasePointerCapture(event.pointerId);
    }
    origin = undefined;
    viewport.classList.remove('dragging');
  };
  viewport.addEventListener('pointerup', release);
  viewport.addEventListener('pointercancel', release);
}

function ddsFacts(inspection: EddsInspection): readonly (readonly [string, string])[] {
  const dds = inspection.dds;
  return [
    ['Flags', hex(dds.flags)],
    ['Pitch / linear size', String(dds.pitchOrLinearSize)],
    ['Depth', String(dds.depth)],
    ['Pixel flags', hex(dds.pixelFormatFlags)],
    ['FourCC', dds.fourCC],
    ['RGB bits', String(dds.rgbBitCount)],
    ['R mask', hex(dds.rMask)],
    ['G mask', hex(dds.gMask)],
    ['B mask', hex(dds.bMask)],
    ['A mask', hex(dds.aMask)],
    ['Caps', hex(dds.caps)],
    ['Caps2', hex(dds.caps2)],
    ['DXGI format', String(dds.dxgiFormat)],
    ['Resource dimension', String(dds.resourceDimension)],
    ['Array size', String(dds.arraySize)],
    ['Misc flag', hex(dds.miscFlag)],
  ];
}

function mipTable(inspection: EddsInspection): HTMLElement {
  const table = document.createElement('table');
  const head = document.createElement('thead');
  const headRow = document.createElement('tr');
  for (const title of ['Mip', 'Size', 'Container', 'Stored', 'Decoded']) {
    headRow.append(cell('th', title));
  }
  head.append(headRow);
  const body = document.createElement('tbody');
  for (const mip of inspection.mips) {
    const row = document.createElement('tr');
    row.append(
      cell('td', String(mip.level)),
      cell('td', `${mip.width} × ${mip.height}`),
      cell('td', mip.container),
      cell('td', bytes(mip.storedBytes)),
      cell('td', bytes(mip.decodedBytes)),
    );
    body.append(row);
  }
  table.append(head, body);
  return table;
}

function reconversion(reconvert: Extract<TextureEditorState, { kind: 'inspect-only' }>['reconvert']): HTMLElement {
  const block = element('section', 'refusal');
  const button = document.createElement('button');
  button.type = 'button';
  button.textContent = 'Reconvert';
  button.disabled = reconvert.kind !== 'available';
  button.title = reconvert.reason ?? 'Open the validated source and preserved texture profile.';
  button.addEventListener('click', () => host.postMessage({ type: 'reconvert' }));
  block.append(
    button,
    paragraph(
      reconvert.reason ?? 'This EDDS has a validated metadata relationship to its source image.',
    ),
  );
  return block;
}

function facts(items: readonly (readonly [string, string])[]): HTMLElement {
  const list = document.createElement('dl');
  for (const [name, value] of items) {
    const term = document.createElement('dt');
    const description = document.createElement('dd');
    term.textContent = name;
    description.textContent = value;
    list.append(term, description);
  }
  return list;
}

/**
 * A caption over its control. A group, not a `<label>`: a label hands its clicks to the first
 * button inside it, so a click on "Zoom" would have been a click on 25%.
 */
function labelled(name: string, control: HTMLElement): HTMLElement {
  const wrapper = element('div', 'control');
  wrapper.setAttribute('role', 'group');
  wrapper.setAttribute('aria-label', name);
  wrapper.append(span(name), control);
  return wrapper;
}

function message(text: string, kind = ''): HTMLElement {
  const block = element('div', `message ${kind}`.trim());
  block.textContent = text;
  return block;
}

function heading(text: string): HTMLElement {
  const result = document.createElement('h2');
  result.textContent = text;
  return result;
}

function paragraph(text: string): HTMLElement {
  const result = document.createElement('p');
  result.textContent = text;
  return result;
}

function span(text: string): HTMLElement {
  const result = document.createElement('span');
  result.textContent = text;
  return result;
}

function cell(kind: 'th' | 'td', text: string): HTMLTableCellElement {
  const result = document.createElement(kind);
  result.textContent = text;
  return result;
}

function element<K extends keyof HTMLElementTagNameMap>(kind: K, className: string): HTMLElementTagNameMap[K] {
  const result = document.createElement(kind);
  result.className = className;
  return result;
}

function hex(value: number): string {
  return `0x${value.toString(16).padStart(8, '0')}`;
}

function bytes(value: number): string {
  return new Intl.NumberFormat().format(value);
}
