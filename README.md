# Enfusion

A VS Code extension for Enfusion mods: what the Workbench plugins do today, in the editor the code
is written in anyway.

## EDDS preview

On Windows x64, **Enfusion: Open EDDS Preview** opens any local `.edds` as a read-only editor; the
same command is available from the Explorer context menu. It does not require an Enfusion
workspace, a source image, sibling metadata or DayZ Tools. The editor reports the DDS and `ENF1`
container properties and every mip level it actually finds. Modern 32-bit BGRX/BGRA textures can
also be viewed from their real `COPY` or `LZ4` payload as RGBA or as individual R, G, B and A
channels, with an alpha checkerboard, mip selection, zoom and pan. So can every GPU format this
converter produces: R8, R8G8, DXT1, DXT5, BC4, BC5 and BC7, including block padding on a texture
whose size is not a multiple of four and on the small mips at the end of a chain. The channel
buttons follow what the file holds rather than what it was made from, so a single-channel texture
offers R and no G, B or A to look at.

Formats whose pixel decoder is not available are still inspected, but are labelled
`unsupported-format` and are never represented by source pixels or a plausible placeholder.
Standalone preview is read-only. Reconvert becomes available only when the native metadata codec
validates the sibling `.edds.meta`, its declared source exists inside the discovered Enfusion
scope, and that relation owns this exact EDDS. A same-stem image is never guessed as the source.

## Texture conversion

On Windows x64, **Enfusion: Convert Texture to EDDS** is available on local `.png`, `.tga`, `.jpg`
and `.tiff` files when the window contains a discovered `mod.enf` or `workspace.enf`. Those four
are the texture resource classes DayZ Workbench registers, and they are matched however the
extension was typed. `.jpeg` and `.tif` are deliberately not among them: Workbench does not
register those spellings, so a file under one is refused rather than converted into an EDDS this
editor would call registered and Workbench would see no source for.

Each format is accepted in a stated subtype, and refused by name outside it, rather than decoded
on a guess:

| Format | Accepted | Refused by name |
| --- | --- | --- |
| PNG | Non-interlaced 8-bit RGB and RGBA | Every other IHDR, unknown critical chunks |
| TGA | Colour-map type 0, uncompressed true-colour type 2, 24 or 32-bit | RLE, palettes, other descriptors |
| JPG | Baseline sequential (SOF0), 8-bit, Huffman, one scan, greyscale or YCbCr at 1x1, 2x1, 1x2 or 2x2 luma over 1x1 chroma | Progressive, arithmetic, lossless, 12-bit, CMYK/YCCK, an Adobe transform other than YCbCr, a non-identity EXIF orientation |
| TIFF | One page, either byte order, 8-bit samples, chunky, top-left, no predictor, strips, and no/LZW/Deflate/PackBits compression; greyscale BlackIsZero, RGB, or RGB plus one unassociated alpha | Tiles, planar separation, palette, WhiteIsZero, CMYK, YCbCr, 16-bit, predictors, premultiplied alpha, further pages |

Samples are taken as the file stores them. No colour management is applied to any of the four: a
PNG `gAMA`, a JPEG ICC profile and a TIFF ICC profile are all read past, exactly as the first
slice already treated `gAMA`. JPEG's three components are converted with the full-range JFIF
YCbCr matrix and its chroma is upsampled by replication.

JPEG carries no alpha, so it always produces an opaque surface; TIFF alpha comes from the file's
own unassociated extra sample and is never a conversion setting. JPEG is lossy, so its result is
held to the channel mapping, mip behaviour and a bounded difference rather than to equal bytes.

The editor opens before anything in the project is written. It shows the decoded source beside a
result decoded back from a temporary native EDDS, with shared channel, alpha checkerboard, mip,
zoom and pan controls.
Changing `Conversion`, `ConversionQuality`, `FormatCompress`, `CompressTreshold` or `GenerateMips`
rebuilds only that temporary preview; the other visible Workbench fields show the exact fixed
values of this slice and explain why they are locked. The left pane always shows the source as it
is, so a GPU conversion has something to be compared against.

