# Builds the regression samples: a fixed corpus of files (gen/mkcorpus.py) packed by every writer this
# PC has - Python, .NET, Windows' tar, makecab, Windows' disc imaging (IMAPI2), Kompressor itself and,
# when installed, 7-Zip. A writer that is missing is skipped; run.ps1 reports its samples as SKIP.
# The archives differ from run to run (timestamps), but their contents never do.
#   powershell -File tests\make-samples.ps1 [-Out dir] [-Cli kompressor-cli.exe]
param(
    [string]$Out = (Join-Path $PSScriptRoot 'samples'),
    [string]$Cli = (Join-Path $PSScriptRoot '..\build\windows\kompressor-cli.exe')
)
$ErrorActionPreference = 'Continue'
$gen = Join-Path $PSScriptRoot 'gen'
$pw = 'kz-test'

# Start from an empty folder, but only ever delete one this script made.
if (Test-Path $Out) {
    if (-not (Test-Path (Join-Path $Out 'corpus.sha256')) -and (Get-ChildItem $Out -Force | Select-Object -First 1)) {
        throw "$Out is not empty and was not made by this script; choose another -Out"
    }
    Remove-Item $Out -Recurse -Force
}
New-Item -ItemType Directory -Force $Out | Out-Null
$Out = (Resolve-Path $Out).Path
$corpus = Join-Path $Out 'corpus'

$python = (Get-Command python, py -ErrorAction SilentlyContinue | Select-Object -First 1).Source
if (-not $python) { throw 'Python 3 is needed to write the corpus' }
& $python -I (Join-Path $gen 'mkcorpus.py') $corpus

