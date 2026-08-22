const fs = require('node:fs');
const path = require('node:path');
const esbuild = require('esbuild');

const production = process.argv.includes('--production');
const watch = process.argv.includes('--watch');
const tests = process.argv.includes('--tests');

/** One line per diagnostic, in the `file:line:column: severity: message` shape the task matcher reads. */
function report(messages, severity) {
  for (const { text, location } of messages) {
    const where = location
      ? `${location.file}:${location.line}:${location.column}`
      : 'esbuild:0:0';
    console.error(`${where}: ${severity}: ${text}`);
  }
}

const reportPlugin = {
  name: 'report',
  setup(build) {
    build.onEnd((result) => {
      report(result.errors, 'error');
      report(result.warnings, 'warning');
    });
  },
};

/** Brackets the build so the background task knows when it starts and settles. */
const problemMatcherPlugin = {
  name: 'problem-matcher',
  setup(build) {
    build.onStart(() => console.log('[watch] build started'));
    build.onEnd(() => console.log('[watch] build finished'));
  },
};

/**
 * The ESM entry of a dependency before its Node one, for the two bundles that run on Node: the UMD
 * entry jsonc-parser ships hides its requires behind the module-system dance, and esbuild leaves
 * them in the bundle for Node to fail on at runtime.
 */
const NODE_MODULE_FIELDS = { mainFields: ['module', 'main'] };

const shared = {
  bundle: true,
  minify: production,
  sourcemap: !production,
  sourcesContent: false,
  logLevel: 'silent',
  plugins: [reportPlugin],
};

/** The extension host bundle: Node, CommonJS, with `vscode` supplied by the host. */
const extension = {
  ...shared,
  entryPoints: ['src/extension.ts'],
  outfile: 'dist/extension.js',
  format: 'cjs',
  platform: 'node',
  target: 'node20',
  ...NODE_MODULE_FIELDS,
  external: ['vscode'],
  plugins: [reportPlugin, problemMatcherPlugin],
};

/**
 * The two browser bundles — the Mods panel and the form over a `.enf` — as ESM, each with its
 * stylesheet coming out beside it under the same name. Bundled apart rather than together: an
 * editor tab loads the form alone, and a panel that is only a list has no business carrying it.
 */
const webview = {
  ...shared,
  entryPoints: { webview: 'src/webview/main.ts', form: 'src/webview/form.ts' },
  outdir: 'dist',
  format: 'esm',
  platform: 'browser',
  target: 'es2022',
};

/** Tests bundle one file each, so `node --test` can run plain CommonJS against the sources. */
function testBuild() {
  const entryPoints = walk('src').filter((file) => file.endsWith('.test.ts'));
  return {
    ...shared,
    entryPoints,
    outdir: 'out/test',
    format: 'cjs',
    platform: 'node',
    target: 'node20',
    ...NODE_MODULE_FIELDS,
    minify: false,
    plugins: [],
    logLevel: 'warning',
  };
}

function walk(directory) {
  return fs.readdirSync(directory, { withFileTypes: true }).flatMap((entry) => {
    const full = path.posix.join(directory, entry.name);
    return entry.isDirectory() ? walk(full) : [full];
  });
}

async function main() {
  if (tests) {
    // Wiped first: a test file that was deleted would otherwise keep running from the last build.
    fs.rmSync('out/test', { recursive: true, force: true });
    await esbuild.build(testBuild());
    return;
  }

  const contexts = await Promise.all([extension, webview].map((build) => esbuild.context(build)));

  if (watch) {
    await Promise.all(contexts.map((context) => context.watch()));
    return;
  }

  await Promise.all(
    contexts.map(async (context) => {
      await context.rebuild();
      await context.dispose();
    }),
  );
}

main().catch((e) => {
  // Build diagnostics are already on stdout in matcher shape; anything else is worth dumping.
  if (!Array.isArray(e?.errors)) {
    console.error(e);
  }
  process.exit(1);
});
