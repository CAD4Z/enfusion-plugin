# Enfusion

A VS Code extension for Enfusion mods: what the Workbench plugins do today, in the editor the code
is written in anyway.

The Activity Bar gains an **Enfusion** container with a **Mods** panel. Along its top is a row of
buttons for everything at once: **Start** puts the game up, **Build** builds the workspace, and the
three square ones on the right mount the work drive, unmount it, and link the mods onto it. A button
that would only fail is disabled and says why in its tooltip, so the reason is there before the
press rather than after it. Below the row is `workspace.enf` and the mods under it, each mod with
its addons in the order they will be built.

A mod's row is its manifest: clicking it opens `mod.enf`, the way a file in the explorer does, and
the only thing written on it is the mod's name. No paths are written there because there is nothing
to write: the mod's name is `P:\<Mod>` and `@<Mod>`.

A mod is a folder with a `mod.enf`. Inside it is the **prefix root**: that folder is what gets
linked onto the work drive (`P:\<Mod>`) and what the game loads (`@<Mod>`). Inside the prefix root
are the **addons**, folders with a `config.cpp`, one pbo each. The layout is worked out from the
tree rather than declared: a `config.cpp` in the prefix root itself means the whole mod packs into
one pbo, its absence means the addons are subfolders. The order of both the mods and the addons
comes off the `requiredAddons` graph. A folder with a `config.cpp` and no `mod.enf` reaches the list
as an unconfigured mod; a folder with no mod reaches it not at all.

A mod is configured by one file, `mod.enf` in its root — the way `package.json` configures a
package; a monorepo may put an optional `workspace.enf` on top. The format is JSONC, with a schema
registered by file name, so the editor completes the fields and underlines the typos. A `launch`
block may be written in either file, but only one of them owns it: where a `workspace.enf` exists,
the block in `mod.enf` is ignored entirely.

The `name` field in `mod.enf` is not a title but a name: the panel shows the mod under it, its
prefix root goes up on `P:\<Name>` under it, it builds into `@<Name>`, and the same name has to
stand in `dir` in `CfgMods`. A mod that did not name itself is called after its folder — which is
why a mod in a folder called `client` that is really `NavigationClient` has to write its name down,
or it links as `P:\client` and builds into `@client`. What the launcher shows a player is
`mod.cpp`'s business, and the title there can be anything at all.

Both files open as a **form**: `mod.enf` and `workspace.enf` are fields rather than text, so the
field names need not be remembered, and what each one means is written under it. The form is a
second view of the same document, not a second copy of it: undo, the unsaved dot, `Ctrl+S`, the diff
and conflict resolution all stay exactly what they are for the text. An edit by hand shows in the
form at once, an edit in the form shows in the text; switching is the editor's own — **Reopen Editor
With...**, or the **Edit as Text** button in the form's header. What the form writes is not the
whole file but the piece of text the field sits in, so the comments, the order of the fields and the
layout around them stay as they were, and the diff shows exactly the line that changed. An empty
field is a field the file does not have: cleared, it is removed rather than written as an empty
string; and the form will not write back what is already there, so as not to mark the file dirty for
nothing. Removing a field is the form's own work rather than `jsonc-parser`'s: that one takes
everything between the field and its neighbour, which is to say the trailing comment on the line
above and the comment over the line below as well — and the last element of a single-line list it
leaves unparsable altogether. Only what belongs to the field goes: the lines it is written on, the
comments and blank lines above it that no neighbour is written on, and the comma it was stitched on
by.

There are three cases where the form shows the file but does not write to it, and says why. A syntax
error: there is nothing to aim at, and guessing means losing the half of the manifest that did
parse. A key written twice: one of them is shown, and an edit would land in the other. A list that
was not read whole — a number among the masks, a target with no name: fewer rows are visible than
the file has elements, and every one after the gap sits somewhere other than where it looks. In all
three the file is shown read as far as it read, with a list of errors, each of which opens the text
at its own place.

A `launch` block in a `mod.enf` that a `workspace.enf` above it owns is marked by the form as
ignored, right where it is written.

