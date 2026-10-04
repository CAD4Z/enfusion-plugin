/** Explorer commands for fonts: native Quick Input, a cancellable operation, and its result. */
import path from 'node:path';
import * as vscode from 'vscode';
import {
  defaultFontName, FONT_SIZE_DEFAULT, FONT_SIZE_MAX, FONT_SIZE_MIN, fontGenerationSummaryOf, fontNameProblemOf, type FontRequest,
} from '../mods/font/fontCommands';
import { samePath } from '../mods/paths';
import { assertFontCommandCurrent, assertFontScope, loadFontCommand } from '../platform/font/fontCommands';
import { FontGenerator } from '../platform/font/fontGenerator';

/** Thin glyphs a notification names before it counts the rest. */
const THIN_GLYPHS_SHOWN = 12;

export function registerFontCommands(context: vscode.ExtensionContext, log: vscode.LogOutputChannel): vscode.Disposable {
  const generator = new FontGenerator(context.extensionPath);
  const running = new Map<string, AbortController>();
  const command = (kind: FontRequest['kind']) => async (selected?: vscode.Uri): Promise<void> => {
    let output: string | undefined;
    try {
      const uri = selected ?? (await vscode.window.showOpenDialog({
        canSelectMany: false, canSelectFolders: false,
        filters: kind === 'generate' ? { TrueType: ['ttf'] } : { 'Enfusion Font': ['fnt'] },
        title: kind === 'generate' ? 'Generate Enfusion Font' : 'Regenerate Enfusion Font',
      }))?.[0];
      if (uri === undefined) return;
      await assertFontScope(uri, kind);
      const request = kind === 'regenerate' ? { kind } as const : await chooseFont(uri, generator);
      if (request === undefined) return;
      const session = await loadFontCommand(uri, request);
      const key = samePath(session.plan.output);
      if (running.has(key)) throw new Error('This font is already being generated.');
      const controller = new AbortController();
      running.set(key, controller);
      output = key;
      if (session.plan.action === 'replace') {
        const replace = await vscode.window.showWarningMessage(
          `Replace ${path.basename(session.plan.output)} and its font files?`,
          { modal: true, detail: `${session.plan.replace.map((file) => path.basename(file)).join('\n')}\n\nThe existing font GUID will be preserved if its recipe has one.` },
          'Replace',
        );
        if (replace !== 'Replace') return;
      }
      const result = await vscode.window.withProgress({
        location: vscode.ProgressLocation.Notification,
        title: `${kind === 'generate' ? 'Generating' : 'Regenerating'} ${path.basename(session.plan.output)}`,
        cancellable: true,
      }, async (progress, token) => {
        const cancellation = token.onCancellationRequested(() => controller.abort());
        if (token.isCancellationRequested) controller.abort();
        try {
          await assertFontCommandCurrent(session);
          controller.signal.throwIfAborted();
          progress.report({ message: 'Building the font and atlas…' });
          return await generator.generate(session, controller.signal);
        } finally {
          cancellation.dispose();
        }
      });
      // The log names every thin glyph; the notification only the first few.
      const file = path.basename(session.plan.output);
      const outcome = { ...result, notice: session.plan.notice };
      const summary = fontGenerationSummaryOf(file, outcome, THIN_GLYPHS_SHOWN);
      log.info(fontGenerationSummaryOf(file, outcome).message);
      if (summary.warning) void vscode.window.showWarningMessage(summary.message);
      else void vscode.window.showInformationMessage(summary.message);
    } catch (error: unknown) {
      if (error instanceof Error && error.name === 'AbortError') {
        void vscode.window.showInformationMessage('Font generation cancelled.');
      } else {
        const reason = error instanceof Error ? error.message : String(error);
        log.error(`Font generation: ${reason}`);
        void vscode.window.showErrorMessage(reason);
      }
    } finally {
      if (output !== undefined) running.delete(output);
    }
  };
  return vscode.Disposable.from(
    vscode.commands.registerCommand('enfusion.font.generate', command('generate')),
    vscode.commands.registerCommand('enfusion.font.regenerate', command('regenerate')),
    new vscode.Disposable(() => { for (const controller of running.values()) controller.abort(); }),
  );
}

async function chooseFont(uri: vscode.Uri, generator: FontGenerator): Promise<FontRequest | undefined> {
  const source = await generator.inspect(uri.fsPath);
  const sizes = [FONT_SIZE_DEFAULT, ...Array.from({ length: FONT_SIZE_MAX - FONT_SIZE_MIN + 1 }, (_, at) => at + FONT_SIZE_MIN)
    .filter((size) => size !== FONT_SIZE_DEFAULT)];
  const size = await vscode.window.showQuickPick(sizes.map((size) => ({
    label: String(size), description: size === FONT_SIZE_DEFAULT ? 'Default' : undefined, size,
  })), {
    title: 'Generate Enfusion Font · 1/3', placeHolder: `Atlas font size (${FONT_SIZE_MIN}–${FONT_SIZE_MAX} px)`, ignoreFocusOut: true,
  });
  if (size === undefined) return undefined;
  const folder = vscode.Uri.file(path.dirname(uri.fsPath));
  const textFiles = (await vscode.workspace.fs.readDirectory(folder))
    .filter(([name, type]) => type === vscode.FileType.File && /\.txt$/i.test(name))
    .map(([name]) => name).sort((a, b) => a.localeCompare(b));
  const characters = await vscode.window.showQuickPick([
    { label: 'Built-in character set', description: 'Basic Latin, Latin-1 and Cyrillic U+0400–U+045F', file: undefined },
    ...textFiles.map((name) => ({ label: name, description: 'Character file beside the source font', file: vscode.Uri.joinPath(folder, name).fsPath })),
  ], { title: 'Generate Enfusion Font · 2/3', placeHolder: 'Characters to include', ignoreFocusOut: true });
  if (characters === undefined) return undefined;
  const name = await vscode.window.showInputBox({
    title: 'Generate Enfusion Font · 3/3', prompt: 'Font filename without extension',
    value: defaultFontName(source.family, source.style, size.size), validateInput: fontNameProblemOf, ignoreFocusOut: true,
  });
  return name === undefined ? undefined : { kind: 'generate', size: size.size, characters: characters.file, name };
}
