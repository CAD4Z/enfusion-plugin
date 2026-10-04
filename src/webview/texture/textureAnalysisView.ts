/** Shared read-only analysis controls; receives decoded pixels and never sends host work. */
import {
  analysisOf, pixelAt, type AnalysisPixels, type AnalysisSelection,
  type Histogram, type TextureAnalysisInput,
} from '../../mods/texture/textureAnalysis';
import './textureAnalysis.css';

export interface TextureAnalysisView {
  readonly element: HTMLElement;
  bind(viewport: HTMLElement, canvas: HTMLCanvasElement, pixels: AnalysisPixels, label: string): void;
}

export function textureAnalysisView(
  input: TextureAnalysisInput,
  selection: AnalysisSelection,
  selectSide?: (side: AnalysisSelection['side']) => void,
): TextureAnalysisView {
  const analysis = analysisOf(input, selection);
  const section = document.createElement('section');
  section.className = 'texture-analysis';
  section.setAttribute('aria-label', 'Texture analysis');
  section.append(text('h2', 'Analysis'));
  if (selectSide !== undefined) {
    const choices = document.createElement('div');
    choices.className = 'analysis-sides';
    for (const side of ['source', 'result'] as const) {
      const button = text('button', side === 'source' ? 'Source' : 'Result');
      button.type = 'button';
      button.classList.toggle('active', selection.side === side);
      button.setAttribute('aria-pressed', String(selection.side === side));
      button.addEventListener('click', () => selectSide(side));
      choices.append(button);
    }
    section.append(choices);
  }
  const label = selection.side === 'source' ? 'Source' : 'Result';
  const level = analysis.pixels.kind === 'available' ? ` · mip ${analysis.pixels.value.level}` : '';
  section.append(text('h3', `${label}${level} · Histogram`));
  if (analysis.histogram.kind === 'available') {
    section.append(histogramChart(analysis.histogram.value));
  } else {
    section.append(text('p', `Unavailable: ${analysis.histogram.reason}`));
  }
  section.append(text('h3', 'Pixel inspector'));
  const inspector = text('output', analysis.histogram.kind === 'unavailable'
    ? `Unavailable: ${analysis.histogram.reason}` : 'Point inside an image to inspect its actual samples.');
  inspector.className = 'pixel-inspector';
  section.append(inspector, text('h3', 'Result runtime memory'));
  section.append(text('p', analysis.memory.kind === 'available'
    ? `Mip: ${bytes(analysis.memory.value.mipBytes)} · Chain: ${bytes(analysis.memory.value.chainBytes)}`
    : `Unavailable: ${analysis.memory.reason}`));
  section.append(text('p', 'GPU payload, excluding driver alignment. Not EDDS file or container size.'));
  section.append(text('h3', 'Source / Result RMSE'));
  section.append(text('p', analysis.error.kind === 'available'
    ? `${analysis.error.value.rmse.toPrecision(6)} · ${analysis.error.value.units} · ${analysis.error.value.samples.toLocaleString()} samples`
    : `Unavailable: ${analysis.error.reason}`));
  section.append(text('p', 'Root mean square error over selected carried channels; no gamma or alpha weighting.'));

  return {
    element: section,
    bind(viewport, canvas, pixels, paneLabel) {
      let pointer: { readonly x: number; readonly y: number } | undefined;
      const inspect = (): void => {
        if (pointer === undefined) return;
        const sample = pixelAt(pixels, pointer, canvas.getBoundingClientRect());
        inspector.textContent = sample.kind === 'available'
          ? `${paneLabel} · mip ${sample.value.level} · (${sample.value.x}, ${sample.value.y})\n${sample.value.channels}: ${sample.value.values.join(', ')}`
          : sample.reason;
      };
      // The viewport captures the pointer during a drag; listening on the canvas loses those moves.
      viewport.addEventListener('pointermove', (event) => {
        pointer = { x: event.clientX, y: event.clientY };
        inspect();
      });
      viewport.addEventListener('scroll', inspect);
      viewport.addEventListener('pointerleave', () => {
        pointer = undefined;
        inspector.textContent = 'Point inside an image to inspect its actual samples.';
      });
    },
  };
}

function histogramChart(histogram: Histogram): HTMLElement {
  const chart = document.createElement('div');
  chart.className = 'histogram-chart';
  const peak = Math.max(1, ...histogram.series.flatMap(({ bins }) => bins));
  for (const series of histogram.series) {
    const row = document.createElement('div');
    row.className = `histogram-series histogram-${series.channel.toLowerCase()}`;
    const canvas = document.createElement('canvas');
    canvas.width = 256;
    canvas.height = 48;
    canvas.setAttribute('role', 'img');
    canvas.setAttribute('aria-label', `${series.channel} histogram: ${histogram.pixelCount} pixels, range ${histogram.minimum} to ${histogram.maximum}`);
    const context = canvas.getContext('2d');
    if (context !== null) {
      // Canvas does not resolve CSS variables: use the theme foreground of an attached page.
      context.fillStyle = getComputedStyle(document.body).color || '#888';
      for (let bin = 0; bin < series.bins.length; bin++) {
        const height = (series.bins[bin] ?? 0) / peak * 46;
        context.fillRect(bin, 48 - height, 1, height);
      }
    }
    canvas.addEventListener('pointermove', (event) => {
      const rect = canvas.getBoundingClientRect();
      const bin = Math.min(255, Math.max(0, Math.floor((event.clientX - rect.left) * 256 / rect.width)));
      canvas.title = `${series.channel} · bin ${bin}: ${series.bins[bin] ?? 0} pixels`;
    });
    row.append(text('span', series.channel), canvas);
    chart.append(row);
  }
  chart.append(text('p', `${histogram.minimum} … ${histogram.maximum} · ${histogram.pixelCount.toLocaleString()} pixels`));
  return chart;
}

function text<K extends keyof HTMLElementTagNameMap>(tag: K, value: string): HTMLElementTagNameMap[K] {
  const node = document.createElement(tag);
  node.textContent = value;
  return node;
}

function bytes(value: number): string {
  return `${value.toLocaleString()} B`;
}
