param(
  [Parameter(Mandatory = $true)]
  [string] $Vsix
)

$ErrorActionPreference = 'Stop'

$expected = @(
  'THIRD-PARTY.md',
  'README.md',
  'package.json',
  'LICENSE',
  'resources/enfusion.svg',
  'schemas/workspace.enf.schema.json',
  'schemas/mod.enf.schema.json',
  'dist/webview.js',
  'dist/webview.css',
  'dist/texture.js',
  'dist/texture.css',
  'dist/texture-conversion.js',
  'dist/texture-conversion.css',
  'dist/texture-batch.js',
  'dist/texture-batch.css',
  'dist/form.js',
  'dist/form.css',
  'dist/extension.js',
  'dist/native/win32-x64/THIRD-PARTY.md',
  'dist/native/win32-x64/NOTICE.txt',
  'dist/native/win32-x64/enfusion.exe'
) | Sort-Object

$listed = @(npx vsce ls | Where-Object { $_ -ne '' } | Sort-Object)
if ($LASTEXITCODE -ne 0) {
  throw 'vsce could not list the package contents'
}
$difference = Compare-Object -ReferenceObject $expected -DifferenceObject $listed
if ($null -ne $difference) {
  $rendered = $difference | Out-String
  throw "package contents differ from the allowlist:`n$rendered"
}

npx vsce package --target win32-x64 --out $Vsix
if ($LASTEXITCODE -ne 0) {
  throw 'vsce could not build the Windows x64 VSIX'
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Vsix))
try {
  $entry = $archive.GetEntry('extension/dist/native/win32-x64/enfusion.exe')
  if ($null -eq $entry) {
    throw 'the packaged VSIX has no native executable'
  }

  $sha = [System.Security.Cryptography.SHA256]::Create()
  try {
    $stream = $entry.Open()
    try {
      $packagedHash = -join ($sha.ComputeHash($stream) | ForEach-Object { $_.ToString('X2') })
    } finally {
      $stream.Dispose()
    }
  } finally {
    $sha.Dispose()
  }

  $stagedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath 'dist/native/win32-x64/enfusion.exe').Hash
  if ($packagedHash -ne $stagedHash) {
    throw 'the VSIX executable is not the staged and tested executable'
  }

  $swizzles = @('TerrainLayerTexture', 'TerrainSuperTexture', 'TerrainNormalSpecular_SYxX',
    'AlphaToRGB', 'SMDIToGS', 'NormalMap_NOHQ', 'NormalMapGA', 'NormalSpecularMapXYZS',
    'AmbientSpecularMapGA')
  foreach ($bundle in @('extension.js', 'texture-conversion.js', 'texture-batch.js')) {
    $entry = $archive.GetEntry("extension/dist/$bundle")
    if ($null -eq $entry) { throw "the packaged VSIX has no $bundle" }
    $reader = New-Object System.IO.StreamReader($entry.Open())
    try { $script = $reader.ReadToEnd() } finally { $reader.Dispose() }
    foreach ($swizzle in $swizzles) {
      if (-not $script.Contains($swizzle)) { throw "$bundle is missing Swizzling=$swizzle" }
    }
  }
} finally {
  $archive.Dispose()
}