`Conversion` turns the decoded RGBA into a runtime format. Which format each value produces is read
off DayZ's own textures, where a `.edds.meta` recipe sits beside the `.edds` Workbench wrote from
it:

| `Conversion` | Runtime format | Channels decoded back | `ConversionQuality` |
| --- | --- | --- | --- |
| `None` | 32-bit BGRX or BGRA | RGB or RGBA | fixed at 1 |
| `DXTCompression` | BC1 (`DXT1`) or BC3 (`DXT5`) | RGB or RGBA | active |
| `Red` | `R8_UNORM` | R | fixed at 1 |
| `RedHQCompression` | BC4 (`BC4_UNORM`) | R | active |
| `RedGreen` | `R8G8_UNORM` | RG | fixed at 1 |
| `RedGreenHQCompression` | BC5 (`BC5_UNORM`) | RG | active |
| `ColorHQCompression` | BC7 (`BC7_UNORM`) | RGBA | active |
| `HDRCompression` | — | — | refused as unsupported |

`None` and `DXTCompression` each have two branches, and the branch is read off one source image's
own samples rather than off the batch it was selected with. `None` keeps whichever channel layout
the source declared. `DXTCompression` writes BC3 only when a sample is actually below fully opaque:
a source with no alpha channel and a source whose alpha is opaque throughout both become BC1,
because BC3 is twice the size and spends all of it on an alpha block.

`ConversionQuality` is a fraction of one, to three decimals, and defaults to 1. Workbench calls it
the quality of a *compressed* conversion, so an uncompressed one takes the default and refuses any
other value before a preview or a write rather than accepting a number that would change nothing.
Where it is active it buys encoder search: at 1 the block encoders refit their endpoints and BC7
also fits the best of the sixty-four two-subset partitions, and at 0 they take their first fit.
`FormatCompress` is a container and never changes a decoded pixel of the result: the same profile
stored as `COPY` and as `LZ4` decodes to the same bytes.

`HDRCompression` is recognized and refused. It is never replaced with the nearest LDR format.

The primary action says **Convert**, **Reconvert** or **Replace** from the validated ownership
state. It always targets the sibling `.edds`. Inside a prefix root it also writes canonical sibling
`.edds.meta`; elsewhere inside the enclosing Enfusion root it makes a detached EDDS and says that
registration was skipped. Existing registered GUIDs are preserved character-for-character, while
a new registered resource receives a random collision-scanned 64-bit GUID. Source, EDDS and
metadata revisions are captured for the session and checked again by the native worker immediately
before its temporary files are published. A failure leaves the complete old pair or the complete
new pair, never one file from each.

An explicit Explorer multi-selection opens one conversion batch. The file whose context menu was
used is the primary source image and supplies the one complete profile; selection order does not.
The editor lists create/reconvert/replace or refusal per item, renders one active viewport, and
reports per-file and overall progress. Sources sharing a Windows-normalized destination are all
refused without a winner. A selection above that ceiling is refused before the editor offers to run
it. One bundled `edds-convert batch` process owns up to 256 jobs (including selections of 100) and
spreads them over one bounded worker pool with one shared memory budget, so a large selection keeps
the cores busy without a codec pool or a memory peak per image. It keeps completed outputs on
cancellation, and lets retryable failures be retried without rerunning successes; an input the
converter refuses identically every time is not offered as retryable work.

The public native CLI can convert any of those four formats to any explicit output path without a
project gate; an unregistered extension is refused there too, with a stable `unsupported-source-extension`.
It accepts the same stable recipe flags used by the editor; `inspect --metadata PATH` returns the
metadata identity and recipe as versioned structured JSON through the same native codec.

The platform-specific VSIX carries its own C17 `edds-convert.exe`. The extension only starts that
binary from its installation directory, passes arguments without a shell and checks protocol
compatibility before asking it to inspect a file. The executable has no DayZ Tools, language
runtime or dynamically loaded codec dependency.