The paths to DayZ, to DayZ Tools and to `pboProject.exe`, the game executable to run, the private
key, the source and the letter of the work drive, the file patching root and the choice of builder
are about the machine rather than about the mod, so they live in VS Code settings with
`scope: machine`, which the editor physically will not let a workspace write. In the ordinary case
nothing has to be entered at all. All three paths are read out of the registry, where the installers
wrote them; DayZ and DayZ Tools are looked for through Steam as well — by its own list of libraries
and by the app manifest — and that is what covers a registry path that now leads nowhere: a game
moved to another library, a key written by another user. The work drive source defaults to the
folder the letter is already mounted from: the drive DayZ Tools put up is this machine's work drive.
What resolved and what did not is written to the **Enfusion** log on every scan; the panel does not
carry it, because there is nothing to look at there, and what was missing is said by the refusal of
the button that missed it.

A mod is made by **Enfusion: Create Mod** — from the context menu of a folder in the Explorer, from
the panel's header, or straight off an empty panel that has nothing else to show. Exactly two things
are asked: the name, and the layout — one pbo for the whole mod, or a pbo per addon. Everything else
follows from the name rather than being typed: a mod's name is the folder, `dir` in `CfgMods`,
`P:\<Name>`, `@<Name>` and the class in `CfgPatches` all at once, and a slip of case in any one of
them makes a mod that builds and says nothing. What comes out is a `mod.enf` with `modsDirectory`
filled in and one target, a `config.cpp` with `CfgPatches` and `CfgMods`, four script modules
(`1_Core`, `3_Game`, `4_World`, `5_Mission`) — each with a file in it, because the builder does not
carry an empty folder into a pbo — `mod.cpp` for the launcher, `Inputs.xml` with the line in the
config that points at it, a `stringtable.csv` in the main addon's root (the engine reads it from the
root of the pbo; there is nowhere to declare it and no need), empty `Missions\Global`,
`Profiles\Global`, `Profiles\Dev` and `Addons`, and a `.gitignore` that closes off the pbo, the logs
and the private key. The mod is linked onto the work drive as soon as it is made, so it can be built
on the spot. The script module paths in `CfgMods` are the same in both layouts, so a mod moves from
one layout to the other by moving files rather than by rewriting its config.

A mod of somebody else's, with no `mod.enf`, is recognised by its `config.cpp` — by the `CfgMods`
block, the very one a mod declares itself to the game with — and shown in the list marked
unconfigured. A **+ Create mod.enf** row on its card, or the **Enfusion: Create mod.enf for an
Existing Mod** command, gets it a manifest, and nothing is asked twice: the name, the description
and the author come out of `CfgMods` (`name`, `overview`, `author`), and whatever the mod did not
say about itself comes out of its main addon's `CfgPatches`, where `author` and `version` sometimes
sit as well; a name that is nowhere stays the name of the prefix root, which is what the mod links
and loads under. What was read is shown before it is written, and a refusal puts nothing on disk:
the mod stays in the list unconfigured. Exactly one file is written — `mod.enf` in the mod's root —
and the layout does not change that: a single-addon mod declares itself in the prefix root, a
multi-addon one in an addon inside it, and the manifest lands in the mod's root either way. The mod
is linked onto the work drive as soon as it is written, the same as a freshly made one, so from
there it is no different from one of our own: it builds, it links, it launches.

There is one case adoption does not take, and it says why: where the prefix root is the open folder
itself (a repository that links onto `P:\<Name>` whole), the mod's root lies above the workspace,
and a file written there is one nobody will find — the search only looks inside the open folders.
The offer then is to open the folder that holds the mod and write the `mod.enf` there.

An addon is added by **Enfusion: Add Addon** — the row under a mod's list of addons in the panel. It
makes the folder with a `config.cpp` and writes its class into the main addon's `requiredAddons`
there and then: an addon nobody requires is one the engine is free to load whenever it likes, and it
will go missing quietly. It writes it as an edit — the comments, the order of the fields and the way
the list is written stay as they were. A mod whose `config.cpp` sits in the prefix root itself will
not take an addon, and says why: it is one addon whole already, and splitting it means moving files
rather than adding a folder.

The work drive is mounted and unmounted by the three buttons on the right of the row: they call
`subst` with the folder and the letter out of the machine settings. A drive mounted somewhere other
than what is configured is a refusal with both folders in the tooltip, rather than a quiet build of
the wrong sources. The third, **link**, lays junctions across the root of the drive onto the prefix
roots of every mod of the workspace — what `SetupWorkdrive.bat` used to do: a junction already
pointing where it should is not an error and is not repointed, one pointing elsewhere is repointed,
and a real folder in its place is left untouched and shown as it is. A mod that is not linked is
marked in the list, so the reason a build would fail is visible beforehand; a linked one is marked
with nothing, which is how it should be. Unpacking the vanilla data and setting the drive up in the
first place with DayZ Tools is not part of this.

