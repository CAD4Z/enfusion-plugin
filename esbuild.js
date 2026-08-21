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
  external: ['vscode'],
  plugins: [reportPlugin, problemMatcherPlugin],
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
    await esbuild.build(testBuild());
    return;
  }

  const context = await esbuild.context(extension);
  if (watch) {
    await context.watch();
    return;
  }
  await context.rebuild();
  await context.dispose();
}

main().catch((e) => {
  // Build diagnostics are already on stdout in matcher shape; anything else is worth dumping.
  if (!Array.isArray(e?.errors)) {
    console.error(e);
  }
  process.exit(1);
});
