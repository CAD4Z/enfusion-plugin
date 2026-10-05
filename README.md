<div align="center">
    <a href="https://github.com/CAD4Z/enfusion-plugin"><img src="https://github.com/user-attachments/assets/8fc337cd-14ef-4bf7-902f-4ea54641f030"></a>

</div>

<div align="center">
    <img src="https://img.shields.io/github/issues/CAD4Z/enfusion-plugin?style=for-the-badge" alt="open issues" />
    <img src="https://img.shields.io/badge/version-0.0.29-blue?style=for-the-badge" alt="version" />
    <img src="https://img.shields.io/badge/platform-Windows%20x64-lightgrey?style=for-the-badge" alt="platform" />
    <a href="LICENSE"><img src="https://img.shields.io/badge/license-PolyForm%20Strict%201.0.0-red?style=for-the-badge" alt="license" /></a>
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
A folder with no mod in it gets **Create Mod** and **Create Workspace** in the middle of the panel
instead.

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
in a [Sandboxie-Plus](https://sandboxie-plus.com/) box with a Steam of its own and joins the same
server; the box is made and signed in for you, once.

### Mod manifests

- A mod is configured by one `mod.enf` — JSONC with a schema, so fields complete and typos are
  underlined. A monorepo can put a `workspace.enf` on top.
- Both open as a **form** over the same document: undo, `Ctrl+S`, the diff and the comments in the
  file stay as they are.
- **Create Mod** scaffolds a mod that builds and loads on the spot: `config.cpp`, the four script
  modules, `mod.cpp`, `Inputs.xml`, `stringtable.csv`, a manifest with a ready target, a dev
  `server.cfg` and a Workbench project. A server target of a mod with no mission of its own runs
  DayZ's `dayzOffline.<map>`. It refuses a folder inside a mod or holding mods, so it breaks
  nothing there.
- **Create Workspace** makes a folder of several mods: a `workspace.enf` that launches them all
  and one Workbench project; **Create Mod** on it gives each mod a folder of its own.
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

The preview and single-source editor include read-only histograms, a pixel inspector and Result
runtime memory for the selected mip and complete chain. Histograms use the selected channels;
pixel coordinates follow the displayed mip through zoom and pan. Memory counts the GPU payload
after container decompression, excluding driver alignment, rather than the EDDS file size.

Source/Result error is RMSE in decoded sample units, without gamma correction or alpha weighting.
The current Source preview contains the original mip 0: it is comparable when swizzling,
normalization and mip removal are off. Other mips or transformed profiles explain why matching
Source pipeline samples are unavailable; standalone EDDS has no Source comparison. Analysis
never starts a codec or writes a recipe. Analysis is limited to 1,048,576 pixels per mip; select a
smaller mip for larger textures. A transient 16 MiB cache reuses previously viewed mips and is
discarded when the profile changes.

### Fonts

Right-click a `.ttf` inside a discovered mod or workspace and choose **Generate Enfusion Font…**.
Choose a size (32 by default, 8–40), a nearby `.txt` character set or the built-in set, and a name.
The command writes `.fnt`, `.edds` and the recipe `.fnt.meta` beside the source, reports missing
characters and glyphs whose strokes are thinner than an atlas pixel, and asks before replacing
existing files. Outside a mod's prefix root, as with textures, it writes the font without a recipe.
**Regenerate Enfusion Font** on the `.fnt` uses the saved recipe and keeps its GUID. Both commands
show cancellable progress; no Workbench or DayZ Tools are needed. The same generator is available
through the [native CLI](native/README.md#fonts-sdf-fonts-from-truetype).

## Requirements

- Windows 10 or 11, x64, and VS Code 1.94 or later.
- **DayZ** and **DayZ Tools** from Steam, and the work drive set up with DayZ Tools, for building and
  launching.
- **pboProject** (Mikero), the default builder — or switch `enfusion.builder` to AddonBuilder.
- **DayZ Server** from Steam, for a Release launch that puts a server up.
- **[Sandboxie-Plus](https://sandboxie-plus.com/)** and a second Steam account, for a second client
  that joins a server.

EDDS preview and texture conversion need none of these. Paths are read from the registry and from
Steam's library list, so in the usual case nothing has to be configured.

## Install

Install [Enfusion from the Visual Studio Marketplace](https://marketplace.visualstudio.com/items?itemName=hurfy.enfusion-plugin).

Every CI run builds `enfusion-plugin-win32-x64.vsix`: download
it from the run's artifacts on the Actions tab, or build it yourself (see
[Development](#development)), then install it:

```
code --install-extension enfusion-plugin-win32-x64.vsix
```

## Quick start

Everything below happens in the **Enfusion** panel in the Activity Bar. The work drive has to have
been set up with DayZ Tools once, the way any DayZ modding starts: `P:` with the game data unpacked
onto it. **Mount** puts it up when it is down.

### Paths and settings

DayZ, DayZ Server, DayZ Tools, pboProject, Steam and Sandboxie-Plus are found on their own, in the
registry and in Steam's library list; what was found and what was not is written to the
**Enfusion** log in the Output panel. A setting is needed only for what is not found, and for a few
choices: open **Settings** and search for `enfusion`. They are machine settings, so a repository
never carries them and every developer sets their own.

- `enfusion.workDrive.source` — the folder holding the unpacked game data, if `P:` has never been
  mounted on this machine. `enfusion.workDrive.letter` if it is not `P:`.
- `enfusion.signing.privateKey` — the `.biprivatekey` the pbos are signed with (`DSCreateKey.exe` in
  DayZ Tools makes a pair). Without one every build warns that it went out unsigned; turn
  `enfusion.signing.enabled` off to build unsigned without the warning.
- `enfusion.builder` — `AddonBuilder` from DayZ Tools instead of pboProject.
- `enfusion.dayz.path`, `enfusion.dayzServer.path`, `enfusion.dayzTools.path`,
  `enfusion.pboProject.path` — an installation the registry and Steam do not point at.
- `enfusion.launch.profiles` — where the profiles of a launch go, `.RPT` and `.ADM` among them;
  point it at `P:\Profiles` to share them with the Workbench plugins.
- `enfusion.launch.secondAccount` — the Steam account of the second client.

Every setting is in the [table below](#settings).

### A mod of your own

1. Open the folder the mod will live in: an empty folder, or the repository it is going into. A
   folder with no mod in it shows **Create Mod** and **Create Workspace** in the middle of the
   panel.
2. Press **Create Mod**. It asks two things: the name, and whether the mod packs into one pbo or
   into one pbo per addon. The name is used everywhere at once — the folder, `dir` in `CfgMods`,
   `P:\<Name>` and `@<Name>` — so it is letters, digits and underscores.
3. The mod is written into the folder and linked onto the work drive. If `P:` was down, the notice
   offers **Mount Work Drive**; press **Link mods** after it.

   ```
   MyMod/                        the folder you opened: the mod's root
   ├── mod.enf                   the name, the version and the launch block
   ├── server.cfg                what a target with a dev server puts it up with
   ├── .gitignore
   ├── MyMod/                    the prefix root: linked as P:\MyMod, built into @MyMod
   │   ├── config.cpp            CfgPatches and CfgMods
   │   ├── mod.cpp               what the DayZ launcher shows
   │   ├── stringtable.csv
   │   └── Scripts/
   │       ├── Inputs.xml
   │       ├── 1_Core/MyMod.c    and the same in 3_Game, 4_World and 5_Mission
   │       └── ...
   ├── Missions/Global/          laid over the mission of every launch
   ├── Profiles/Global/          laid into the profile of every launch; Dev/ only into Debug ones
   ├── Profiles/Dev/
   ├── Workbench/dayz.gproj      the project the Workbench button opens
   └── Addons/                   where Build puts @MyMod; ignored by git
   ```

   With one pbo per addon, `config.cpp` and `stringtable.csv` sit in `Scripts/` instead, and every
   addon added later with **Add Addon** is a folder of its own beside it.
4. Press **Build**. The pbo is packed (pboProject by default), signed, and put into
   `Addons\@MyMod`; a build error lands in Problems, on the line of your own file.
5. Pick **Debug** and the **Client** target at the top of the panel and press **Start**, or `F5`.
   DayZDiag comes up with the mod loaded straight off your sources, on the map's offline mission,
   and the script log streams into the Debug Console.
6. For a dev server, add a target to `mod.enf` — the form shows it as a row, or write it by hand:

   ```jsonc
   "targets": [
     { "name": "Client", "map": "ChernarusPlus", "run": "client" },
     { "name": "Client and server", "map": "ChernarusPlus", "run": "both" }
   ]
   ```

   The server starts with `server.cfg` and the client joins it on its own. Until the mod keeps a
   mission of its own in `Missions\MyMod.<map>`, the server runs DayZ's `dayzOffline.<map>` with
   your `Missions` layers over it. **Release** runs the retail game off the packed pbos instead.
7. **Workbench** opens `Workbench\dayz.gproj`, which already lists the mod's script folders.

### Several mods in one repository

1. Open the repository's folder and press **Create Workspace**. It writes a `workspace.enf` that
   owns the launch of every mod under it, one `Addons` they are all built into, one `server.cfg`
   and one Workbench project.
2. Press **Create Mod** once for each mod. Each goes into a folder of its own named after it, gets a
   `mod.enf` with no launch block of its own, is added to `mods` in `workspace.enf` and to the
   Workbench project, and is linked onto `P:`.

   ```
   MyMods/
   ├── workspace.enf             the launch of every mod: mods, targets, ignore
   ├── server.cfg
   ├── .gitignore
   ├── Workbench/dayz.gproj      one project with the scripts of every mod
   ├── Addons/                   @MyMod and @OtherMod after a Build
   ├── MyMod/                    a mod: mod.enf, the MyMod/ prefix root, Missions/, Profiles/
   └── OtherMod/
   ```
3. Keep `mods` in load order, a mod after the mods it needs, and give each target the mod whose
   profile, mission and `server.cfg` it runs with:

   ```jsonc
   "launch": {
     "modsDirectory": "Addons",
     "mods": ["@MyMod", "@OtherMod"],
     "targets": [
       { "name": "Client", "map": "ChernarusPlus", "run": "client" },
       { "name": "Other server", "map": "ChernarusPlus", "run": "both", "mod": "OtherMod" }
     ]
   }
   ```

   A target that names no `mod` runs with the first mod of the workspace. Third-party mods are
   named in `mods` too, by their folder under `Addons`; nothing is loaded that is not named there.
4. **Build** packs every mod, in the order their `requiredAddons` give — so a mod that needs
   another names that mod's `CfgPatches` class there — and **Start** launches them all. A folder
   listed in `ignore` is left out of the window: nothing under it is listed, built, linked or
   launched.

### A mod that is already there

Open the folder that holds it. A mod with a `mod.enf` is listed as it is. One without — somebody
else's, or yours from before this extension — is found by the `CfgMods` in its `config.cpp` and
listed as unconfigured: **+ Create mod.enf** on its card reads the name, description and author out
of `CfgMods`, writes the manifest and links the mod. From there it builds and starts like any other.

The details of every step, and the reasons behind them, are in [MANUAL.md](MANUAL.md).

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
| Enfusion: Create Mod | Panel header, an empty panel, folder context menu in the Explorer |
| Enfusion: Create Workspace | Panel header menu, an empty panel, folder context menu in the Explorer |
| Enfusion: Create mod.enf for an Existing Mod | An unconfigured mod's card |
| Enfusion: Add Addon | Under a mod's addons |
| Enfusion: Build Addon / Build All Addons | An addon's row / the panel's **Build** |
| Enfusion: Start the Game / Add a Second Client | The panel's **Start** / **Add client** |
| Enfusion: Select Launch Target / Select Launch Build | The panel's two lists, the status bar |
| Enfusion: Mount Work Drive / Unmount Work Drive / Link Mods onto the Work Drive | The panel's work drive tools |
| Enfusion: Open Workbench | The panel's **Workbench**; writes the project first if the mod has none |
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
| `npm run check:text` | every tracked and new file against what a public repository must not hold: Cyrillic, invisible characters, machine paths, tracker references, files left behind |
| `npm run check:format` | the native C sources against clang-format 20+ and the native Python tests against ruff, rewriting nothing |
| `git config core.hooksPath .githooks` | once per clone: the two checks above run on the staged files before every commit |
| `npm run install:local` | builds a commit in a clean worktree, checks it the way CI does and installs the VSIX into VS Code |
| `npm run logs` | the extension's log from every VS Code window, with the folder each window has open |
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
├── scripts/            text, format and packaging checks, the local install, the log finder
├── MANUAL.md           how everything behaves, and why
└── THIRD-PARTY.md      notices for what the extension bundles
```

The rule that the domain knows nothing of the host is held by `no-restricted-imports` in
`eslint.config.mjs`: `src/mods/**`, `src/webview/**` and their tests cannot import `vscode` or the
`platform` and `view` layers.

## License

PolyForm Strict 1.0.0 — see [LICENSE](./LICENSE). Source-available, not open source: you may read
and use this extension for any noncommercial purpose, but not redistribute or modify it.
Third-party work it bundles keeps its own license, as listed in [THIRD-PARTY.md](./THIRD-PARTY.md).
DayZ and the Enfusion engine are the property of Bohemia Interactive; this extension is not
affiliated with or endorsed by Bohemia Interactive.
