# Regression run: opens every sample with kompressor-cli and checks the SHA-256 of each file inside
# against what samples.tsv says it must hold. "all" and corpus paths are checked against Windows' own
# hashes of the corpus, so they are right by construction. "baseline" samples (formats whose output
# has no independent reference) are checked against tests/baselines/<name>.sha256, which -Update
# writes from Kompressor's current output; review those changes in git before committing them.
#   powershell -File tests\run.ps1 [-Generate] [-Update] [-Filter wtar*] [-Samples dir] [-Cli exe]
# Exit code: the number of failed samples.
param(
    [switch]$Generate,
    [switch]$Update,
    [string]$Filter = '*',
    [string]$Samples = (Join-Path $PSScriptRoot 'samples'),
    [string]$Cli = (Join-Path $PSScriptRoot '..\build\windows\kompressor-cli.exe')
)
$ErrorActionPreference = 'Continue'
$utf8 = New-Object Text.UTF8Encoding($false)
if (-not (Test-Path $Cli)) { throw "$Cli not found: build first" }
$Cli = (Resolve-Path $Cli).Path
if ($Generate -or -not (Test-Path (Join-Path $Samples 'corpus.sha256'))) {
    & (Join-Path $PSScriptRoot 'make-samples.ps1') -Out $Samples -Cli $Cli
}
$Samples = (Resolve-Path $Samples).Path
$baselines = Join-Path $PSScriptRoot 'baselines'
$lists = Join-Path $Samples '.lists'
New-Item -ItemType Directory -Force $lists, $baselines | Out-Null

# corpus path -> hash
$truth = @{}
foreach ($l in [IO.File]::ReadAllLines((Join-Path $Samples 'corpus.sha256'), $utf8)) {
    if ($l -match '^([0-9a-f]{64}) \*(.+)$') { $truth[$Matches[2]] = $Matches[1] }
}

$rows = Get-Content (Join-Path $PSScriptRoot 'samples.tsv') -Encoding UTF8 | Where-Object { $_ -and -not $_.StartsWith('#') } | ForEach-Object {
    $c = $_ -split "`t"
    [pscustomobject]@{ Name = $c[0]; Expect = $c[1] }
}
$known = @{}
$rows | ForEach-Object { $known[$_.Name] = $true }

$pass = 0; $fail = 0; $skip = 0; $new = 0
$t0 = Get-Date
foreach ($r in $rows) {
    if ($r.Name -notlike $Filter) { continue }
    $file = Join-Path $Samples $r.Name
    if (-not (Test-Path -LiteralPath $file)) {
        '  SKIP  {0,-24} (its writer is not installed here)' -f $r.Name
        $skip++
        continue
    }
    $pwArgs = @()
    if ($r.Name -like '*-pw*') { $pwArgs = @('-p', 'kz-test') }

    # The list to check against.
    if ($r.Expect -eq 'baseline') {
        $list = Join-Path $baselines ($r.Name + '.sha256')
        if ($Update -or -not (Test-Path $list)) {
            $out = & $Cli hash $file -a sha256 @pwArgs 2>$null
            if ($LASTEXITCODE -ne 0 -or -not $out) {
                '  FAIL  {0,-24} could not read it to make a baseline' -f $r.Name
                $fail++
                continue
            }
            $had = if (Test-Path $list) { [IO.File]::ReadAllText($list, $utf8) } else { $null }
            [IO.File]::WriteAllLines($list, [string[]]$out, $utf8)
            $now = [IO.File]::ReadAllText($list, $utf8)
            if ($had -ne $now) {
                '  NEW   {0,-24} baseline {1} - check it before committing' -f $r.Name, $(if ($had) { 'changed' } else { 'written' })
                $new++
                continue
            }
        }
    } else {
        $lines = New-Object System.Collections.Generic.List[string]
        if ($r.Expect -like 'all*') {
            $except = @($r.Expect -split ' ' | Where-Object { $_.StartsWith('-') } | ForEach-Object { $_.Substring(1) })
            foreach ($k in $truth.Keys) {
                if (@($except | Where-Object { $k.StartsWith($_) }).Count) { continue }
                $lines.Add(('{0} *{1}' -f $truth[$k], $k))
            }
        } else {
            foreach ($part in $r.Expect -split '\|') {
                $from, $as = $part -split '>'
                if (-not $as) { $as = $from }
                if (-not $truth.ContainsKey($from)) { throw "samples.tsv: $($r.Name) names $from, which the corpus doesn't have" }
                $lines.Add(('{0} *{1}' -f $truth[$from], $as))
            }
        }
        $list = Join-Path $lists ($r.Name + '.sha256')
        [IO.File]::WriteAllLines($list, [string[]]$lines, $utf8)
    }

    $sw = [Diagnostics.Stopwatch]::StartNew()
    $out = @(& $Cli hash $file -c $list @pwArgs 2>&1 | ForEach-Object { "$_" })
    $ms = $sw.ElapsedMilliseconds
    $summary = $out | Where-Object { $_ -match '^(\d+) checked, (\d+) problem\(s\), (\d+) not in the list$' } | Select-Object -Last 1
    $ok = $summary -and $Matches[2] -eq '0' -and $Matches[3] -eq '0' -and [int]$Matches[1] -gt 0
    if ($ok) {
        '  PASS  {0,-24} {1,4} files  {2,6} ms' -f $r.Name, $Matches[1], $ms
        $pass++
    } else {
        '  FAIL  {0,-24} {1}' -f $r.Name, $(if ($summary) { $summary } else { 'could not be read' })
        $out | Where-Object { $_ -notmatch '^OK ' -and $_ -ne $summary } | Select-Object -First 8 | ForEach-Object { "          $_" }
        $fail++
    }
}

# Samples nobody describes (a writer added to make-samples.ps1 but not to samples.tsv).
Get-ChildItem $Samples -File | Where-Object {
    $_.Name -ne 'corpus.sha256' -and -not $known.ContainsKey($_.Name) -and $_.Name -notmatch '\.\d{3}$'
} | ForEach-Object { '  NOTE  {0,-24} is not listed in samples.tsv' -f $_.Name }

''
'{0} passed, {1} failed, {2} skipped{3} in {4:n0} s' -f $pass, $fail, $skip, $(if ($new) { ", $new new baseline(s)" } else { '' }), ((Get-Date) - $t0).TotalSeconds
exit $fail