# The expected answer: Windows' own SHA-256 of every corpus file, as sha256sum would list it.
$lines = Get-ChildItem $corpus -Recurse -File | Sort-Object FullName | ForEach-Object {
    $rel = $_.FullName.Substring($corpus.Length + 1).Replace('\', '/')
    '{0} *{1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower(), $rel
}
[IO.File]::WriteAllLines((Join-Path $Out 'corpus.sha256'), [string[]]$lines, (New-Object Text.UTF8Encoding($false)))
$top = @(Get-ChildItem $corpus | ForEach-Object Name)

function Make([string]$name, [scriptblock]$how) {
    $target = Join-Path $Out $name
    $global:LASTEXITCODE = 0
    $failed = $false
    try { & $how *>&1 | Out-Null } catch { $failed = $true }
    # A writer that fails part way leaves a truncated file behind: that is not a sample.
    if ($failed -or $global:LASTEXITCODE -ne 0) { Remove-Item -LiteralPath $target -Force -ErrorAction SilentlyContinue }
    if (Test-Path -LiteralPath $target) { "  made         $name" } else { "  can't make   $name" }
}

"Python"
& $python -I (Join-Path $gen 'mkpy.py') $corpus $Out | Out-Null
Get-ChildItem $Out -File | Where-Object Name -ne 'corpus.sha256' | ForEach-Object { "  made         $($_.Name)" }

".NET"
Add-Type -AssemblyName System.IO.Compression.FileSystem
Make 'dotnet.zip' { [IO.Compression.ZipFile]::CreateFromDirectory($corpus, (Join-Path $Out 'dotnet.zip')) }

"Windows tar"
$tar = (Get-Command tar.exe -ErrorAction SilentlyContinue).Source
if ($tar) {
    Push-Location $corpus
    $wt = @(
        @('wtar.zip', @('--format', 'zip')),
        @('wtar-store.zip', @('--format', 'zip', '--options', 'zip:compression=store')),
        @('wtar-pw.zip', @('--format', 'zip', '--options', 'zip:encryption=aes256', '--passphrase', $pw)),
        @('wtar-zipcrypto-pw.zip', @('--format', 'zip', '--options', 'zip:encryption=traditional', '--passphrase', $pw)),
        @('wtar.7z', @('--format', '7zip')),
        @('wtar-lzma1.7z', @('--format', '7zip', '--options', '7zip:compression=lzma1')),
        @('wtar-bzip2.7z', @('--format', '7zip', '--options', '7zip:compression=bzip2')),
        @('wtar-deflate.7z', @('--format', '7zip', '--options', '7zip:compression=deflate')),
        @('wtar-ppmd.7z', @('--format', '7zip', '--options', '7zip:compression=ppmd')),
        @('wtar-store.7z', @('--format', '7zip', '--options', '7zip:compression=store')),
        @('wtar.cpio', @('--format', 'newc')),
        @('wtar-odc.cpio', @('--format', 'odc')),
        @('wtar.iso', @('--format', 'iso9660')),
        @('wtar.xar', @('--format', 'xar')),
        @('wtar-bzip2.xar', @('--format', 'xar', '--options', 'xar:compression=bzip2')),
        @('wtar-xz.xar', @('--format', 'xar', '--options', 'xar:compression=xz')),
        @('wtar-lzma.xar', @('--format', 'xar', '--options', 'xar:compression=lzma')),
        @('wtar-none-md5.xar', @('--format', 'xar', '--options', 'xar:compression=none,xar:checksum=md5')),
        @('wtar-gnu.tar', @('--format', 'gnutar')),
        @('wtar.tar.gz', @('--format', 'pax', '-z')),
        @('wtar.tar.bz2', @('--format', 'pax', '-j')),
        @('wtar.tar.xz', @('--format', 'pax', '-J')),
        @('wtar.tar.zst', @('--format', 'pax', '--zstd')),
        @('wtar.tar.lzma', @('--format', 'pax', '--lzma')),
        @('wtar.tar.lz', @('--format', 'pax', '--lzip')),
        @('wtar.tar.Z', @('--format', 'ustar', '-Z'))
    )
    foreach ($w in $wt) {
        $name = $w[0]; $opts = $w[1]
        # Windows' tar crashes on the folder of Unicode names, so it gets everything else.
        Make $name { & $tar @opts -cf (Join-Path $Out $name) @($top | Where-Object { $_ -ne 'unicode' }) }
    }
    Pop-Location
} else { "  tar.exe not found" }

"makecab"
$cabSrc = Join-Path $corpus 'readme.txt'
Make 'cab-mszip.cab' { makecab.exe /D CompressionType=MSZIP $cabSrc (Join-Path $Out 'cab-mszip.cab') }
Make 'cab-lzx.cab' { makecab.exe /D CompressionType=LZX /D CompressionMemory=21 (Join-Path $corpus 'data\log.txt') (Join-Path $Out 'cab-lzx.cab') }
Make 'cab-none.cab' { makecab.exe /D Compress=OFF (Join-Path $corpus 'code\app.exe') (Join-Path $Out 'cab-none.cab') }

"Windows disc imaging"
$disc = Join-Path $gen 'mkdisc.ps1'
Make 'imapi-joliet.iso' { & $disc -Dir $corpus -Out (Join-Path $Out 'imapi-joliet.iso') -Fs 3 }
Make 'imapi-bridge.iso' { & $disc -Dir $corpus -Out (Join-Path $Out 'imapi-bridge.iso') -Fs 7 -Rev 0x102 }
foreach ($r in @(0x102, 0x150, 0x201, 0x250)) {
    $name = 'imapi-udf{0:x}.iso' -f $r
    Make $name { & $disc -Dir $corpus -Out (Join-Path $Out $name) -Fs 4 -Rev $r }
}
if (Test-Path (Join-Path $Out 'imapi-udf102.iso')) {
    Make 'udf.dmg' { & $python -I (Join-Path $gen 'mkdmg.py') (Join-Path $Out 'imapi-udf102.iso') (Join-Path $Out 'udf.dmg') }
}

"Kompressor (round trips through its own writers)"
if (Test-Path $Cli) {
    $items = @(Get-ChildItem $corpus | ForEach-Object FullName)
    foreach ($e in 'zip', '7z', 'rar', 'tar', 'tar.gz', 'tar.xz', 'tar.zst', 'tar.bz2', 'tar.lz4', 'iso', 'cpio') {
        $name = "kz.$e"
        Make $name { & $Cli create (Join-Path $Out $name) @items }
    }
    Make 'kz-pw.zip' { & $Cli create (Join-Path $Out 'kz-pw.zip') @items -p $pw }
    Make 'kz-split.7z.001' { & $Cli create (Join-Path $Out 'kz-split.7z') @items -v 1m }
    foreach ($m in 'fast', 'balanced', 'ultra') {
        $name = "kz-$m.kmp"
        Make $name { & $Cli super (Join-Path $Out $name) @items -m $m }
    }
    foreach ($e in 'gz', 'xz', 'zst', 'bz2', 'lz4') {
        $name = "kz-log.txt.$e"
        Make $name { & $Cli create (Join-Path $Out $name) (Join-Path $corpus 'data\log.txt') }
    }
} else { "  $Cli not found: build first" }

"7-Zip"
$sz = @("$env:ProgramFiles\7-Zip\7z.exe", "${env:ProgramFiles(x86)}\7-Zip\7z.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $sz) { $sz = (Get-Command 7z -ErrorAction SilentlyContinue).Source }
if ($sz) {
    Push-Location $corpus
    $s7 = @(
        @('7z-lzma2.7z', @('-t7z')),
        @('7z-bcj2.7z', @('-t7z', '-mf=BCJ2')),
        @('7z-ppmd.7z', @('-t7z', '-m0=PPMd')),
        @('7z-pw.7z', @('-t7z', "-p$pw", '-mhe=on')),
        @('7z-deflate64.zip', @('-tzip', '-mm=Deflate64')),
        @('7z-ppmd.zip', @('-tzip', '-mm=PPMd')),
        @('7z-aes-pw.zip', @('-tzip', "-p$pw", '-mem=AES256')),
        @('7z.wim', @('-twim'))
    )
    foreach ($s in $s7) {
        $name = $s[0]; $opts = $s[1]
        Make $name { & $sz a -bso0 -bsp0 @opts (Join-Path $Out $name) @top }
    }
    Make '7z-split.7z.001' { & $sz a -bso0 -bsp0 -t7z -v1m (Join-Path $Out '7z-split.7z') @top }
    Pop-Location
} else { "  7-Zip not installed" }