The Activity Bar gains an **Enfusion** container with a **Mods** panel. Along its top are two lists
saying what the next launch is — which **target**, and whether it is a **Debug** or a **Release**
build of the game — and under them a row of buttons for everything at once: **Start** puts the game
up, **Build** builds the workspace, and the three square ones on the right mount the work drive,
unmount it, and link the mods onto it. A button that would only fail is disabled and says why in its
tooltip, so the reason is there before the press rather than after it. Below the row is
`workspace.enf` and the mods under it, each mod with its addons in the order they will be built.

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
`mod.cpp`'s business, and the title there can be anything at all. A mod name consists of letters,
digits and underscores and starts with a letter or underscore; Windows device names such as `CON`
and `LPT1` are excluded. An invalid value is shown as written and underlined in its manifest, but no
work-drive, build or launch path is made from it.

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

The paths to DayZ, to DayZ Server, to DayZ Tools and to `pboProject.exe`, the diag executable to
run, the private key, the source and the letter of the work drive, the file patching root and the
choice of builder are about the machine rather than about the mod, so they live in VS Code settings
with `scope: machine`, which the editor physically will not let a workspace write. In the ordinary
case nothing has to be entered at all. The paths are read out of the registry, where the installers
wrote them; DayZ and DayZ Tools are looked for through Steam as well — by its own list of libraries
and by the app manifest — and that is what covers a registry path that now leads nowhere: a game
moved to another library, a key written by another user. DayZ Server is the one that has no registry
key at all, so Steam is the whole of the answer for it. The work drive source defaults to the
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

The lower **Workspace tools** group keeps one contextual drive button: it says **Mount** while the
letter is free and **Unmount** while anything is mounted there. For the usual
`P:`, they call the helper shipped with DayZ Tools, which asks for elevation and makes the drive
visible to both ordinary and elevated tools; without that helper, and for a nonstandard letter,
they fall back to `subst`. The folder and letter come from the machine settings. A drive mounted
somewhere other than what is configured is a refusal with both folders in the tooltip, rather than
a quiet build of the wrong sources. **Link mods** lays junctions across the root of the drive
onto the prefix roots of every mod of the workspace — what `SetupWorkdrive.bat` used to do: a
junction already pointing where it should is not an error and is not repointed, one pointing
elsewhere is repointed, and a real folder in its place is left untouched and shown as it is. A mod
that is not linked is marked in the list, so the reason a build would fail is visible beforehand;
a linked one is marked with nothing, which is how it should be. Unpacking the vanilla data and
setting the drive up in the first place with DayZ Tools is not part of this. **Workbench** opens
the `.gproj` inside the selected launch target's mod (preferring `dayz.gproj`) with the Workbench
installed by DayZ Tools; it stays disabled when the work drive or project is unavailable.

What is built is an **addon** rather than a mod: every addon in the list has a **Build** button of
its own, and the square **Build** at the top builds everything the workspace turned out to hold — in
the dependency order out of `requiredAddons`, the same order the addons are listed in. The button is
unconditional: staleness is not tracked, and the extension will not argue about whether a rebuild is
needed.

Builds still run one at a time, but a press is not refused: it either starts a build or lines up
behind the one that is running. Two builds of one workspace together are not allowed — they do not
share the work, they fight over one pbo, one taking the file while the other writes it — but a
second press has something to say: a minute of building is long enough for the sources to change,
and whoever pressed again is asking for what they have become. The queue collapses as it goes. A
press on something already in it adds nothing — that build has not started and will read the sources
for itself — and **Build** at the top swallows the addons queued on their own, because it will build
all of them anyway. So the held key that once put five hundred builders on the machine now costs
exactly none: the collapsing sits at the head of the queue rather than at the process. What is
already running is never swallowed. Cancelling the notification takes the queue with it: whoever
stopped a build did not ask for the next one. A launch, unlike a build, stays one at a time with a
refusal — whether it came from **Start** or **Run and Debug**: two launches put up two servers on one
port and lay two sets of junctions into one run folder, and a second copy of the same game is not
something anybody asked for.

