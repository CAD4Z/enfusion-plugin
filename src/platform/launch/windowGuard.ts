/**
 * The first seconds of a client's window: kept from taking the foreground, and from coming up in
 * front of whatever the developer is working in.
 *
 * The game makes its window and at once shows it on top of everything, maximises it and asks for
 * the foreground (`SetWindowPos` without `SWP_NOACTIVATE`, `ShowWindow(SW_MAXIMIZE)`,
 * `SetForegroundWindow`, one after the other). Whether Windows lets it have the foreground depends
 * on who had input last and when, which is why it sometimes did and sometimes did not: and when it
 * does, the game captures the mouse for a loading screen. When it does not, the window still comes
 * up on top, over the editor, which keeps the keyboard while the monitor shows the game. Past those
 * first moments the game never asks again — it only raises itself when it is already active.
 *
 * So a guard is put up beside each client for exactly that moment, and it does three things:
 *
 * - It holds the foreground (`LockSetForegroundWindow`) from before the game starts until the game
 *   window is up. Windows lets it do that under the same conditions it would let the game take the
 *   foreground, so the lock is there precisely when it is needed; anything the developer does —
 *   a click, Alt — releases it, as Windows always does.
 * - A game window that took the foreground anyway in its first moments gets it taken back, for the
 *   window it was taken from; one that keeps it is minimised, which needs no permission at all.
 * - A game window that came up above the window being worked in has that window put back above it.
 *   The developer's window is the one moved, because the game's thread is busy creating its device
 *   and a request to move the game's window would wait for it.
 *
 * The game window is told by its program and by having started after the guard: the server's console
 * and the other client are the same program and were there before — the server started a moment
 * ago included, which is why the processes already running are noted by id rather than judged by
 * a start time the clocks only agree on to within a margin. It is looked for by polling,
 * which costs nothing measurable for the few seconds it runs. A game that never makes a window is
 * given two minutes; either way the guard is put down with the game.
 *
 * It is PowerShell with the Win32 part compiled from C# on the spot, which is ready in about 300 ms —
 * long before the game makes a window. Everything it does is a courtesy around a launch that has
 * already happened: if it cannot run, the game comes up the way it would have anyway.
 */

