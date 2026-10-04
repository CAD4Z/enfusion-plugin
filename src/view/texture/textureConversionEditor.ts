/** Source-image authoring editor: orchestration only; policy lives in the pure state and plan. */

import { randomBytes } from 'node:crypto';
import path from 'node:path';
import * as vscode from 'vscode';
import { literalGlobOf } from '../../mods/paths';
import {
  type TextureAuthoringEffect,
  type TextureAuthoringEvent,
  type TextureAuthoringState,
  openedTextureAuthoring,
  updateTextureAuthoring,
} from '../../mods/texture/textureAuthoring';
import { textureScopeOf } from '../../mods/texture/textureConversion';
import { isTextureSwizzling } from '../../mods/texture/textureSwizzles';
import {
  TEXTURE_SOURCES,
  TEXTURE_SOURCE_EXTENSIONS_EITHER,
  isTextureSourcePath,
} from '../../mods/texture/textureSources';
import {
  commitTextureConversion,
  loadTextureConversion,
  renderTextureDraft,
  rootsOf,
} from '../../platform/texture/textureConversion';
import { EddsConverter } from '../../platform/texture/eddsConverter';
import { findMods, watchMods } from '../../platform/workspace';
import type {
  TextureAuthoringRequest,
  TextureAuthoringStateMessage,
} from '../../webview/texture/textureConversionProtocol';
import { TextureBatchEditor } from './textureBatchEditor';
import {
  isSupportedTextureConversion,
  isTextureQuality,
} from '../../mods/texture/textureConversions';

class TextureSourceDocument implements vscode.CustomDocument {
  constructor(readonly uri: vscode.Uri) {}
  dispose(): void {
    // The source stays unopened between native operations; the document owns no handle.
  }
}

