# Enfusion

A VS Code extension for Enfusion mods: what the Workbench plugins do today, in the editor the code
is written in anyway.

For now this is a skeleton: the Activity Bar gains an **Enfusion** container with a **Mods** tree,
which shows the mods of the open workspace. A mod is a folder with a `config.cpp` in its root (the
same rule by which pboProject packs one into a single pbo), and the mod's name is the name of that
folder, because that is how it is linked onto the work drive (`P:\<Mod>`) and loaded by the game
(`@<Mod>`).

## Development

| Command | What it does |
|---|---|
| `npm install` | dependencies |
| `npm run watch` | esbuild in watch mode, which is also the `preLaunchTask` for `F5` |
| `npm run check-types` | `tsc --noEmit` (esbuild only transpiles, it checks no types) |
| `npm run lint` | ESLint with type checking |
| `npm test` | builds `*.test.ts` through esbuild and runs `node --test`, with no extension host |
| `npm run vsix` | build the `.vsix` |

`F5` puts up an Extension Development Host and opens the folder one level above this one in it, so
that the tree has some mods to show straight away.

## Layout

```
src/
  extension.ts        composition root: everything is made and disposed here
  mods/               the domain: finding the mods by their paths, with no vscode — hence tests on bare Node
  platform/           access to the workspace: findFiles, the watcher, Uri
  view/               the Mods tree
```

The "the domain knows nothing of the host" boundary is held by `no-restricted-imports` in
`eslint.config.mjs` rather than by convention: `src/mods/**` cannot import `vscode` or the
`platform`/`view` layers.