What is built is an **addon** rather than a mod: every addon in the list has a **Build** button of
its own, and the square **Build** at the top builds everything the workspace turned out to hold — in
the dependency order out of `requiredAddons`, the same order the addons are listed in. The button is
unconditional: staleness is not tracked, and the extension will not argue about whether a rebuild is
needed.

Builds run one at a time, and a second one is refused rather than queued. Two builds of one
workspace do not share the work, they fight over one pbo — one taking the file while the other
writes it — and each of them puts a builder process per addon on the machine. The objection is not
theoretical: a button that stays focused is a button a held key presses over and over, and the
machine that met that got five hundred builders and stopped answering. A queue would have been the
same heap of processes, only later. **Start** is one at a time for the same reason: two launches put
up two servers on one port and lay two sets of junctions into one run folder. What comes out is a
`<modsDirectory>\@<Mod>` folder with the pbos, the signatures, `mod.cpp` and the public key;
`modsDirectory` comes out of the `launch` block of whichever `.enf` owns that mod, and a relative
path is taken from the folder of that file, so that it means the same thing on any machine.

Three things the Enforce Script plugins suffered for are kept literally, and re-checked against the
live tools. The builder is started through `start`, in a console of its own — without one pboProject
exits with code 1 immediately, having built nothing. That console comes up minimised (`/MIN`), and
that is not a matter of taste either: pboProject is a windowed program that *attaches* a console to
itself (`AttachConsole`, `CONOUT$`, and `Cannot Attach_Console` when it could not), and failing that
shows its own panel. The extension host has no console of its own, and it hands its children a
hidden window state which every console below it inherits; without `/MIN`, what opens in the console
is the builder's dialog rather than a build. Naming a show state explicitly breaks that inheritance,
and minimised is the only one that does so without taking the screen. Success is decided by the pbo
appearing rather than by the exit code: both builders answer zero to a failure too. A failed build
is retried exactly once, after which the path to the packing log is shown. A fourth thing turned up
during that re-check: pboProject pointed at a folder that does not exist quietly does nothing —
which is why the folders of the built mod are made before it is started.

Signing is a separate step through `DSSignFile.exe`, the same for both builders; an empty key means
"do not sign", while a key with no `DSSignFile.exe` is a refusal rather than a quietly unsigned pbo.
The packing exclusions come out of `exclude` in `mod.enf` and replace the default list whole;
AddonBuilder is not given them, because `-exclude=` brings it down (1.0.240639) with an
`ArgumentNullException` on any list at all, its own example included. Build errors are read out of
the packing log and reach Problems — on the line of the file of the workspace the builder was
talking about, not on its twin on `P:`; an addon that failed without a place to point at is marked
on its own `config.cpp`. Building is one of the two places where a path out of a `mod.enf` reaches a
command line (the other is launching), so it wants the folder trusted (Workspace Trust).

The game is launched by the editor's own **Run and Debug** — and by the blue **Start** in the row at
the top, which does exactly the same thing: the same configuration resolved the same way, so it puts
up the target that is selected in the status bar, and asks only when nothing is selected. The
configurations are handed out dynamically from the targets of the `launch` block, so `launch.json`
is not needed and is never created. One written by hand is useless for configuring, too: a debug
configuration takes exactly `type`, `request` and `target`, and any other field is an error pointing
at `mod.enf`. The selected target is shown in the status bar and changed there; `target` in a
configuration is a target's name, and targets of the same name in different mods are told apart as
`<Mod>: <Name>`. There are no breakpoints, no stacks and no variables, but **Stop** puts down every
process of the launch along with its children (`taskkill /T`). The session ends when any one of them
goes on its own: a client with no server left has nobody to talk to, and a server nobody connects to
any more would otherwise hang about without a single line in the editor to say it is there.

The **Debug Console** carries the script log of both processes — prefixed `[CLIENT]` in green and
`[SERVER]` in red, so it is clear who said what in the one stream. It does not come out of a file:
the diag build reaches out over TCP itself, usually to the Workbench, and hands the `SCRIPT` channel
down that connection — exactly what `script_<stamp>.log` is written from. The extension puts up one
listener per role. The client is told its port by us; the server does not listen to `-debuggerPort`
at all — it writes over it once it knows it is a server, and always goes to **1001** (`-client2`
goes to 1002). That is why the Workbench switches sides with a button rather than with a port. The
start of the log does not reach the console: the game connects on the first tick of the script VM,
and `Module: GameLib; loaded 18x files` and everything a mod prints from a static constructor are
written by then — they are in the profile. The protocol is documented nowhere and was read off
`DayZDiag_x64.exe`.