export class TextureConversionEditor
  implements vscode.CustomReadonlyEditorProvider<TextureSourceDocument>
{
  static readonly viewType = 'enfusion.textureConversion';

  constructor(
    private readonly extensionUri: vscode.Uri,
    private readonly storageUri: vscode.Uri,
    private readonly converter: EddsConverter,
    private readonly log: vscode.LogOutputChannel,
  ) {}

  openCustomDocument(uri: vscode.Uri): TextureSourceDocument {
    return new TextureSourceDocument(uri);
  }

  resolveCustomEditor(
    document: TextureSourceDocument,
    panel: vscode.WebviewPanel,
    token: vscode.CancellationToken,
  ): void {
    panel.webview.options = {
      enableScripts: true,
      localResourceRoots: [vscode.Uri.joinPath(this.extensionUri, 'dist')],
    };
    panel.webview.html = this.page(panel.webview);

    const opening = openedTextureAuthoring();
    let current: TextureAuthoringState = opening.state;
    let started = false;
    let disposed = false;
    const work = new Set<AbortController>();
    let draft: AbortController | undefined;

    const send = (): void => {
      if (!disposed) {
        const message: TextureAuthoringStateMessage = {
          type: 'state', state: current.kind === 'authoring' ? { ...current, cachedMips: undefined } : current,
        };
        void panel.webview.postMessage(message);
      }
    };

    const apply = (event: TextureAuthoringEvent): void => {
      const update = updateTextureAuthoring(current, event);
      current = update.state;
      send();
      for (const effect of update.effects) {
        run(effect);
      }
    };

    const run = (effect: TextureAuthoringEffect): void => {
      if (effect.kind === 'render-draft') {
        draft?.abort();
      }
      const controller = new AbortController();
      work.add(controller);
      if (effect.kind === 'render-draft') {
        draft = controller;
      }

      const operation = operationOf(effect, current, document.uri, this.storageUri, this.converter, controller.signal);
      operation
        .then(apply)
        .catch((error: unknown) => {
          if (controller.signal.aborted || disposed) {
            return;
          }
          const reason = error instanceof Error ? error.message : String(error);
          this.log.error(`Texture conversion: ${reason}`);
          apply(failedEvent(effect, reason));
        })
        .finally(() => {
          work.delete(controller);
          if (draft === controller) {
            draft = undefined;
          }
        });
    };

    const watchers = artifactWatchers(document.uri, (artifact) => {
      if (current.kind === 'authoring') {
        apply({ kind: 'artifact-changed', artifact });
      }
    });
    const attached = vscode.Disposable.from(
      watchers,
      panel.webview.onDidReceiveMessage((request: TextureAuthoringRequest) => {
        switch (request.type) {
          case 'ready':
            send();
            if (!started) {
              started = true;
              if (document.uri.scheme !== 'file') {
                apply({ kind: 'load-failed', reason: 'Texture conversion opens local files only.' });
              } else if (process.platform !== 'win32') {
                apply({ kind: 'load-failed', reason: 'Texture conversion is available on Windows x64.' });
              } else {
                for (const effect of opening.effects) run(effect);
              }
            }
            return;
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
          case 'change-swizzling':
            if (isTextureSwizzling(request.value)) {
              apply({ kind: 'change-profile', field: 'Swizzling', value: request.value });
            }
            break;
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
          case 'select-mip':
            if (Number.isInteger(request.mip) && request.mip >= 0) {
              apply({ kind: 'select-mip', mip: request.mip });
            }
            return;
          case 'run':
            apply({ kind: 'run' });
            return;
        }
      }),
      token.onCancellationRequested(() => abortAll(work)),
    );

    panel.onDidDispose(() => {
      disposed = true;
      attached.dispose();
      abortAll(work);
      work.clear();
    });
  }

  private page(webview: vscode.Webview): string {
    const script = this.asset(webview, 'texture-conversion.js');
    const style = this.asset(webview, 'texture-conversion.css');
    const nonce = randomBytes(16).toString('base64');
    return `<!DOCTYPE html>
<html lang="en"><head><meta charset="UTF-8" />
<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src ${webview.cspSource}; script-src 'nonce-${nonce}';" />
<meta name="viewport" content="width=device-width, initial-scale=1.0" />
<link href="${style}" rel="stylesheet" /><title>Texture Conversion</title></head>
<body><script type="module" nonce="${nonce}" src="${script}"></script></body></html>`;
  }

  private asset(webview: vscode.Webview, file: string): string {
    return webview.asWebviewUri(vscode.Uri.joinPath(this.extensionUri, 'dist', file)).toString();
  }
}

export function registerTextureConversionEditor(
  context: vscode.ExtensionContext,
  log: vscode.LogOutputChannel,
  converter = new EddsConverter(context.extensionPath),
): vscode.Disposable {
  const provider = new TextureConversionEditor(
    context.extensionUri,
    context.storageUri ?? context.globalStorageUri,
    converter,
    log,
  );
  const batchEditor = new TextureBatchEditor(
    context.extensionUri,
    context.storageUri ?? context.globalStorageUri,
    converter,
    log,
  );
  const editor = vscode.window.registerCustomEditorProvider(TextureConversionEditor.viewType, provider, {
    supportsMultipleEditorsPerDocument: true,
  });
  const command = vscode.commands.registerCommand(
    'enfusion.texture.convert',
    async (selected?: vscode.Uri, selectedUris?: vscode.Uri[]) => {
      if (process.platform !== 'win32') {
        await vscode.window.showErrorMessage('Texture conversion is available on Windows x64.');
        return;
      }
      const uri = selected ?? (await chooseSource());
      if (uri === undefined) return;
      if (uri.scheme !== 'file' || !isTextureSourcePath(uri.fsPath)) {
        await vscode.window.showErrorMessage(
          `Choose one local ${TEXTURE_SOURCE_EXTENSIONS_EITHER} source image.`,
        );
        return;
      }
      const roots = rootsOf(await findMods());
      if (roots.length === 0) {
        await vscode.window.showErrorMessage(
          'Texture conversion requires a discovered mod.enf or workspace.enf root.',
        );
        return;
      }
      if (textureScopeOf(uri.fsPath, roots) === undefined) {
        await vscode.window.showErrorMessage(
          'The source is outside every discovered Enfusion root in this window.',
        );
        return;
      }
      if (selectedUris !== undefined && selectedUris.length > 1) {
        batchEditor.open(uri, selectedUris);
        return;
      }
      await vscode.commands.executeCommand('vscode.openWith', uri, TextureConversionEditor.viewType);
    },
  );
  const refreshContext = async (): Promise<void> => {
    const available = process.platform === 'win32' && rootsOf(await findMods()).length > 0;
    await vscode.commands.executeCommand('setContext', 'enfusion.hasTextureRoot', available);
  };
  void refreshContext();
  const watcher = watchMods(() => void refreshContext());
  return vscode.Disposable.from(editor, command, watcher);
}

