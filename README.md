# Enfusion

A VS Code extension for Enfusion mods: what the Workbench plugins do today, in the editor the code
is written in anyway.

For now this is a skeleton: the Activity Bar gains an **Enfusion** container with a **Mods** panel,
which shows the mods of the open workspace, and under each of them its addons in the order they will
be built.

A mod is a folder with a `mod.enf`. Inside it is the **prefix root**, the folder named after the
mod: that one is linked onto the work drive (`P:\<Mod>`) and loaded by the game (`@<Mod>`). Inside
the prefix root are the **addons**, folders with a `config.cpp`, one pbo each. The layout is worked
out from the tree rather than declared: a `config.cpp` in the prefix root itself means the whole mod
packs into one pbo, its absence means the addons are subfolders. The order of both the mods and the
addons comes off the `requiredAddons` graph. A folder with a `config.cpp` and no `mod.enf` reaches
the list as an unconfigured mod; a folder with no mod reaches it not at all.

A mod is configured by one file, `mod.enf` in its root — the way `package.json` configures a
package; a monorepo may put an optional `workspace.enf` on top. The format is JSONC, with a schema
registered by file name, so the editor completes the fields and underlines the typos. A `launch`
block may be written in either file, but only one of them owns it: where a `workspace.enf` exists,
the block in `mod.enf` is ignored entirely.

The paths to DayZ, to DayZ Tools and to `pboProject.exe`, the private key, the source and the letter
of the work drive, the file patching root and the choice of builder are about the machine rather
than about the mod, so they live in VS Code settings with `scope: machine`, which the editor
physically will not let a workspace write. All three paths are read out of the registry by default,
so in the ordinary case nothing has to be entered; what resolved and what did not is visible in the
panel.

The work drive is mounted and unmounted from the same panel: the buttons call `subst` with the
folder and the letter out of the machine settings, and the panel shows where the letter actually
leads. A drive mounted somewhere other than what is configured is a warning with both folders in it,
rather than a quiet build of the wrong sources. The **Link mods** button lays junctions across the
root of the drive onto the prefix roots of every mod of the workspace — what `SetupWorkdrive.bat`
used to do: a junction already pointing where it should is not an error and is not repointed, one
pointing elsewhere is repointed, and a real folder in its place is left untouched and shown as it
is. Every mod in the list shows whether it is linked or not, so the reason a build would fail is
visible beforehand. Unpacking the vanilla data and setting the drive up in the first place with DayZ
Tools is not part of this.

What is built is an **addon** rather than a mod: every addon in the list has a **Build** button of
its own, and the icon in the panel's header builds the whole workspace — in the dependency order out
of `requiredAddons`, the same order the addons are listed in. The button is unconditional: staleness
is not tracked, and the extension will not argue about whether a rebuild is needed. What comes out
is a `<modsDirectory>\@<Mod>` folder with the pbos, the signatures, `mod.cpp` and the public key;
`modsDirectory` comes out of the `launch` block of whichever `.enf` owns that mod, and a relative
path is taken from the folder of that file, so that it means the same thing on any machine.

Three things the Enforce Script plugins suffered for are kept literally, and re-checked against the
live tools. The builder is started through `start`, in a console of its own — without one pboProject
exits with code 1 immediately, having built nothing. Success is decided by the pbo appearing rather
than by the exit code: both builders answer zero to a failure too. A failed build is retried exactly
once, after which the path to the packing log is shown. A fourth thing turned up during that
re-check: pboProject pointed at a folder that does not exist quietly does nothing — which is why the
folders of the built mod are made before it is started.

Signing is a separate step through `DSSignFile.exe`, the same for both builders; an empty key means
"do not sign", while a key with no `DSSignFile.exe` is a refusal rather than a quietly unsigned pbo.
The packing exclusions come out of `exclude` in `mod.enf` and replace the default list whole;
AddonBuilder is not given them, because `-exclude=` brings it down (1.0.240639) with an
`ArgumentNullException` on any list at all, its own example included. Build errors are read out of
the packing log and reach Problems — on the line of the file of the workspace the builder was
talking about, not on its twin on `P:`; an addon that failed without a place to point at is marked
on its own `config.cpp`. Building is the only thing that puts paths out of a `mod.enf` on a command
line, so it is the only thing that wants the folder trusted (Workspace Trust).

One more thing about AddonBuilder, nothing to do with this extension but worth knowing in advance:
it binarises through `binarize.exe -addon="P:"`, which is to say it reads **every** config on the
work drive. One broken `config.cpp` in any third-party mod on `P:` brings down any build of any mod
— with an empty message and code 1. pboProject reads only the addon it is packing.

## Development

| Command | What it does |
|---|---|
| `npm install` | dependencies |
| `npm run watch` | esbuild in watch mode, which is also the `preLaunchTask` for `F5` |
| `npm run check-types` | `tsc --noEmit` over both projects (esbuild only transpiles, it checks no types) |
| `npm run lint` | ESLint with type checking |
| `npm test` | builds `*.test.ts` through esbuild and runs `node --test`, with no extension host |
| `npm run vsix` | build the `.vsix` |

`F5` puts up an Extension Development Host and opens the folder one level above this one in it, so
that the panel has some mods to show straight away.

## Layout

```
src/
  extension.ts        composition root: everything is made and disposed here
  mods/               the domain: the model of the mods and the config.cpp parsing, with no vscode — hence tests on bare Node
  platform/           access to the workspace: findFiles, reading files, the watcher, Uri
  view/               the Mods panel on the extension's side: the webview, the messages, opening files
  webview/            the Mods panel on the browser's side: a tsconfig of its own, DOM instead of Node
schemas/              the JSON schemas of `mod.enf` and `workspace.enf`, registered through jsonValidation
```

The "the domain knows nothing of the host" boundary is held by `no-restricted-imports` in
`eslint.config.mjs` rather than by convention: `src/mods/**` and `src/webview/**` cannot import
`vscode` or the `platform`/`view` layers. The panel has a `src/webview/tsconfig.json` of its own on
top of that — with DOM and without the Node types — so host API does not compile there even by
accident.

