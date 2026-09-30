/** Explicit Explorer multi-selection authoring: one captured batch and one native process. */

import { randomBytes } from 'node:crypto';
import * as vscode from 'vscode';
import {
  type TextureBatchAuthoringEffect,
  type TextureBatchAuthoringEvent,
  type TextureBatchAuthoringState,
  openedTextureBatch,
  updateTextureBatch,
} from '../mods/textureBatchAuthoring';
import { loadTextureBatch, renderTextureDraft } from '../platform/textureConversion';
import { batchFailureRetryable } from '../platform/eddsBatch';
import type { EddsConverter } from '../platform/eddsConverter';
import {
  isSupportedTextureConversion,
  isTextureQuality,
} from '../mods/textureConversions';
import type {
  TextureBatchRequest,
  TextureBatchStateMessage,
} from '../webview/textureBatchProtocol';

export class TextureBatchEditor {
  constructor(
    private readonly extensionUri: vscode.Uri,
    private readonly storageUri: vscode.Uri,
    private readonly converter: EddsConverter,
    private readonly log: vscode.LogOutputChannel,
  ) {}

  open(primary: vscode.Uri, selected: readonly vscode.Uri[]): void {
    const count = [primary, ...selected].filter(
      (uri, at, all) => all.findIndex((candidate) => candidate.toString() === uri.toString()) === at,
    ).length;
    const panel = vscode.window.createWebviewPanel(
      'enfusion.textureBatchConversion',
      `Convert ${count} textures`,
      vscode.ViewColumn.Active,
      {
        enableScripts: true,
        localResourceRoots: [vscode.Uri.joinPath(this.extensionUri, 'dist')],
      },
    );
    panel.webview.html = this.page(panel.webview);
    const opening = openedTextureBatch();
    let current: TextureBatchAuthoringState = opening.state;
    let started = false;
    let disposed = false;
    let preview: AbortController | undefined;
    let batch: AbortController | undefined;

    const send = (): void => {
      if (!disposed) {
        const message: TextureBatchStateMessage = { type: 'state', state: current };
        void panel.webview.postMessage(message);
      }
    };
    const apply = (event: TextureBatchAuthoringEvent): void => {
      const update = updateTextureBatch(current, event);
      current = update.state;
      send();
      for (const effect of update.effects) run(effect);
    };
    const run = (effect: TextureBatchAuthoringEffect): void => {
      switch (effect.kind) {
        case 'load':
          void loadTextureBatch(primary, selected, this.converter).then(
            (plan) => apply({ kind: 'loaded', plan }),
            (error: unknown) => apply({ kind: 'load-failed', reason: reasonOf(error) }),
          );
          return;
        case 'render-item': {
          preview?.abort();
          const controller = new AbortController();
          preview = controller;
          void renderTextureDraft(effect.plan, this.storageUri, this.converter, 0, controller.signal).then(
            (rendered) => apply({
              kind: 'item-rendered',
              source: effect.source,
              revision: effect.revision,
              rendered,
            }),
            (error: unknown) => {
              if (!controller.signal.aborted) apply({
                kind: 'item-render-failed',
                source: effect.source,
                revision: effect.revision,
                reason: reasonOf(error),
              });
            },
          );
          return;
        }
        case 'convert-batch': {
          preview?.abort();
          const controller = new AbortController();
          batch = controller;
          void this.converter.batch(effect.jobs, (event) => apply({ kind: 'native-event', event }), controller.signal).catch(
            (error: unknown) => {
              this.log.error(`Texture batch conversion: ${reasonOf(error)}`);
              apply({
                kind: 'batch-failed',
                reason: reasonOf(error),
                retryable: batchFailureRetryable(error),
              });
            },
          ).finally(() => {
            if (batch === controller) batch = undefined;
          });
          return;
        }
        case 'refresh-retry':
          void loadTextureBatch(primary, selected, this.converter).then(
            (plan) => apply({ kind: 'retry-refreshed', plan }),
            (error: unknown) => apply({ kind: 'retry-refresh-failed', reason: reasonOf(error) }),
          );
          return;
        case 'cancel-batch':
          batch?.abort();
          return;
      }
    };
    const attached = panel.webview.onDidReceiveMessage((request: TextureBatchRequest) => {
      switch (request.type) {
        case 'ready':
          send();
          if (!started) {
            started = true;
            for (const effect of opening.effects) run(effect);
          }
          return;
        case 'select-item': apply({ kind: 'select-item', source: request.source }); return;
        case 'change-compression':
          if (isCompression(request.value)) {
            apply({ kind: 'change-profile', field: 'FormatCompress', value: request.value });
          }
          return;
        case 'change-threshold':
          if (Number.isInteger(request.value) && request.value >= 0 && request.value <= 100) {
            apply({ kind: 'change-profile', field: 'CompressTreshold', value: request.value });
          }
          return;
        case 'change-remove-mips':
          if (Number.isInteger(request.value) && request.value >= 0 && request.value <= 14) {
            apply({ kind: 'change-profile', field: 'RemoveMips', value: request.value });
          }
          return;
        case 'change-contains-mips':
          if (typeof request.value === 'boolean') {
            apply({ kind: 'change-profile', field: 'ContainsMips', value: request.value });
          }
          return;
        case 'change-mips':
          if (typeof request.value === 'boolean') {
            apply({ kind: 'change-profile', field: 'GenerateMips', value: request.value });
          }
          return;
        case 'change-tiled-texture':
          if (typeof request.value === 'boolean') {
            apply({ kind: 'change-profile', field: 'TiledTexture', value: request.value });
          }
          return;
        case 'change-normalize':
          if (typeof request.value === 'boolean') {
            apply({ kind: 'change-profile', field: 'Normalize', value: request.value });
          }
          return;
        case 'change-mipmap-function':
          if (request.value === 'Filter' || request.value === 'Normalize' || request.value === 'ColorNoise') {
            apply({ kind: 'change-profile', field: 'MipMapFunction', value: request.value });
          }
          return;
        case 'change-mipmap-filter':
          if (request.value === 'Box' || request.value === 'Kaiser') {
            apply({ kind: 'change-profile', field: 'MipMapFilter', value: request.value });
          }
          return;
        case 'change-conversion':
          if (isSupportedTextureConversion(request.value)) {
            apply({ kind: 'change-profile', field: 'Conversion', value: request.value });
          }
          return;
        case 'change-quality':
          if (isTextureQuality(request.value)) {
            apply({ kind: 'change-profile', field: 'ConversionQuality', value: request.value });
          }
          return;
        case 'run': apply({ kind: 'run' }); return;
        case 'cancel': apply({ kind: 'cancel' }); return;
        case 'retry-failed': apply({ kind: 'retry-failed' }); return;
      }
    });
    panel.onDidDispose(() => {
      disposed = true;
      attached.dispose();
      preview?.abort();
      batch?.abort();
    });
  }

  private page(webview: vscode.Webview): string {
    const nonce = randomBytes(16).toString('base64');
    const script = webview.asWebviewUri(vscode.Uri.joinPath(this.extensionUri, 'dist', 'texture-batch.js'));
    const style = webview.asWebviewUri(vscode.Uri.joinPath(this.extensionUri, 'dist', 'texture-batch.css'));
    return `<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8" />
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src ${webview.cspSource}; script-src 'nonce-${nonce}';" />
<meta name="viewport" content="width=device-width, initial-scale=1.0" />
<link href="${style.toString()}" rel="stylesheet" /><title>Texture Batch Conversion</title></head>
<body><script type="module" nonce="${nonce}" src="${script.toString()}"></script></body></html>`;
  }
}

function reasonOf(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

function isCompression(value: unknown): value is 'Copy' | 'Fastest' | 'Medium' | 'Best' {
  return value === 'Copy' || value === 'Fastest' || value === 'Medium' || value === 'Best';
}
