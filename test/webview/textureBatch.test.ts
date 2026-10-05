import assert from 'node:assert/strict';
import { test } from 'node:test';
import { Script } from 'node:vm';
import { buildSync } from 'esbuild';
import { textureBatchPlanOf } from '../../src/mods/texture/textureBatch';
import { openedTextureBatch, updateTextureBatch } from '../../src/mods/texture/textureBatchAuthoring';
import type { TextureBatchStateMessage } from '../../src/webview/texture/textureBatchProtocol';
import { cubeRendering } from '../fixtures/textureRendering';

/** Only the DOM boundary is replaced; the bundled message handler and controls run unchanged. */
class Element {
  children: Element[] = [];
  parent: Element | undefined;
  className = '';
  textContent = '';
  style = {};
  selected = false;
  pixels: Uint8ClampedArray | undefined;
  private selectedValue: string | undefined;
  private readonly listeners = new Map<string, () => void>();

  constructor(readonly tag: string) {}

  get value(): string {
    return this.selectedValue ?? this.children.find((child) => child.selected)?.value ?? this.children[0]?.value ?? '';
  }
  set value(value: string) { this.selectedValue = value; }
  append(...children: Element[]): void {
    for (const child of children) {
      if (child.parent !== undefined) child.parent.children = child.parent.children.filter((other) => other !== child);
      child.parent = this;
      this.children.push(child);
    }
  }
  appendChild(child: Element): Element { this.append(child); return child; }
  replaceChildren(...children: Element[]): void {
    for (const child of this.children) child.parent = undefined;
    this.children = [];
    this.append(...children);
  }
  addEventListener(kind: string, listener: () => void): void { this.listeners.set(kind, listener); }
  change(value: string): void { this.value = value; this.listeners.get('change')?.(); }
  getContext() {
    return { putImageData: (image: { data: Uint8ClampedArray }) => { this.pixels = image.data; } };
  }
  descendants(): Element[] { return [this, ...this.children.flatMap((child) => child.descendants())]; }
}

function browserHarness() {
  const body = new Element('body');
  let receive: (event: { data: TextureBatchStateMessage }) => void = () => { throw new Error('No message listener'); };
  const bundled = buildSync({
    entryPoints: ['src/webview/texture/textureBatch.ts'], bundle: true, write: false,
    platform: 'browser', format: 'iife', loader: { '.css': 'empty' },
  }).outputFiles[0];
  assert.ok(bundled);
  new Script(bundled.text).runInNewContext({
    document: { body, createElement: (tag: string) => new Element(tag) },
    window: { addEventListener: (_kind: string, listener: typeof receive) => { receive = listener; } },
    acquireVsCodeApi: () => ({ postMessage() { /* Host requests are covered by the editor tests. */ } }),
    ImageData: class { constructor(readonly data: Uint8ClampedArray) {} },
    Uint8ClampedArray,
  });
  const face = () => {
    const label = body.descendants().find((element) => element.tag === 'label' && element.textContent === 'Cube face ');
    const select = label?.children.find((element) => element.tag === 'select');
    assert.ok(select, 'the cube face selector must use the separately delivered rendering');
    return select;
  };
  return {
    send: (data: TextureBatchStateMessage) => receive({ data }),
    face,
    pixel: () => body.descendants().find((element) => element.tag === 'canvas')?.pixels?.[0],
    hasCanvas: () => body.descendants().some((element) => element.tag === 'canvas'),
  };
}

test('batch progress preserves the selected cube face using pixels delivered only once', () => {
  const browser = browserHarness();
  const rendering = cubeRendering();
  const facts = { ...rendering,
    source: { ...rendering.source, rgba: new Uint8Array(0), faces: undefined },
    result: { ...rendering.result, rgba: new Uint8Array(0), faces: undefined } };
  const primary = 'C:/mod/Mod/sky.hdr';
  const plan = textureBatchPlanOf({ primary, roots: [{ root: 'C:/mod', prefixRoot: 'C:/mod/Mod' }], occupiedGuids: [],
    items: [{ source: primary, kind: 'file', sourceRevision: { size: 1, modified: 2 },
      metadata: { kind: 'missing' }, newGuid: '0123456789ABCDEF' }] });
  const loaded = updateTextureBatch(openedTextureBatch().state, { kind: 'loaded', plan });
  let state = updateTextureBatch(loaded.state,
    { kind: 'item-rendered', source: primary, revision: 1, rendered: facts }).state;
  browser.send({ type: 'state', state, rendering });
  browser.face().change('2');
  assert.equal(browser.face().value, '2');
  assert.equal(browser.pixel(), 30);

  state = updateTextureBatch(state, { kind: 'run' }).state;
  browser.send({ type: 'state', state });
  state = updateTextureBatch(state,
    { kind: 'native-event', event: { protocolVersion: 1, kind: 'progress', id: '0', progress: 0.5 } }).state;
  browser.send({ type: 'state', state });
  assert.equal(browser.face().value, '2');
  assert.equal(browser.pixel(), 30);
  browser.face().change('5');
  assert.equal(browser.pixel(), 60);
  state = updateTextureBatch(state,
    { kind: 'native-event', event: { protocolVersion: 1, kind: 'complete', converted: 0, failed: 0, cancelled: 1 } }).state;
  browser.send({ type: 'state', state });
  assert.equal(browser.face().value, '5');
  assert.equal(browser.pixel(), 60);

  browser.send({ type: 'state', state, rendering: cubeRendering() });
  assert.equal(browser.face().value, '0');
  assert.equal(browser.pixel(), 10);
  browser.send({ type: 'state', state, rendering: null });
  assert.equal(browser.hasCanvas(), false);
});
