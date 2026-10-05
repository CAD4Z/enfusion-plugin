<#
.SYNOPSIS
Builds the extension from a clean copy of a commit, checks it the way CI does, and installs it into
VS Code: the build a developer tries is that commit, not the working tree with whatever else is in it.

.DESCRIPTION
  npm run install:local                                 HEAD
  npm run install:local -- -Ref 1a2b3c4                 another commit
  npm run install:local -- -Paths src/a.ts,test/a.ts    HEAD with these files as they are on disk
  npm run install:local -- -SkipSmoke -SkipInstall      build and check only

The steps, each logged to <temp>/<name>-install-local/logs: a git worktree of the commit; the native
enfusion.exe configured, built, tested and staged as in CI; the text check, types, lint and tests;
the VSIX packaged and held to scripts/package-smoke.ps1; the installed smoke in a clean VS Code
profile; then code --install-extension. The VSIX stays in <temp>/<name>-install-local as
<name>-<commit>-win32-x64.vsix, the last run's only, and nothing is left in the repository folder;
what was installed is written to install-local.json in the git folder, so the next run says what
it replaces.
#>
param(
  [string] $Ref = 'HEAD',
  [string[]] $Paths = @(),
  [switch] $SkipSmoke,
  [switch] $SkipInstall
)

# Native tools write their progress to stderr; a step fails by its exit code, not by that.
$ErrorActionPreference = 'Continue'

$repo    = (git -C $PSScriptRoot rev-parse --show-toplevel).Trim()
$package = Get-Content -Raw -LiteralPath "$repo/package.json" | ConvertFrom-Json

# npm hands "-Paths a,b" over as one string.
$Paths = @($Paths | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne '' })
if ($Paths.Count -gt 0 -and $Ref -ne 'HEAD') {
  throw '-Paths lays files of the working tree over HEAD; build another commit without it'
}

$commit = (git -C $repo rev-parse --short $Ref).Trim()
if ($LASTEXITCODE -ne 0) {
  throw "no commit $Ref"
}

$label = if ($Paths.Count -gt 0) { "$commit-patched" } else { $commit }
$work  = Join-Path ([IO.Path]::GetTempPath()) "$($package.name)-install-local"
$tree  = Join-Path $work 'tree'
$logs  = Join-Path $work 'logs'
$vsix  = Join-Path $work "$($package.name)-$label-win32-x64.vsix"

# The record of the last install sits in the git folder shared by every worktree of this clone.
$gitFolder = (git -C $repo rev-parse --git-common-dir).Trim()
if (-not [IO.Path]::IsPathRooted($gitFolder)) {
  $gitFolder = Join-Path $repo $gitFolder
}
$record = Join-Path $gitFolder 'install-local.json'