There is one notification for all of it — one per run of the queue, not one per build in it and not
one per mod in a build. It goes up from the press rather than from the builder's first step (reading
the workspace, the machine and the work drive is a wait in itself), names the current step and the
number of presses standing behind it while it runs, and ends in one sentence saying what came out:
`Built CoreMod, NavigationClient and NavigationServer.` A failure names what failed and offers to
open the packing log; cancelling in it cancels the whole run.

There is a story about focus here, and it is also the reason there is only one notification. The
builder runs in a console of its own, one console per addon; each of them takes the focus off the
editor for an instant and hands it straight back. Along with the returning focus, the button that
had been holding it got a press nobody made — one per addon, some fifteen hundredths of a second
after the console appeared. Under the refusal this was invisible (the press was simply turned away),
but with a queue the build began to feed itself and went round and round until it was stopped. So
the panel's button drops the focus as soon as it is clicked with the mouse (a keyboard press keeps
it — that is how the panel is walked), and a press arriving in the first fractions of a second after
the window got its focus back does not count as a press. On the extension's side there is a settling
window on top of that: a press on something that started less than a second ago collapses into it —
the sources do not change in a second, while a press a human makes a second later or more queues up
as usual.

What comes out of a build is a `<modsDirectory>\@<Mod>` folder with the pbos, the signatures,
`mod.cpp` and the public key; `modsDirectory` comes out of the `launch` block of whichever `.enf`
owns that mod, and a relative path is taken from the folder of that file, so that it means the same
thing on any machine.

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

Signing is a separate step through `DSSignFile.exe`, the same for both builders, and it happens
unless `enfusion.signing.enabled` is turned off: a mod is packed to be run somewhere, and a server
that checks signatures takes nothing else. The key is whatever `enfusion.signing.privateKey` names —
`DSCreateKey.exe` in DayZ Tools is what makes a pair — and signing left on with no key named packs
the pbo anyway and says out loud that they went out bare, because the next thing that would ever
notice is somebody else's server turning the mod away. A key with no `DSSignFile.exe` behind it is a
refusal rather than a quietly unsigned pbo. Signing turned off leaves out the public key as well as
the signature: a `.bikey` shipped in `Keys` beside unsigned pbos claims a signing that never
happened.
The packing exclusions come out of `exclude` in `mod.enf` and replace the default list whole;
AddonBuilder is not given them, because `-exclude=` brings it down (1.0.240639) with an
`ArgumentNullException` on any list at all, its own example included. Build errors are read out of
the packing log and reach Problems — on the line of the file of the workspace the builder was
talking about, not on its twin on `P:`; an addon that failed without a place to point at is marked
on its own `config.cpp`. Building is one of the two places where a path out of a `mod.enf` reaches a
command line (the other is launching), so it wants the folder trusted (Workspace Trust).

The game is launched by the editor's own **Run and Debug** — and by the blue **Start** in the row at
the top, which does exactly the same thing: the same configuration resolved the same way, so it puts
up the target that is selected, and asks only when nothing is selected. The configurations are
handed out dynamically from the targets of the `launch` block, so `launch.json` is not needed and is
never created. One written by hand is useless for configuring, too: a debug configuration takes
exactly `type`, `request`, `target` and `build`, and any other field is an error pointing at
`mod.enf`. The target and the build are chosen in the two lists at the top of the panel, shown
together in the status bar, and changed from either or from the palette; `target` in a configuration
is a target's name, and targets of the same name in different mods are told apart as
`<Mod>: <Name>`. A configuration that names no `build` uses the chosen one, which is what keeps an
ordinary F5 following the panel rather than pinning a build the day it was written. There are no breakpoints, no stacks and no variables, but **Stop** puts down every
process of the launch along with its children (`taskkill /T`). A launch owns its processes and
debugger listeners as one thing: a failed or cancelled start rolls back everything it acquired,
and closing the session takes the same cleanup path. The session ends when any primary process goes
on its own: a client with no server left has nobody to talk to, and a server nobody connects to any
more would otherwise hang about without a single line in the editor to say it is there.

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
not touched at all. A launch refuses to start where the work drive is not mounted, or where the
program it would start is not there.