Before a launch the run folder is put together — by default
`%LOCALAPPDATA%\Enfusion\run\<workspace>`. Inside it is `game\`, the **file patching root**: the
working directory the game will get, holding junctions onto **every folder of the game root**,
obtained by listing it, plus junctions onto the prefix roots of the workspace's mods, plus **copies
of every file of the root** except the programs, the libraries and the logs; neither the game folder
nor the work drive is changed by any of it. The listing is not a detail, and that goes for both
halves. Folders: the Workbench plugins had the list hardcoded as `Addons`, `bliss` and `sakhal`,
while a live installation has had no `bliss` for a long time and does have `!Workshop`, `dta`,
`Missions`, `MainMenu.*` and the rest. Files: there was one name on the list, `steam_appid.txt`,
while the engine also reads `DayZSetting.xml` and `dayz.gproj` out of the working directory, and
without the latter it gets as far as "Cannot find game project settings!", "Failed to create
Enfusion engine" and dies of an access violation, having said nothing. So here too it is a listing
now, and what is written down is only the list of what not to carry over: the game finds its `.exe`
and `.dll` by the path it was started with, and it writes its `.log` itself. A second launch redoes
nothing: a link pointing where it should stays, one that has moved is repointed, one no longer
wanted is taken off, and whatever the game itself wrote into the working directory (logs, dumps) is
not touched at all. A launch refuses to start where the work drive is not mounted, or where there is
no game executable — by default `DayZDiag_x64.exe`, because only the diagnostic build understands
`-filePatching`; the name or the path is changed by the `enfusion.dayz.executable` setting.

A target says what to put up: a client, the server alone, or both at once. Both is one launch: the
server starts first, the client follows with `-connect=127.0.0.1 -port=2302`, so there is no
connecting by hand. The client gets `-filePatching`, a profile of its own inside the working
directory, `-mod=` out of `clientMods` and `-name=SurvivorA`; a client with nothing to connect to
loads `-mission=dayzOffline.<map>` instead. The server gets the same `-mod=` the client does, plus
`-serverMod=` out of `serverMods`, `-config=`, `-profiles=`, `-mission=` and `-world=none`. An empty
list does not become an empty argument but is not passed at all: the game takes an empty `-mod=`
badly.

The lists are the whole answer to what gets loaded: nothing is appended to them along the way. Mods
of the workspace are named there alongside third-party ones, so the order is the one that is
written, and a mod this launch does not want is simply not named. It was once the other way round —
our own mods were appended to the list on their own — and that cost two things at once: naming one
of ours to move it earlier loaded it twice instead of moving it, and there was no way of not loading
a mod the workspace happens to hold. A mod of the workspace is checked more strictly than a
third-party one: what one of ours packs into is known, so "not built" names the missing pbo, while
of a third-party one only the folder can be asked about.

The profile and the mission come from the mod the target belongs to rather than from its neighbours
in the workspace — otherwise a launch would mean different things on different machines. The profile
is layered out of the mod's `Profiles`: `Global`, `Dev` and then `Client` or `Server`, and a server
one takes `Maps\<map>` as well; the mission comes out of `Missions\<Mod>.<map>` with `Global` and
`Dev` laid over it. Both are assembled in the run folder, but beside `game\` rather than inside it:
beside, because the game root has a `Missions` of its own and Windows does not tell it apart from
our `missions` — in one folder the mission would ride into the DayZ installation straight through a
junction. Neither the mod's sources nor the work drive is changed by a launch, still. A layer the
mod does not have is not asked for by anybody.

The profile is the one part of a launch that is read by human eyes: `.RPT`, `.ADM`, whatever a
server mod keeps its configuration in. So its place is the `enfusion.launch.profiles` setting, and
by default that is the run folder. The layout under it is `<Mod>\<role>`, exactly the one the
Workbench plugins use, so pointing the setting at their folder (`P:\Profiles`) leaves one profile
for both tools instead of two that drift apart quietly. Only the profile moves, though: the file
patching root stays in the run folder whatever is set there, because it is a mirror of the whole
game installation, and the work drive is the one place it must not be — pboProject reads what lies
on that drive, and AddonBuilder binarises through `-addon="P:"`, which is to say every config on it
at once. `server.cfg` is taken from the target's mod, and where it is not there, from beside the
`.enf` that owns the `launch` block; the `serverConfig` field points at any path relative to the mod
instead.

Before the start it is checked that everything to be loaded has been built: for our own mods, the
pbo of every addon in `<modsDirectory>\@<Mod>\Addons`, for third-party ones the `@<Mod>` folder
itself. A mod that is not built is named and the launch does not begin — rather than the game coming
up quietly without it while everything that depended on it falls into a script error. A server
target with no `map`, and a missing `server.cfg`, are refused the same way. Like a build, a launch
puts paths out of a `mod.enf` on a command line, so it wants the folder trusted too.

The "second client" button adds one more client to a launch that is already up: the same target, a
profile of its own, a debugger port of its own, `-client2`, a name of its own and a connection to
the same server. Two clients on one machine are two Steam accounts, and Steam holds one signed-in
account per Windows session. So the second one runs inside a Sandboxie box with a Steam of its own.

The name is `-name`, which is also the engine's profile name. Say nothing and both clients are
called `Survivor`, and the second one on the server becomes `Survivor (2)`: two players nobody can
tell apart in an `.ADM` line or over a character's head, when two at once is exactly what the second
client is for. So the first is `SurvivorA` and the second `SurvivorB`; the name belongs to the role
rather than to the developer, so the same target names the same two players on anybody's machine.
`-name` is documented nowhere in DayZ and was found by measurement: a server started with `-name=X`
writes its profile into `Users\X` instead of `Users\Survivor`. The documented `-gamertag`, tried the
same way, does nothing at all.

One thing is wanted from the developer: install Sandboxie-Plus and write the second account's name
into `enfusion.launch.secondAccount`. The extension does the rest. Sandboxie and Steam are looked
for in the registry — as DayZ and DayZ Tools are — and are overridden by `enfusion.sandboxie.path`
and `enfusion.steam.path`. The box is called `steam2` and is not configurable: it is not something
to choose between, it is where the second Steam lives.

On a press: no box, and one is made (`SbieIni set steam2 Enabled y`, after which Sandboxie fills in
its own defaults — templates, recovery folders, the border); `NeverRemove=y` and `AutoDelete=n` are
added to those, and only at creation. That is the protection from deletion, and it is about the
sign-in: what is inside the box is a signed-in Steam, and a box that gets removed is a password and
a Steam Guard code on the next launch. Then: the Steam in the box is not up, and it is started
(`Start.exe /box:steam2 steam.exe -login <account> -silent`), and the launch **waits for the
sign-in** rather than asking for the button to be pressed again. The first time, the wait is as long
as a human takes to type a password; after that Steam signs itself in within ten seconds or so. Once
the account is in, the game starts.

Three things here are not obvious and cost some debugging. First: "is there anything in the box" is
the wrong question — `Start.exe /box:<box> /listpids` counts Sandboxie's own service processes,
which live in a box after anything at all has run in it; so the list of pids is crossed with
`tasklist` by program name. Second: the sign-in is visible from outside the box — Steam writes
`config\loginusers.vdf`, the sandbox keeps it in its own copy of the disk (`<box>\drive\C\...`), and
it is rewritten exactly on a successful sign-in; hence "signed in" means the file names the account
and is newer than the moment the Steam was started — or simply names the account, where enough time
has passed since that start not to doubt it. Third: `Start.exe` without `/wait` hands the program to
Sandboxie's service and exits at once, so the game is started with `/wait` — otherwise the session
reports the second client "gone" a second after it started. And even with `/wait`, `taskkill /T`
does not reach the game: a boxed process is a child of the service rather than of `Start.exe`, so
Stop additionally puts down the game's processes by their pids inside the box. The first client is
not touched by that: it is the same executable, but not in a box.

With no account set, the second client is just one more client: fine for offline, and unable to join
a server the first one is already on.

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
  view/               the Mods panel and the .enf editor on the extension's side: the webview, the messages, the document edits
  webview/            the Mods panel and the .enf form on the browser's side: a tsconfig of its own, DOM instead of Node
schemas/              the JSON schemas of `mod.enf` and `workspace.enf`, registered through jsonValidation
```

The "the domain knows nothing of the host" boundary is held by `no-restricted-imports` in
`eslint.config.mjs` rather than by convention: `src/mods/**` and `src/webview/**` cannot import
`vscode` or the `platform`/`view` layers. The panel has a `src/webview/tsconfig.json` of its own on
top of that — with DOM and without the Node types — so host API does not compile there even by
accident.