<# Deletes the worktree of an earlier or the current run, its junctions first. #>
function Remove-Worktree {
  if (-not (Test-Path -LiteralPath $tree)) {
    return
  }

  # Removing the tree through a junction would delete what it points at in the main checkout.
  $links = @("$tree\node_modules") + @(Get-ChildItem -LiteralPath "$tree\.vscode-test" -Force -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
  foreach ($link in $links) {
    $item = Get-Item -LiteralPath $link -Force -ErrorAction SilentlyContinue
    if ($null -ne $item -and $item.LinkType -eq 'Junction') {
      $item.Delete()
    }
  }

  git -C $repo worktree remove --force $tree *> $null
  if (Test-Path -LiteralPath $tree) {
    $left = Get-ChildItem -LiteralPath $tree -Recurse -Depth 2 -Force -Attributes ReparsePoint -ErrorAction SilentlyContinue
    if ($null -ne $left) {
      throw "a link is still in $tree; remove it by hand"
    }
    Remove-Item -LiteralPath $tree -Recurse -Force
  }
  git -C $repo worktree prune *> $null
}

<# Runs one step in the worktree with its output in a log, and stops the run when it fails. #>
function Invoke-Step([string] $name, [scriptblock] $block) {
  $log     = Join-Path $logs "$name.log"
  $started = Get-Date

  Push-Location -LiteralPath $tree
  try {
    & $block *> $log
  } finally {
    Pop-Location
  }

  if ($LASTEXITCODE -ne 0) {
    throw "$name failed with exit code $LASTEXITCODE, see $log"
  }
  '  {0,-16} ok  {1,4} s' -f $name, [int]((Get-Date) - $started).TotalSeconds
}

<# cmake from PATH, or the one Visual Studio Build Tools carries. #>
function Find-CMake {
  $onPath = Get-Command cmake -ErrorAction SilentlyContinue
  if ($null -ne $onPath) {
    return $onPath.Source
  }

  $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
  if (Test-Path -LiteralPath $vswhere) {
    $found = & $vswhere -latest -products * -find 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' | Select-Object -First 1
    if ($found) {
      return $found
    }
  }

  throw 'cmake was not found: install the C++ CMake tools of Visual Studio Build Tools'
}

<# Lays the -Paths files of the working tree over the worktree; a file deleted on disk is deleted there. #>
function Copy-Paths {
  foreach ($path in $Paths) {
    $source = Join-Path $repo $path
    $target = Join-Path $tree $path

    if (Test-Path -LiteralPath $source -PathType Container) {
      throw "-Paths takes files, not folders: $path"
    }

    if (Test-Path -LiteralPath $source -PathType Leaf) {
      New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
      Copy-Item -LiteralPath $source -Destination $target -Force
    } elseif (Test-Path -LiteralPath $target) {
      Remove-Item -LiteralPath $target -Force
    }
  }
}

if (Test-Path -LiteralPath $record) {
  $previous = Get-Content -Raw -LiteralPath $record | ConvertFrom-Json
  $extra    = if ($previous.paths.Count -gt 0) { " with $($previous.paths -join ', ')" } else { '' }
  "installed now: $($previous.commit)$extra, $($previous.installedAt)"
}
"building $commit$(if ($Paths.Count -gt 0) { " with $($Paths -join ', ')" })"

$cmake         = Find-CMake
$ctest         = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
$vscodeVersion = [regex]::Match((Get-Content -Raw -LiteralPath "$repo/scripts/run-installed-smoke.mjs"), "VSCODE_VERSION = '([^']+)'").Groups[1].Value

Remove-Worktree
if (Test-Path -LiteralPath $logs) {
  Remove-Item -LiteralPath $logs -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $logs | Out-Null

# The package of an earlier run goes: the folder holds the last run's only.
Get-ChildItem -LiteralPath $work -Filter '*.vsix' -File | Remove-Item -Force

git -C $repo worktree add --detach $tree $commit *> (Join-Path $logs 'worktree.log')
if ($LASTEXITCODE -ne 0) {
  throw "git worktree add failed, see $logs\worktree.log"
}

try {
  if ($Paths.Count -gt 0) {
    Copy-Paths
  }

  # Dependencies and the VS Code the smoke runs in come from the main checkout, not a download.
  New-Item -ItemType Junction -Path "$tree\node_modules" -Target "$repo\node_modules" | Out-Null
  New-Item -ItemType Directory -Path "$tree\.vscode-test" | Out-Null
  $archive = "vscode-win32-x64-archive-$vscodeVersion"
  if (Test-Path -LiteralPath "$repo\.vscode-test\$archive") {
    New-Item -ItemType Junction -Path "$tree\.vscode-test\$archive" -Target "$repo\.vscode-test\$archive" | Out-Null
  }

  # The native executable, by the steps of the windows-x64-package job in CI.
  Invoke-Step 'native-configure' { & $cmake -S native -B native/.build -A x64 '-DCMAKE_INSTALL_PREFIX=dist/native/win32-x64' }
  Invoke-Step 'native-build' { & $cmake --build native/.build --config Release --parallel }
  Invoke-Step 'native-test' { & $ctest --test-dir native/.build -C Release --output-on-failure }
  Invoke-Step 'native-stage' { & $cmake --install native/.build --config Release }

  # The extension. The checks are this script's own, so a commit that predates them is held to them too.
  Invoke-Step 'text' { node "$repo/scripts/check-text.mjs" }
  Invoke-Step 'types' { npm run check-types }
  Invoke-Step 'lint' { npm run lint }
  $env:EDDS_TEST_CONVERTER = Join-Path $tree 'native\.build\Release\enfusion.exe'
  Invoke-Step 'test' { npm test }
  $counts = Select-String -LiteralPath (Join-Path $logs 'test.log') -Pattern '^\S+ (tests|pass|fail) (\d+)' | ForEach-Object { "$($_.Matches[0].Groups[1].Value) $($_.Matches[0].Groups[2].Value)" }
  "  {0,-16} {1}" -f '', ($counts -join ', ')

  # The package: allowlist, the tested executable inside it, HDR smoke on that executable.
  Invoke-Step 'bundle' { npm run package }
  Invoke-Step 'package' { powershell -NoProfile -ExecutionPolicy Bypass -File "$repo/scripts/package-smoke.ps1" -Vsix $vsix -NoDependencies }

  if (-not $SkipSmoke) {
    Invoke-Step 'smoke-build' { node esbuild.js --smoke }
    Invoke-Step 'smoke' { node scripts/run-installed-smoke.mjs $vsix }
  }

  "package $vsix"

  if (-not $SkipInstall) {
    Invoke-Step 'install' { code --install-extension $vsix --force }

    $installed = [ordered]@{
      commit      = (git -C $repo rev-parse $commit).Trim()
      paths       = @($Paths)
      vsix        = $vsix
      installedAt = (Get-Date).ToString('s')
    }
    [IO.File]::WriteAllText($record, ($installed | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))
    "installed $($package.publisher).$($package.name) from ${label}: reload the VS Code windows (Developer: Reload Window)"
  }
} finally {
  Remove-Worktree
}