Which program that is, is what **Debug** and **Release** decide. Debug is everything above: one
`DayZDiag_x64.exe` playing both parts, started in the mirror, given `-filePatching`,
`-scriptDebug=true` and `-newErrorsAreWarnings=1`, reading the mods off the sources. The name or the
path of that one executable is changed by the `enfusion.dayz.executable` setting, for a diag build
kept outside the stable installation. Experimental targets always take `DayZDiag_x64.exe` from
their separate installation so that an absolute stable override cannot silently defeat the
checkbox. Release is the pair a player and a host run: `DayZ_x64.exe` out of
the DayZ folder for the client, `DayZServer_x64.exe` out of the DayZ Server folder for the server,
each started where it is installed and given none of those three arguments — no mirror is built for
it at all, because a retail game reads its mods out of the packed pbo and out of nothing else. Which
is the whole point of having the choice: Debug is what a mod is written under, and Release is the
only launch that says whether what was packed is the same mod. The script log goes with the
debugger, so a Release console carries what the processes print and no `SCRIPT` channel.

DayZ Server is a Steam application of its own — installed beside DayZ rather than inside it, and
recording nothing in the registry the way the client does. So it is found through Steam's own list
of libraries, the folder beside DayZ is what is settled for, and `enfusion.dayzServer.path` names it
outright on a machine where neither answer is right. A Release launch of a client-only target never
asks about it: the refusal is only ever about a program that launch would actually start.

A target's **Experimental** checkbox switches that target as a whole to the separate DayZ
Experimental applications: Steam app 1024020 for the client and diag executable, and app 1042420
for the release server. Their manifests are found across Steam's libraries automatically;
`enfusion.dayzExperimental.path` and `enfusion.dayzExperimentalServer.path` are the machine-local
overrides. Debug still means `DayZDiag_x64.exe` with file patching and Release still means the
retail pair, but both now come from the Experimental installation for that target.

BattlEye is left to the game. The retail client is started directly, the way DayZ's own launcher
starts it with the BattlEye tick off, and neither side is given anything about it. There is nothing
else to give: the server has no switch for it, and three ways round it were measured and all three
failed. `-BEpath` names where BattlEye lives rather than whether it runs, and both processes deploy
their own copies into whatever folder it names; a file where that folder would go hangs the server
six seconds into its start; and `battlEye = 0` in `serverDZ.cfg` changes nothing, because there is
no such configuration key — the only `battleye` the server executable knows is the tag it reports
to the server browser, beside `privHive` and `no3rd`. Both sides deploy and update their own
BattlEye themselves, and a client whose BattlEye has just updated itself asks to be started again
before it will let one play: if a Release client is thrown off a moment after joining for
`BattlEye: Game Restart Required`, the second launch is the one that goes through.

A target says what to put up: a client, the server alone, or both at once. Both is one launch: the
server starts first, the client follows with `-connect=127.0.0.1 -port=2302`, so there is no
connecting by hand. The client gets `-filePatching`, a profile of its own inside the working
directory, `-mod=` out of `mods` and `-name=SurvivorA`; a client with nothing to connect to
loads `-mission=dayzOffline.<map>` instead. The server gets the same `-mod=` out of `mods`,
plus `-serverMod=` out of `serverMods`, `-config=`, `-profiles=`, `-mission=` and `-world=none`.
Name shared mods once in `mods`; put additional mods that only the server needs in `serverMods`.
An empty list is not passed at all: the game takes an empty `-mod=` badly.

For manifests written before 0.0.28, rename `clientMods` to `mods` and remove shared entries
from `serverMods`, leaving only the additional server mods there.

