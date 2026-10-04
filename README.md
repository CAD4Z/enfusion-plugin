<div align="center">
    <a href="https://github.com/CAD4Z/enfusion-plugin"><img src="https://github.com/user-attachments/assets/f554aa83-bc3e-4c86-a489-24caff9b2c9c"></a>

</div>

<div align="center">
    <img src="https://img.shields.io/github/issues/CAD4Z/enfusion-plugin?style=for-the-badge" alt="open issues" />
    <img src="https://img.shields.io/badge/version-0.0.28-blue?style=for-the-badge" alt="version" />
    <img src="https://img.shields.io/badge/platform-Windows%20x64-lightgrey?style=for-the-badge" alt="platform" />
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-MIT-green?style=for-the-badge" alt="license" /></a>
</div>

<br />

<div align="center">
  🛠️Enfusion tooling for your VS Code!
</div>

<div align="center">
  <sub>
    Built with love
    &bull; Brought to you by <a href="https://github.com/CAD4Z">@CAD4Z</a>
    and other <a href="https://github.com/CAD4Z/enfusion-plugin/graphs/contributors">contributors</a>
  </sub>
</div>

## Introduction

A VS Code extension for DayZ mods on the Enfusion engine. It does what the Workbench plugins and a
folder of batch files do — mount the work drive, pack and sign the pbos, put a client and a dev
server up — in the editor the scripts are written in anyway, and adds what they never had: the
script log of both sides in the Debug Console, a second client on a Steam account of its own, and
an EDDS converter and viewer that need no DayZ Tools at all.

