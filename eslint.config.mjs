/**
 * Lint rules for the extension bundle and the build script.
 *
 * Type-checked linting against `tsconfig.json`, with `esbuild.js` and this file falling back to the
 * default project. Anything `tsc` already rejects under `strict` (unused locals and parameters,
 * missing returns, fallthrough) is left to `npm run check-types` rather than said twice.
 *
 * The one rule that is this project's own is `no-restricted-imports`: it keeps `vscode` out of
 * `src/mods/`, which is what makes `npm test` run without an extension host.
 */

import js from '@eslint/js';
import tseslint from 'typescript-eslint';

/** The layer that must compile without an extension host. */
const HOST_FREE = ['src/mods/**'];

/** The host modules, named so the domain cannot reach the disk behind the port's back. */
const HOST_MODULES = ['**/platform/*', '**/view/*'];

export default tseslint.config(
  { ignores: ['dist/**', 'out/**', 'node_modules/**', '**/*.d.ts'] },

  js.configs.recommended,
  tseslint.configs.recommendedTypeChecked,
  tseslint.configs.stylisticTypeChecked,

  {
    name: 'enfusion/parser',
    languageOptions: {
      parserOptions: {
        projectService: { allowDefaultProject: ['esbuild.js', 'eslint.config.mjs'] },
        tsconfigRootDir: import.meta.dirname,
      },
    },
  },

  {
    name: 'enfusion/rules',
    rules: {
      // `tsc` already reports these, and a second voice saying the same thing helps nobody.
      '@typescript-eslint/no-unused-vars': 'off',

      // A `void` on a floating promise is the deliberate form; anything else is a missing `await`.
      '@typescript-eslint/no-floating-promises': ['error', { ignoreVoid: true }],
      '@typescript-eslint/no-misused-promises': ['error', { checksVoidReturn: { arguments: false } }],

      // The domain leans on `undefined` throughout, and `== null` would quietly widen it to both.
      eqeqeq: ['error', 'always'],
      'no-console': 'error',
      'object-shorthand': 'error',
      'prefer-const': 'error',
      'no-param-reassign': 'error',

      '@typescript-eslint/consistent-type-imports': [
        'error',
        { prefer: 'type-imports', fixStyle: 'inline-type-imports' },
      ],
      '@typescript-eslint/switch-exhaustiveness-check': 'error',
    },
  },

  {
    name: 'enfusion/host-free-layers',
    files: HOST_FREE,
    rules: {
      'no-restricted-imports': [
        'error',
        {
          paths: [
            {
              name: 'vscode',
              message: 'The domain reaches the host through a port, so that it stays testable on plain Node.',
            },
          ],
          patterns: [
            {
              group: HOST_MODULES,
              message: 'The domain never imports the host layers.',
            },
          ],
        },
      ],
    },
  },

  {
    name: 'enfusion/build-script',
    files: ['esbuild.js', 'eslint.config.mjs'],
    extends: [tseslint.configs.disableTypeChecked],
    languageOptions: {
      globals: {
        require: 'readonly',
        module: 'writable',
        process: 'readonly',
        console: 'readonly',
        __dirname: 'readonly',
      },
    },
    rules: {
      // The build script is the one place that talks to a terminal.
      'no-console': 'off',
      '@typescript-eslint/no-require-imports': 'off',
    },
  },

  {
    name: 'enfusion/tests',
    files: ['**/*.test.ts'],
    rules: {
      // `node:test` hands back a promise nobody is meant to await; the runner does the waiting.
      '@typescript-eslint/no-floating-promises': 'off',
    },
  },
);