The lists are the whole answer to what gets loaded: nothing is appended to them along the way. Mods
of the workspace are named there alongside third-party ones, so the order is the one that is
written, and a mod this launch does not want is simply not named. It was once the other way round —
our own mods were appended to the list on their own — and that cost two things at once: naming one
of ours to move it earlier loaded it twice instead of moving it, and there was no way of not loading
a mod the workspace happens to hold. A mod of the workspace is checked more strictly than a
third-party one: what one of ours packs into is known, so "not built" names the missing pbo, while
of a third-party one only the folder can be asked about.

A client comes up borderless over the whole primary monitor, and it does so because the game is
asked to rather than because its window is moved about afterwards. DayZ has a borderless mode that
its options menu never names: windowed at exactly the desktop's resolution, it makes a popup and
maximises it (the engine logs it as `bordeless`). `-window`, which clients used to be given, forces
the other windowed mode — a title bar, placed wherever the last run left it, and the bottom of a
monitor-sized picture off the screen — so it is no longer passed. Before each client starts, its
`Users\<Windows account>\DayZ.cfg` in the profile (`DayZ Exp.cfg` for Experimental) is made to say
`Windowed=1`, `WinX=0` and `WinY=0`, and loses `WindowWidth` and `WindowHeight`: without them the
game takes the primary display's own resolution, which is what selects the borderless mode, and no
monitor or scale has to be known here. Nothing else in the file changes. A file that is not there
yet is written too, because without one the game starts in exclusive fullscreen. The second client
reads its settings through its box, so the box's own copy is put right as well once the game has
made one. None of this is documented; it was read out of the game.

For the first seconds of each client's window a small guard runs beside it, because the game shows
its new window on top of everything and asks for the foreground, and Windows sometimes grants it —
which is a loading screen holding the mouse, or at least a game over the editor. The guard holds
the foreground from before the game starts until its window is up, hands the foreground back to
the window the developer was in if the game took it anyway, and puts that window back above the
game's. After that it is gone: the game comes up behind whatever the developer was doing, with its
taskbar button flashing, and is switched to like any other window. It is PowerShell with a little
C# compiled on the spot, killed with the game, and what it did is logged as `client window: ...`.

`mods` and `serverMods` may also be written on an individual target. An absent target list
inherits the list on `launch`; a present target list replaces it whole. That distinction includes
an explicit empty list: `mods: []` clears shared mods for both processes, and `serverMods: []`
clears only the additional server mods. The form shows empty target rows until a target actually
overrides them, while the shared lists remain the default for every other target.

Each `mods` or `serverMods` entry names exactly one folder directly under `modsDirectory`.
Spaces and hyphens are allowed there because these are folder references rather than class names;
path separators, Windows path punctuation and semicolons are refused before a path is constructed.

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

The "second client" button adds one more client to a launch that is already up: the same target and
the same build, a profile of its own, a debugger port of its own, `-client2`, a name of its own and
a connection to the same server. Only one may be starting or running at a time. If it leaves, its listener is closed
and the primary launch stays up, so the button can add a replacement. Two clients on one machine
are two Steam accounts, and Steam holds one signed-in account per Windows session. So the second
one runs inside a Sandboxie box with a Steam of its own.

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
(`Start.exe /box:steam2 steam.exe -login <account> -silent -inhibitbootstrap`). The boxed and
unboxed Steams share one installation, so the boxed one must not try to update files held by the
unboxed one: the failed update otherwise falls into a full checksum pass and rollback. The launch
then **waits for the final client and its sign-in** rather than asking for the button to be pressed
again. The first time, the wait is as long as a human takes to type a password; after that Steam
signs itself in within ten seconds or so. Only then does the game start.