async function operationOf(
  effect: TextureAuthoringEffect,
  current: TextureAuthoringState,
  source: vscode.Uri,
  storage: vscode.Uri,
  converter: EddsConverter,
  signal: AbortSignal,
): Promise<TextureAuthoringEvent> {
  switch (effect.kind) {
    case 'load':
      return { kind: 'loaded', plan: await loadTextureConversion(source, converter) };
    case 'render-draft':
      return {
        kind: 'draft-rendered',
        revision: effect.revision,
        rendered: await renderTextureDraft(effect.plan, storage, converter, effect.mip, signal),
      };
    case 'convert': {
      if (current.kind !== 'running' || current.revision !== effect.revision) {
        throw new Error('The conversion run no longer matches this editor revision.');
      }
      const result = await commitTextureConversion(
        effect.plan,
        current.rendered.source,
        converter,
        signal,
      );
      return { kind: 'converted', revision: effect.revision, ...result };
    }
  }
}

function failedEvent(effect: TextureAuthoringEffect, reason: string): TextureAuthoringEvent {
  switch (effect.kind) {
    case 'load': return { kind: 'load-failed', reason };
    case 'render-draft': return { kind: 'draft-failed', revision: effect.revision, reason };
    case 'convert': return { kind: 'conversion-failed', revision: effect.revision, reason };
  }
}

function artifactWatchers(
  source: vscode.Uri,
  changed: (artifact: 'source' | 'output' | 'metadata') => void,
): vscode.Disposable {
  const output = source.fsPath.slice(0, source.fsPath.lastIndexOf('.')) + '.edds';
  const watched = [
    [source.fsPath, 'source'],
    [output, 'output'],
    [`${output}.meta`, 'metadata'],
  ] as const;
  return vscode.Disposable.from(
    ...watched.map(([file, artifact]) => {
      const watcher = vscode.workspace.createFileSystemWatcher(
        new vscode.RelativePattern(path.dirname(file), literalGlobOf(path.basename(file))),
      );
      return vscode.Disposable.from(
        watcher,
        watcher.onDidChange(() => changed(artifact)),
        watcher.onDidCreate(() => changed(artifact)),
        watcher.onDidDelete(() => changed(artifact)),
      );
    }),
  );
}

function abortAll(work: ReadonlySet<AbortController>): void {
  for (const controller of work) controller.abort();
}

function isCompression(value: unknown): value is 'Copy' | 'Fastest' | 'Medium' | 'Best' {
  return value === 'Copy' || value === 'Fastest' || value === 'Medium' || value === 'Best';
}

async function chooseSource(): Promise<vscode.Uri | undefined> {
  return (
    await vscode.window.showOpenDialog({
      canSelectFiles: true,
      canSelectFolders: false,
      canSelectMany: false,
      filters: { 'Source image': TEXTURE_SOURCES.map((source) => source.extension) },
      title: 'Convert Texture',
    })
  )?.[0];
}