If you write mods with an AI agent, pair it with
[CAD4Z/enfusion-skills](https://github.com/CAD4Z/enfusion-skills): the skills know Enforce Script,
the extension builds and runs it.

## Features

### Mods panel

An **Enfusion** container in the Activity Bar lists the mods of the workspace and their addons, in
the order they build, worked out from `requiredAddons`. At its top, two lists pick the launch
**target** and the **Debug** or **Release** build; under them, one group of buttons holds **Start**,
**Add client** and **Build**, and another the work drive tools — **Mount** / **Unmount**, **Link
mods** and **Workbench**. A button that would only fail is disabled and says why in its tooltip.

### Build

- Packs every addon, or one, with **pboProject** or DayZ Tools' **AddonBuilder**, in dependency order.
- Signs each pbo with `DSSignFile.exe` and puts the `.bikey` into the built mod's `Keys`.
- Puts build errors into Problems, on the line of your own file rather than its twin on `P:`.
- Queues a second press instead of refusing it, and collapses the queue so nothing builds twice.

### Launch

- **Run and Debug** or the panel's **Start** puts up a client, the server alone, or both, with the
  client joining the server on its own. No `launch.json` is needed.
- **Debug** runs `DayZDiag_x64.exe` with file patching, straight off your sources; **Release** runs
  the retail client and server off the packed pbos, the way a player and a host will.
- The script log of the client and the server arrives in the Debug Console as one stream, marked
  `[CLIENT]` and `[SERVER]`.
- Profiles and the dev mission are layered from the mod's own `Profiles` and `Missions`, in a run
  folder per checkout; neither the game folder nor the work drive is changed.
- **Stop** puts down every process of the launch. The client comes up borderless and behind the
  window you were working in, instead of taking the focus.
- Targets can run from the DayZ Experimental client and server instead.

### Second client

**Add client** puts one more client into the running launch. Given a second Steam account, it runs
in a Sandboxie-Plus box with a Steam of its own and joins the same server; the box is made and
signed in for you, once.

### Mod manifests

- A mod is configured by one `mod.enf` — JSONC with a schema, so fields complete and typos are
  underlined. A monorepo can put a `workspace.enf` on top.
- Both open as a **form** over the same document: undo, `Ctrl+S`, the diff and the comments in the
  file stay as they are.
- **Create Mod** scaffolds a mod that builds and loads on the spot: `config.cpp`, the four script
  modules, `mod.cpp`, `Inputs.xml`, `stringtable.csv` and a manifest with a ready target.
- **Create mod.enf** adopts somebody else's mod from its `CfgMods`, and **Add Addon** adds an addon
  and writes it into `requiredAddons`.

### Textures

- **EDDS preview** opens any `.edds`, no project needed: every mip, R/G/B/A channels, alpha
  checkerboard, zoom and pan.
- **Convert Texture to EDDS** turns `.png`, `.tga`, `.jpg`, `.tiff` and `.dds` into EDDS with the
  recipe Workbench reads in `.edds.meta`, showing the source beside the converted result before
  anything is written.
- Every LDR format Workbench offers — BGRA, DXT1/DXT5, BC4, BC5, BC7, R8, R8G8 — with mips, all nine
  swizzling modes and batches of up to 256 files.
- An interrupted conversion leaves the old texture pair or the new one, never half of each.

### Fonts

The bundled `enfusion.exe` also makes the SDF fonts the engine draws from TrueType fonts, with the
recipe kept in `.fnt.meta` — see [native/README.md](native/README.md#fonts-sdf-fonts-from-truetype).

## Requirements

- Windows 10 or 11, x64, and VS Code 1.94 or later.
- **DayZ** and **DayZ Tools** from Steam, and the work drive set up with DayZ Tools, for building and
  launching.
- **pboProject** (Mikero), the default builder — or switch `enfusion.builder` to AddonBuilder.
- **DayZ Server** from Steam, for a Release launch that puts a server up.
- **Sandboxie-Plus** and a second Steam account, for a second client that joins a server.

EDDS preview and texture conversion need none of these. Paths are read from the registry and from
Steam's library list, so in the usual case nothing has to be configured.

## Install

There is no Marketplace listing yet. Every CI run builds `enfusion-plugin-win32-x64.vsix`: download
it from the run's artifacts on the Actions tab, or build it yourself (see
[Development](#development)), then install it:

```
code --install-extension enfusion-plugin-win32-x64.vsix
```

## Quick start

1. Open the folder that holds your mods. The **Enfusion** panel lists the mods it finds: those with
   a `mod.enf`, and those without one by the `CfgMods` in their `config.cpp`.
2. No mod yet? Run **Enfusion: Create Mod**. A mod without a manifest? Press **+ Create mod.enf** on
   its card.
3. Press **Mount** to put the work drive up (set `enfusion.workDrive.source` first if `P:` has never
   been mounted on this machine), then **Link mods**.
4. Press **Build**.
5. Pick a target at the top of the panel and press **Start**, or `F5`.

## Configuration

### `mod.enf`

What **Create Mod** writes, give or take the comments:

```jsonc
{
  "name": "MyMod",
  "version": "0.1.0",
  // "description": "What the mod does, in a sentence.",
  // "author": "Who made it.",

  "launch": {
    // Where the built mod goes, counted from this file: Addons\@MyMod.
    "modsDirectory": "Addons",
    // What every target loads, in load order.
    "mods": ["@MyMod"],
    "targets": [
      { "name": "Client", "map": "ChernarusPlus", "run": "client" }
    ]
  }
}
```

| Field | What it is |
| --- | --- |
| `name` | The mod's one name: `P:\<name>` on the work drive, `@<name>` when built, `dir` in `CfgMods`. The folder's name when left out. |
| `exclude` | File masks the builder leaves out of the pbo; replaces the default list whole. |
| `launch.modsDirectory` | Where the `@<Mod>` folders are built and loaded from, relative to this file. |
| `launch.mods` | Mods the client and the server load, in order; your own mods are named here too. |
| `launch.serverMods` | Additional mods only the server loads. |
| `launch.targets[]` | The Run and Debug entries: `name`, `run` (`client`, `server` or `both`), `map`, `mod`, `experimental`, `serverConfig`, and their own `mods` / `serverMods`. |

A `workspace.enf` above several mods owns the `launch` block for all of them and can `ignore`
folders the window should not see. The schema explains every field on hover.

### Settings

Everything about the machine lives in VS Code settings with `scope: machine`, so a repository never
carries it. All of them can stay empty in the usual case.

| Setting | What it is |
| --- | --- |
| `enfusion.dayz.path` | DayZ. From the registry and Steam when empty. |
| `enfusion.dayz.executable` | The diag build a Debug launch starts; `DayZDiag_x64.exe` when empty. |
| `enfusion.dayzServer.path` | DayZ Server. From Steam when empty. |
| `enfusion.dayzExperimental.path`, `enfusion.dayzExperimentalServer.path` | DayZ Experimental client and server. From Steam when empty. |
| `enfusion.dayzTools.path` | DayZ Tools. From the registry and Steam when empty. |
| `enfusion.builder` | `pboProject` (default) or `AddonBuilder`. |
| `enfusion.pboProject.path` | `pboProject.exe`. From the registry when empty. |
| `enfusion.signing.enabled`, `enfusion.signing.privateKey` | Whether pbos are signed, and the `.biprivatekey` they are signed with. |
| `enfusion.workDrive.source`, `enfusion.workDrive.letter` | The folder the work drive is mounted from, and its letter (`P:`). |
| `enfusion.filePatching.root` | Where run folders are built; `%LOCALAPPDATA%\Enfusion\run` when empty. |
| `enfusion.launch.profiles` | Where launch profiles go; point it at `P:\Profiles` to share them with the Workbench plugins. |
| `enfusion.launch.secondAccount` | The Steam account the second client signs in as. |
| `enfusion.sandboxie.path`, `enfusion.steam.path` | Sandboxie-Plus and Steam, for the second client. From the registry when empty. |

## Commands

| Command | Where |
| --- | --- |
| Enfusion: Create Mod | Panel header, folder context menu in the Explorer |
| Enfusion: Create mod.enf for an Existing Mod | An unconfigured mod's card |
| Enfusion: Add Addon | Under a mod's addons |
| Enfusion: Build Addon / Build All Addons | An addon's row / the panel's **Build** |
| Enfusion: Start the Game / Add a Second Client | The panel's **Start** / **Add client** |
| Enfusion: Select Launch Target / Select Launch Build | The panel's two lists, the status bar |
| Enfusion: Mount Work Drive / Unmount Work Drive / Link Mods onto the Work Drive | The panel's work drive tools |
| Enfusion: Open Workbench | The panel's **Workbench** |
| Enfusion: Open EDDS Preview | Context menu of a `.edds` |
| Enfusion: Convert Texture to EDDS | Context menu of a source image, in a window with a mod |
| Enfusion: Refresh Mods | Panel header |

How each of these behaves, and why — the findings about pboProject, DayZDiag, BattlEye, Steam and
Sandboxie that shaped them — is written down in [MANUAL.md](MANUAL.md).

## Development

| Command | What it does |
|---|---|
| `npm install` | dependencies |
| `npm run watch` | esbuild in watch mode, which is also the `preLaunchTask` for `F5` |
| `npm run check-types` | `tsc --noEmit` over the host, browser and test projects (esbuild only transpiles, it checks no types) |
| `npm run lint` | ESLint with type checking |
| `npm test` | builds `*.test.ts` through esbuild and runs `node --test`, with no extension host |
| `npm run vsix` | build the `.vsix` |
| `cmake -S native -B native/.build -A x64` | configure the C17 `enfusion.exe` with MSVC |
| `cmake --build native/.build --config Release` | build `enfusion.exe` with the static CRT |
| `ctest --test-dir native/.build -C Release` | run native core and black-box CLI tests |

`F5` puts up an Extension Development Host and opens the folder one level above this one in it, so
that the panel has some mods to show straight away.

## Project structure

```
.
├── src/
│   ├── extension.ts    composition root: everything is made and disposed here
│   ├── mods/           the domain, with no vscode: mods, manifests, builds, launches, textures
│   ├── platform/       access to the workspace and the machine: files, processes, the registry
│   ├── view/           the extension's side of the panel and the editors
│   └── webview/        the browser's side: the panel, the form, the texture editors
├── test/               mirrors src/: bare-Node tests, plus the smoke test of the installed VSIX
├── schemas/            the JSON schemas of mod.enf and workspace.enf
├── native/             the C17 enfusion.exe, its tests and fuzz targets — see native/README.md
├── resources/          the Activity Bar icon
├── scripts/            packaging checks
├── MANUAL.md           how everything behaves, and why
└── THIRD-PARTY.md      notices for what the extension bundles
```

The rule that the domain knows nothing of the host is held by `no-restricted-imports` in
`eslint.config.mjs`: `src/mods/**`, `src/webview/**` and their tests cannot import `vscode` or the
`platform` and `view` layers.

## License

MIT — see [LICENSE](./LICENSE); bundled third-party work is listed in
[THIRD-PARTY.md](./THIRD-PARTY.md). DayZ and Enfusion are trademarks of Bohemia Interactive a.s.;
this extension is not affiliated with or endorsed by Bohemia Interactive.