import { type ChildProcess, spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { readFileSync, renameSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { createInterface } from 'node:readline';

/** A guard that is up; putting it down twice is not an error. */
export interface WindowGuard {
  stop(): void;
}

/** How long the game has to make a window at all. */
const WAIT = 120_000;

/** How long after it is up the game taking the foreground is the game's doing rather than a click. */
const STEAL = 1_500;

/** How long after it is up the game may still move itself on top: the whole of its window setup. */
const SETTLE = 4_000;

/**
 * Processes started this long before the guard still count as the game's: the clock the launch
 * reads and the one Windows stamps a process with are the same clock read at different moments.
 */
const MARGIN = 1_000;

/**
 * Puts a guard up for the next window of `program` (a file name, `DayZDiag_x64.exe`), before the
 * game is started. What it does is said a line at a time.
 */
export function guardGameWindow(program: string, said: (line: string) => void): WindowGuard {
  let child: ChildProcess;
  try {
    const script = scriptOf(program, Date.now() - MARGIN, guardSourceFile());
    child = spawn('powershell.exe', ['-NoProfile', '-NonInteractive', '-EncodedCommand', encoded(script)], {
      stdio: ['ignore', 'pipe', 'ignore'],
      windowsHide: true,
    });
  } catch {
    return { stop: () => undefined };
  }

  child.on('error', () => undefined);
  if (child.stdout !== null) {
    createInterface({ input: child.stdout, crlfDelay: Infinity }).on('line', (line) => {
      if (line.trim() !== '') {
        said(line.trim());
      }
    });
  }
  child.unref();

  return {
    stop: () => {
      // Killing it is also what lets go of the foreground: Windows drops the lock with the process.
      if (child.exitCode === null && child.signalCode === null) {
        child.kill();
      }
    },
  };
}

/** The script, as `-EncodedCommand` takes it: UTF-16LE in base64, so no quoting can break it. */
export function encoded(script: string): string {
  return Buffer.from(script, 'utf16le').toString('base64');
}

/**
 * The whole of what the guard runs; the numbers are the ones above. The C# is compiled out of a
 * file rather than carried on the command line: encoded, it would come within a few hundred
 * characters of the most Windows takes on one.
 */
export function scriptOf(program: string, startedAfter: number, source: string): string {
  return [
    "$ErrorActionPreference = 'Stop'",
    "$ProgressPreference = 'SilentlyContinue'",
    '[Console]::OutputEncoding = [Text.Encoding]::UTF8',
    `Add-Type -Path ${quoted(source)}`,
    `[EnfusionWindowGuard]::Run(${quoted(program)}, ${Math.floor(startedAfter)}, ${WAIT}, ${STEAL}, ${SETTLE})`,
  ].join('\n');
}

/** A PowerShell literal string: nothing in it is expanded, and a quotation mark is doubled. */
function quoted(text: string): string {
  return `'${text.replace(/'/g, "''")}'`;
}

/**
 * The C# half, in the temporary folder under a name that is its own hash, so that two versions of
 * the extension never compile each other's. Written when it is not there as it should be — the
 * folder is anybody's to clean — and written whole or not at all, since a guard of another window
 * may be compiling it at that moment.
 */
export function guardSourceFile(folder: string = tmpdir()): string {
  const hash = createHash('sha256').update(GUARD_SOURCE).digest('hex').slice(0, 16);
  const path = join(folder, `enfusion-window-guard-${hash}.cs`);

  let present = '';
  try {
    present = readFileSync(path, 'utf8');
  } catch {
    // Not there yet, which is the first launch after a restart of the machine or of the extension.
  }

  if (present !== GUARD_SOURCE) {
    const partial = `${path}.${process.pid}.tmp`;
    writeFileSync(partial, GUARD_SOURCE, 'utf8');
    renameSync(partial, path);
  }

  return path;
}

/** The Win32 half of the guard, compiled by PowerShell's `Add-Type` each time it is put up. */
export const GUARD_SOURCE = String.raw`using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

public static class EnfusionWindowGuard
{
    [DllImport("user32.dll")] static extern bool LockSetForegroundWindow(uint code);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] static extern bool ShowWindowAsync(IntPtr window, int command);
    [DllImport("user32.dll")] static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] static extern IntPtr GetTopWindow(IntPtr parent);
    [DllImport("user32.dll")] static extern IntPtr GetWindow(IntPtr window, uint command);
    [DllImport("user32.dll")] static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr window);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongW")] static extern int GetWindowLong(IntPtr window, int index);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr window, StringBuilder name, int size);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr window, StringBuilder text, int size);
    [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, uint process);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll")] static extern bool GetProcessTimes(IntPtr process, out long created, out long exited, out long kernel, out long user);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] static extern bool QueryFullProcessImageNameW(IntPtr process, uint flags, StringBuilder name, ref uint size);

    const uint LSFW_LOCK = 1, LSFW_UNLOCK = 2, QUERY_LIMITED = 0x1000;
    const uint GW_HWNDNEXT = 2, GW_HWNDPREV = 3, GW_OWNER = 4;
    const uint SWP_NOSIZE = 0x1, SWP_NOMOVE = 0x2, SWP_NOACTIVATE = 0x10, SWP_ASYNCWINDOWPOS = 0x4000;
    const int GWL_EXSTYLE = -20, WS_EX_TOPMOST = 0x8, SW_MINIMIZE = 6, POLL = 20;
    const long UNIX_EPOCH_AS_FILETIME = 116444736000000000, GIVE_BACK_PATIENCE = 250;
    const string DIALOG = "#32770";
    const int NOT_GAME = 0, GAME = 1, EARLIER_GAME = 2, UNKNOWN_AGE = 3;

    static string image;
    static long startedAfter;
    static HashSet<IntPtr> before;
    // The same program already running when the guard came up: the server this launch started a
    // moment ago is inside the clock's margin, and its window is not the client's.
    static HashSet<int> earlier;
    static readonly Dictionary<uint, int> kinds = new Dictionary<uint, int>();
    static readonly HashSet<string> said = new HashSet<string>();
    static readonly HashSet<string> shell = new HashSet<string>(StringComparer.Ordinal)
    {
        "Progman", "WorkerW", "Shell_TrayWnd", "Shell_SecondaryTrayWnd", "NotifyIconOverflowWindow",
        "MultitaskingViewFrame", "XamlExplorerHostIslandWindow", "ForegroundStaging", "#32768",
    };

    public static void Run(string program, long startedAfterUnixMs, int waitMs, int stealMs, int settleMs)
    {
        image = program;
        startedAfter = startedAfterUnixMs;
        before = new HashSet<IntPtr>(TopLevel());
        earlier = new HashSet<int>();
        foreach (var running in Process.GetProcessesByName(Path.GetFileNameWithoutExtension(program)))
        {
            earlier.Add(running.Id);
        }

        bool held = LockSetForegroundWindow(LSFW_LOCK);
        Say(held ? "holding the foreground until the game window is up" : "the foreground cannot be held");

        try
        {
            held = Guard(waitMs, stealMs, settleMs, held);
        }
        catch (Exception error)
        {
            Say("the guard stopped: " + error.GetType().Name + ": " + error.Message);
        }
        finally
        {
            if (held)
            {
                LockSetForegroundWindow(LSFW_UNLOCK);
            }
        }
    }

    static bool Guard(int waitMs, int stealMs, int settleMs, bool held)
    {
        IntPtr working = Working(GetForegroundWindow(), IntPtr.Zero);
        var born = new Dictionary<IntPtr, long>();
        var asked = new Dictionary<IntPtr, long>();
        var clock = Stopwatch.StartNew();
        long raised = -1000;

        while (true)
        {
            long now = clock.ElapsedMilliseconds;
            foreach (IntPtr window in TopLevel())
            {
                if (!born.ContainsKey(window) && IsGameWindow(window))
                {
                    born[window] = now;
                    Say("the game window " + Hex(window) + " is up");
                }
            }

            IntPtr front = GetForegroundWindow();
            working = Working(front, working);

            bool young = false;
            foreach (KeyValuePair<IntPtr, long> birth in born)
            {
                IntPtr window = birth.Key;
                long age = now - birth.Value;
                if (age > settleMs || !IsWindow(window))
                {
                    continue;
                }

                young = true;
                if (window == front)
                {
                    if (age <= stealMs)
                    {
                        held = GiveBack(window, working, held, asked, now);
                    }
                    continue;
                }

                if (working != IntPtr.Zero && now - raised >= 50 && IsAbove(window, working))
                {
                    SetWindowPos(working, IntPtr.Zero, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
                    raised = now;
                    SayOnce("kept the game window behind " + Title(working));
                }
            }

            if (born.Count > 0 && !young)
            {
                break;
            }

            if (born.Count == 0 && now > waitMs)
            {
                Say("no game window came up");
                break;
            }

            Thread.Sleep(POLL);
        }

        return held;
    }

    // The switch lands when the other window's thread gets to it, so the game is looked at again a
    // moment later, and one still in front then is minimized: the one move that needs no permission.
    static bool GiveBack(IntPtr window, IntPtr working, bool held, Dictionary<IntPtr, long> asked, long now)
    {
        long when;
        if (!asked.TryGetValue(window, out when))
        {
            if (held)
            {
                LockSetForegroundWindow(LSFW_UNLOCK);
            }

            bool sent = working != IntPtr.Zero && IsWindow(working) && SetForegroundWindow(working);
            asked[window] = sent ? now : now - GIVE_BACK_PATIENCE;
            SayOnce(sent ? "the game took the foreground; gave it back to " + Title(working) : "the game took the foreground");
            return LockSetForegroundWindow(LSFW_LOCK);
        }

        if (now - when >= GIVE_BACK_PATIENCE)
        {
            ShowWindowAsync(window, SW_MINIMIZE);
            SayOnce("minimized the game window: it kept the foreground");
        }

        return held;
    }

    // The foreground window, unless that is the game or no place to work; then the one under it.
    static IntPtr Working(IntPtr front, IntPtr current)
    {
        if (Usable(front))
        {
            return front;
        }

        if (front == IntPtr.Zero || !IsGameWindow(front) || (current != IntPtr.Zero && IsWindow(current)))
        {
            return current;
        }

        for (IntPtr below = GetWindow(front, GW_HWNDNEXT); below != IntPtr.Zero; below = GetWindow(below, GW_HWNDNEXT))
        {
            if (GetWindow(below, GW_OWNER) == IntPtr.Zero && Usable(below))
            {
                return below;
            }
        }

        return current;
    }

    // A client that was up already is a place to work; a dialog of the game's program is not.
    static bool Usable(IntPtr window)
    {
        if (window == IntPtr.Zero || !IsWindowVisible(window) || IsIconic(window) ||
            (GetWindowLong(window, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0)
        {
            return false;
        }

        string name = ClassOf(window);
        return !shell.Contains(name) && !IsGameWindow(window) && !(name == DIALOG && KindOfWindow(window) != NOT_GAME);
    }

    // A main window of the game started after the guard. A process that cannot be asked its age is
    // judged by its name and by the window being new since the guard came up.
    static bool IsGameWindow(IntPtr window)
    {
        if (window == IntPtr.Zero || ClassOf(window) == DIALOG)
        {
            return false;
        }

        int kind = KindOfWindow(window);
        return kind == GAME || (kind == UNKNOWN_AGE && !before.Contains(window));
    }

    static int KindOfWindow(IntPtr window)
    {
        uint process;
        GetWindowThreadProcessId(window, out process);
        int kind;
        if (!kinds.TryGetValue(process, out kind))
        {
            kind = KindOf(process);
            kinds[process] = kind;
        }

        return kind;
    }

    static int KindOf(uint process)
    {
        if (earlier.Contains((int)process))
        {
            return EARLIER_GAME;
        }

        IntPtr handle = OpenProcess(QUERY_LIMITED, false, process);
        if (handle == IntPtr.Zero)
        {
            // Protected, gone, or otherwise not to be opened: its name, if even that can be had.
            try
            {
                return string.Equals(Process.GetProcessById((int)process).ProcessName + ".exe", image, StringComparison.OrdinalIgnoreCase) ? UNKNOWN_AGE : NOT_GAME;
            }
            catch (Exception)
            {
                return NOT_GAME;
            }
        }

        try
        {
            var name = new StringBuilder(1024);
            uint size = (uint)name.Capacity;
            if (!QueryFullProcessImageNameW(handle, 0, name, ref size) || !string.Equals(Path.GetFileName(name.ToString()), image, StringComparison.OrdinalIgnoreCase))
            {
                return NOT_GAME;
            }

            long created, exited, kernel, user;
            if (!GetProcessTimes(handle, out created, out exited, out kernel, out user))
            {
                return UNKNOWN_AGE;
            }

            return (created - UNIX_EPOCH_AS_FILETIME) / 10000 >= startedAfter ? GAME : EARLIER_GAME;
        }
        finally
        {
            CloseHandle(handle);
        }
    }

    static List<IntPtr> TopLevel()
    {
        var windows = new List<IntPtr>();
        for (IntPtr window = GetTopWindow(IntPtr.Zero); window != IntPtr.Zero; window = GetWindow(window, GW_HWNDNEXT))
        {
            if (IsWindowVisible(window) && GetWindow(window, GW_OWNER) == IntPtr.Zero)
            {
                windows.Add(window);
            }
        }

        return windows;
    }

    static bool IsAbove(IntPtr window, IntPtr other)
    {
        int steps = 0;
        for (IntPtr above = GetWindow(other, GW_HWNDPREV); above != IntPtr.Zero && steps < 4096; above = GetWindow(above, GW_HWNDPREV), steps++)
        {
            if (above == window)
            {
                return true;
            }
        }

        return false;
    }

    static string ClassOf(IntPtr window)
    {
        var name = new StringBuilder(256);
        GetClassNameW(window, name, name.Capacity);
        return name.ToString();
    }

    static string Title(IntPtr window)
    {
        var text = new StringBuilder(120);
        GetWindowTextW(window, text, text.Capacity);
        return text.Length == 0 ? Hex(window) : "\"" + text + "\"";
    }

    static string Hex(IntPtr window)
    {
        return "0x" + window.ToInt64().ToString("X");
    }

    static void SayOnce(string line)
    {
        if (said.Add(line))
        {
            Say(line);
        }
    }

    static void Say(string line)
    {
        Console.Out.WriteLine(line);
        Console.Out.Flush();
    }
}`;