Three things here are not obvious and cost some debugging. First: "is there anything in the box" is
the wrong question — `Start.exe /box:<box> /listpids` counts Sandboxie's own service processes,
which live in a box after anything at all has run in it; so the list of pids is crossed with
`tasklist` by program name. Second: the sign-in is visible from outside the box — Steam writes
`config\loginusers.vdf`, and the sandbox keeps it in its own copy of the disk
(`<box>\drive\C\...`). That file only remembers which accounts belong here; it does not mean
that the current Steam is ready. What it is good for is the account's Steam3 id, taken from the
SteamID it is filed under, because that id is what the connection log names. Readiness therefore
needs three facts: the final client's `steamwebhelper.exe` is in the box, the latest connection
state is `[Logged On, ...] [U:1:<id>] RecvMsgClientLogOnResponse() : processing complete`, and that
`<id>` belongs to the requested account — a box that remembers two accounts and signed the other
one in is not ready for this launch. The line's timestamp must not predate the current boxed
`steam.exe` process, so a success left by the preceding process cannot release the game.
The first sign-in can leave the boxed `loginusers.vdf` absent or unchanged. In that case the
account's SteamID is read from the boxed `config\config.vdf`, under
`InstallConfigStore/Software/Valve/Steam/Accounts/<account>`. Both files establish identity only;
the same process and live-connection checks still apply.
Both `logs\connection_log.previous.txt` and `connection_log.txt` are read because Steam rotates the
live log while it is running. Third: `Start.exe` without `/wait` hands the program to Sandboxie's
service and exits at once, so the game is started with `/wait` — otherwise the session reports the
second client "gone" a second after it started. And even with `/wait`, `taskkill /T` does not reach
the game: a boxed process is a child of the service rather than of `Start.exe`, so Stop
additionally puts down the game's processes by their pids inside the box. The first client is not
touched by that: it is the same executable, but not in a box.

`-inhibitbootstrap` prevents the failed updater on subsequent starts; it cannot remove files an
older failed attempt has already copied into the box. If such a box still cannot reach "Steam is
ready", close its Steam and empty/recreate `steam2` once in Sandboxie. That recovery deliberately
is not automatic because it also removes the remembered login and requires Steam Guard again.

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
| `npm run check-types` | `tsc --noEmit` over the host, browser and test projects (esbuild only transpiles, it checks no types) |
| `npm run lint` | ESLint with type checking |
| `npm test` | builds `*.test.ts` through esbuild and runs `node --test`, with no extension host |
| `npm run vsix` | build the `.vsix` |
| `cmake -S native -B native/.build -A x64` | configure the C17 EDDS converter with MSVC |
| `cmake --build native/.build --config Release` | build the native converter with the static CRT |
| `ctest --test-dir native/.build -C Release` | run native core and black-box CLI tests |

`F5` puts up an Extension Development Host and opens the folder one level above this one in it, so
that the panel has some mods to show straight away.

## Layout

```
src/
  extension.ts        composition root: everything is made and disposed here
  mods/               the domain: the model of the mods and the config.cpp parsing, with no vscode
  platform/           access to the workspace: findFiles, reading files, the watcher, Uri
  view/               the Mods panel and the .enf editor on the extension's side: the webview, the messages, the document edits
  webview/            the Mods panel and the .enf form on the browser's side: a tsconfig of its own, DOM instead of Node
test/
  mods/               bare-Node domain tests, mirroring src/mods
  platform/           host adapters whose resource ownership can be exercised on bare Node
  webview/            browser-side logic that needs no DOM or extension host
  schemas.test.ts     the contract shared by the two JSON schemas
schemas/              the JSON schemas of `mod.enf` and `workspace.enf`, registered through jsonValidation
native/               dependency-free C17 EDDS parser/decoder, CLI, synthetic tests and fuzz target
dist/native/           staged platform executable and notices; produced by CI, not kept in git
```

The "the domain knows nothing of the host" boundary is held by `no-restricted-imports` in
`eslint.config.mjs` rather than by convention: `src/mods/**`, `src/webview/**` and their mirrored
tests cannot import `vscode` or the `platform`/`view` layers. The panel and the tests each have a
tsconfig of their own on top of that: the panel gets DOM without Node, while tests stay plain Node
programs outside the production tree.

