$ErrorActionPreference = "Stop"

$Exe = Join-Path $PSScriptRoot "hydration.exe"
if (-not (Test-Path $Exe)) {
    $Exe = Join-Path $PSScriptRoot "build\hydration.exe"
}
if (-not (Test-Path $Exe)) {
    $Exe = Join-Path $PSScriptRoot "build\Release\hydration.exe"
}
if (-not (Test-Path $Exe)) {
    throw "Could not find hydration.exe under root, build\, or build\Release\."
}

$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

$CacheFile       = Join-Path $PSScriptRoot "test_cache.json"
$SeedFile        = Join-Path $PSScriptRoot "test_seed.json"
$PageFile        = Join-Path $PSScriptRoot "test_page.html"
$CustomValFile   = Join-Path $PSScriptRoot "custom_validator.ps1"
$MultiValFile    = Join-Path $PSScriptRoot "multiline_validator.ps1"
$ExitValFile     = Join-Path $PSScriptRoot "exitonly_validator.ps1"
$HtmlValFile     = Join-Path $PSScriptRoot "html_validator.ps1"
$CustomCacheFile = Join-Path $PSScriptRoot "custom_cache.txt"
$NestedDir       = Join-Path $PSScriptRoot "nested_dir"

function Assert-Condition {
    param([bool]$Cond, [string]$Msg)
    if (-not $Cond) {
        throw "ASSERTION FAILED: $Msg"
    }
}

function Format-Win32Arguments {
    param([string[]]$ArgsList)
    if ($null -eq $ArgsList -or $ArgsList.Count -eq 0) {
        return ""
    }
    $escaped = foreach ($a in $ArgsList) {
        if ($a -eq "") {
            '""'
        } elseif ($a -notmatch '[\s"]') {
            $a
        } else {
            # Escape backslashes preceding double quotes and double quotes themselves per Win32 CommandLineToArgvW rules
            $e = $a -replace '(\\*)"', '$1$1\"'
            $e = $e -replace '(\\+)$', '$1$1'
            '"' + $e + '"'
        }
    }
    return ($escaped -join ' ')
}

function Invoke-HydrationChild {
    param(
        [string[]]$ArgsList,
        [string]$StdinText = $null,
        [hashtable]$EnvVars = @{ HYD_MANAGED = "1" }
    )

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.WorkingDirectory = $PSScriptRoot
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = $Utf8NoBom
    $psi.StandardErrorEncoding = $Utf8NoBom

    # Clear inherited management vars unless explicitly requested in $EnvVars
    $psi.EnvironmentVariables.Remove("HYD_MANAGED")
    $psi.EnvironmentVariables.Remove("HYD_NO_PARENT")

    foreach ($k in $EnvVars.Keys) {
        $psi.EnvironmentVariables[$k] = $EnvVars[$k]
    }

    $psi.Arguments = Format-Win32Arguments $ArgsList

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    [void]$proc.Start()

    if ($null -ne $StdinText) {
        $bytes = $Utf8NoBom.GetBytes($StdinText)
        $proc.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
    }
    $proc.StandardInput.Close()

    $stdout = $proc.StandardOutput.ReadToEnd()
    $stderr = $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()

    return [PSCustomObject]@{
        ExitCode = $proc.ExitCode
        StdOut   = $stdout
        StdErr   = $stderr
    }
}

function Cleanup-Artifacts {
    Remove-Item $CacheFile, $SeedFile, $PageFile, $CustomValFile, $MultiValFile, $ExitValFile, $HtmlValFile, $CustomCacheFile -Force -ErrorAction SilentlyContinue
    Remove-Item $NestedDir -Recurse -Force -ErrorAction SilentlyContinue
}

Cleanup-Artifacts

try {
    # Test 1: No args -> Usage & exit 1
    $r1 = Invoke-HydrationChild -ArgsList @()
    Write-Host "Test 1 (No args) [EXPECT: FAIL] Exit=$($r1.ExitCode)"
    Assert-Condition ($r1.ExitCode -eq 1 -and $r1.StdErr -match "Usage: hydration") "Test 1 failed"

    # Test 2: Cold cache with piped stdin -> creates cache and emits normalized JSON
    $r2 = Invoke-HydrationChild -ArgsList @('@test_cache.json') -StdinText '[" Beta ", "alpha", "ALPHA"]'
    Write-Host "Test 2 (Cold cache + stdin) [EXPECT: PASS] Exit=$($r2.ExitCode) StdOut=$($r2.StdOut.Trim())"
    Assert-Condition ($r2.ExitCode -eq 0 -and $r2.StdOut.Trim() -eq '["beta","alpha"]' -and (Test-Path $CacheFile)) "Test 2 failed"

    # Test 3: Warm cache hit -> emits cached JSON without needing stdin
    $r3 = Invoke-HydrationChild -ArgsList @('@test_cache.json')
    Write-Host "Test 3 (Warm cache hit) [EXPECT: PASS] Exit=$($r3.ExitCode) StdOut=$($r3.StdOut.Trim())"
    Assert-Condition ($r3.ExitCode -eq 0 -and $r3.StdOut.Trim() -eq '["beta","alpha"]' -and $r3.StdErr -eq "") "Test 3 failed"

    # Test 4: Large dataset (9,000 items) performance & correctness
    $bigItems = 1..9000 | ForEach-Object { "`"item_$_`"" }
    $bigJson = "[" + ($bigItems -join ",") + "]"
    Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $r4 = Invoke-HydrationChild -ArgsList @('@test_cache.json') -StdinText $bigJson
    $sw.Stop()
    Write-Host "Test 4 (9000 items in $($sw.ElapsedMilliseconds)ms) [EXPECT: PASS] Exit=$($r4.ExitCode)"
    Assert-Condition ($r4.ExitCode -eq 0 -and $r4.StdOut.StartsWith('["item_1",')) "Test 4 failed"

    # Test 5: --pretty flag formatting
    Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
    $r5 = Invoke-HydrationChild -ArgsList @('@test_cache.json', '--pretty') -StdinText '["one","two"]'
    Write-Host "Test 5 (--pretty) [EXPECT: PASS] Exit=$($r5.ExitCode)"
    Assert-Condition ($r5.ExitCode -eq 0 -and $r5.StdOut -match "`n  `"one`",") "Test 5 failed"

    # Test 6: Empty stdin on cold cache -> exit 1, file not created
    Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
    $r6 = Invoke-HydrationChild -ArgsList @('@test_cache.json') -StdinText ""
    Write-Host "Test 6 (Empty stdin) [EXPECT: FAIL] Exit=$($r6.ExitCode)"
    Assert-Condition ($r6.ExitCode -eq 1 -and -not (Test-Path $CacheFile)) "Test 6 failed"

    # Test 7: Corrupt existing cache self-heals from valid stdin
    [System.IO.File]::WriteAllText($CacheFile, "{corrupt_json", $Utf8NoBom)
    $r7 = Invoke-HydrationChild -ArgsList @('@test_cache.json') -StdinText '["healed"]'
    Write-Host "Test 7 (Self-heal corrupt cache) [EXPECT: PASS] Exit=$($r7.ExitCode) StdOut=$($r7.StdOut.Trim())"
    Assert-Condition ($r7.ExitCode -eq 0 -and $r7.StdOut.Trim() -eq '["healed"]') "Test 7 failed"

    # Test 8: Corrupt cache without external parent -> preserves corrupt cache untouched, exits 1
    [System.IO.File]::WriteAllText($CacheFile, "{corrupt_json", $Utf8NoBom)
    $r8 = Invoke-HydrationChild -ArgsList @('@test_cache.json') -StdinText "not_json" -EnvVars @{}
    $after8 = [System.IO.File]::ReadAllText($CacheFile, $Utf8NoBom)
    Write-Host "Test 8 (Bad seed preserves existing file) [EXPECT: FAIL] Exit=$($r8.ExitCode)"
    Assert-Condition ($r8.ExitCode -eq 1 -and $after8 -eq "{corrupt_json") "Test 8 failed"

    # Test 9: Non-string array elements rejected (e.g. [1, 2, 3])
    Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
    $r9 = Invoke-HydrationChild -ArgsList @('@test_cache.json') -StdinText '[1, 2, 3]'
    Write-Host "Test 9 (Reject numeric JSON array) [EXPECT: FAIL] Exit=$($r9.ExitCode)"
    Assert-Condition ($r9.ExitCode -eq 1 -and -not (Test-Path $CacheFile)) "Test 9 failed"

    # Test 10: --handshake mode emits HEAL_NEEDED and reads single-line seed
    Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
    $r10 = Invoke-HydrationChild -ArgsList @('@test_cache.json', '--handshake') -StdinText '["hs_ok"]'
    Write-Host "Test 10 (--handshake) [EXPECT: PASS] Exit=$($r10.ExitCode) StdOut=$($r10.StdOut.Trim())"
    Assert-Condition ($r10.ExitCode -eq 0 -and $r10.StdErr -match "HEAL_NEEDED" -and $r10.StdOut.Trim() -eq '["hs_ok"]') "Test 10 failed"

    # Test 11: --acknowledge=<inline_json>
    Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
    $r11 = Invoke-HydrationChild -ArgsList @('@test_cache.json', '--acknowledge=["ack_inline"]')
    Write-Host "Test 11 (--acknowledge inline) [EXPECT: PASS] Exit=$($r11.ExitCode) StdOut=$($r11.StdOut.Trim())"
    Assert-Condition ($r11.ExitCode -eq 0 -and $r11.StdOut.Trim() -eq '["ack_inline"]') "Test 11 failed"

    # Test 12: --acknowledge=@<file>
    Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
    [System.IO.File]::WriteAllText($SeedFile, '["ack_file_1", "ack_file_2"]', $Utf8NoBom)
    $r12 = Invoke-HydrationChild -ArgsList @('@test_cache.json', '--acknowledge=@test_seed.json')
    Write-Host "Test 12 (--acknowledge=@file) [EXPECT: PASS] Exit=$($r12.ExitCode) StdOut=$($r12.StdOut.Trim())"
    Assert-Condition ($r12.ExitCode -eq 0 -and $r12.StdOut.Trim() -eq '["ack_file_1","ack_file_2"]') "Test 12 failed"

    # Test 13: Empty JSON array []
    Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
    $r13 = Invoke-HydrationChild -ArgsList @('@test_cache.json') -StdinText '[]'
    Write-Host "Test 13 (Empty JSON array []) [EXPECT: PASS] Exit=$($r13.ExitCode) StdOut=$($r13.StdOut.Trim())"
    Assert-Condition ($r13.ExitCode -eq 0 -and $r13.StdOut.Trim() -eq '[]') "Test 13 failed"

    # Test 14: UTF-8 BOM + Surrogate pairs (emoji)
    Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
    $bom = [char]0xFEFF
    $rocket = [char]::ConvertFromUtf32(0x1F680)
    $r14 = Invoke-HydrationChild -ArgsList @('@test_cache.json') -StdinText ($bom + '["\uD83D\uDE80 rocket"]')
    Write-Host "Test 14 (BOM + Surrogate pair) [EXPECT: PASS] Exit=$($r14.ExitCode)"
    Assert-Condition ($r14.ExitCode -eq 0 -and $r14.StdOut.Trim() -eq "[`"$rocket rocket`"]") "Test 14 failed"

    # Test 15: Unpaired surrogate rejected
    Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
    $r15 = Invoke-HydrationChild -ArgsList @('@test_cache.json') -StdinText '["\uD800"]'
    Write-Host "Test 15 (Reject unpaired surrogate) [EXPECT: FAIL] Exit=$($r15.ExitCode)"
    Assert-Condition ($r15.ExitCode -eq 1) "Test 15 failed"

    # Test 16: Unescaped control char rejected
    Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
    $r16 = Invoke-HydrationChild -ArgsList @('@test_cache.json') -StdinText "[`"line1`nline2`"]"
    Write-Host "Test 16 (Reject raw control char) [EXPECT: FAIL] Exit=$($r16.ExitCode)"
    Assert-Condition ($r16.ExitCode -eq 1) "Test 16 failed"

    # Test 17: Trailing garbage after ] rejected
    Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
    $r17 = Invoke-HydrationChild -ArgsList @('@test_cache.json') -StdinText '["ok"] trailing'
    Write-Host "Test 17 (Reject trailing garbage) [EXPECT: FAIL] Exit=$($r17.ExitCode)"
    Assert-Condition ($r17.ExitCode -eq 1) "Test 17 failed"

    # Test 18: --no-parent flag
    Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
    $r18 = Invoke-HydrationChild -ArgsList @('@test_cache.json', '--no-parent') -StdinText '["np_ok"]'
    Write-Host "Test 18 (--no-parent) [EXPECT: PASS] Exit=$($r18.ExitCode)"
    Assert-Condition ($r18.ExitCode -eq 0 -and $r18.StdOut.Trim() -eq '["np_ok"]') "Test 18 failed"

    # Test 19: --child flag
    Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
    $r19 = Invoke-HydrationChild -ArgsList @('@test_cache.json', '--child') -StdinText '["child_ok"]'
    Write-Host "Test 19 (--child) [EXPECT: PASS] Exit=$($r19.ExitCode)"
    Assert-Condition ($r19.ExitCode -eq 0 -and $r19.StdOut.Trim() -eq '["child_ok"]') "Test 19 failed"

    # Test 20: Whitespace-only entries stripped during normalization
    Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
    $r20 = Invoke-HydrationChild -ArgsList @('@test_cache.json') -StdinText '["   ", "\t\n", "valid"]'
    Write-Host "Test 20 (Whitespace-only entries stripped) [EXPECT: PASS] Exit=$($r20.ExitCode) StdOut=$($r20.StdOut.Trim())"
    Assert-Condition ($r20.ExitCode -eq 0 -and $r20.StdOut.Trim() -eq '["valid"]') "Test 20 failed"

    # Test 21: Missing --acknowledge file fails cleanly
    Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
    $r21 = Invoke-HydrationChild -ArgsList @('@test_cache.json', '--acknowledge=@nonexistent_seed.json')
    Write-Host "Test 21 (Missing --acknowledge file) [EXPECT: FAIL] Exit=$($r21.ExitCode)"
    Assert-Condition ($r21.ExitCode -eq 1 -and -not (Test-Path $CacheFile)) "Test 21 failed"

    # Test 22: Nested parent directory auto-creation
    $nestedRel  = "nested_dir/sub_a/sub_b/cache.json"
    $nestedFull = Join-Path $PSScriptRoot "nested_dir\sub_a\sub_b\cache.json"
    $r22 = Invoke-HydrationChild -ArgsList @("@$nestedRel") -StdinText '["nested_ok"]'
    Write-Host "Test 22 (Nested dir creation) [EXPECT: PASS] Exit=$($r22.ExitCode)"
    Assert-Condition ($r22.ExitCode -eq 0 -and (Test-Path $nestedFull)) "Test 22 failed"

    # Test 23: 16-process concurrent atomic writes (direct ProcessStartInfo launch)
    Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
    $procs23 = foreach ($idx in 1..16) {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $Exe
        $psi.WorkingDirectory = $PSScriptRoot
        $psi.UseShellExecute = $false
        $psi.RedirectStandardInput = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.StandardOutputEncoding = $Utf8NoBom
        $psi.StandardErrorEncoding = $Utf8NoBom
        $psi.EnvironmentVariables.Remove("HYD_NO_PARENT")
        $psi.EnvironmentVariables["HYD_MANAGED"] = "1"
        $psi.Arguments = '@test_cache.json'
        $p = [System.Diagnostics.Process]::Start($psi)
        $bytes = $Utf8NoBom.GetBytes("[`"worker_$idx`"]")
        $p.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
        $p.StandardInput.Close()
        $p
    }
    $codes = foreach ($p in $procs23) {
        $null = $p.StandardOutput.ReadToEnd()
        $null = $p.StandardError.ReadToEnd()
        $p.WaitForExit()
        $code = $p.ExitCode
        $p.Dispose()
        $code
    }
    $allZero = ($codes | Where-Object { $_ -ne 0 }).Count -eq 0
    Write-Host "Test 23 (16 concurrent writers) [EXPECT: PASS] AllZero=$allZero"
    Assert-Condition ($allZero -and (Test-Path $CacheFile)) "Test 23 failed"

    # Test 24: Direct Inline JSON Positional Argument (No Cache File)
    $r24 = Invoke-HydrationChild -ArgsList @('["Direct_A", "DIRECT_A", "Direct_B"]')
    Write-Host "Test 24 (Direct Inline JSON) [EXPECT: PASS] Exit=$($r24.ExitCode) StdOut=$($r24.StdOut.Trim()) StdErr='$($r24.StdErr)'"
    Assert-Condition ($r24.ExitCode -eq 0 -and $r24.StdOut.Trim() -eq '["direct_a","direct_b"]' -and $r24.StdErr -eq "") "Test 24 failed"

    # Test 25: Inline HTML Rejected Without --validator (Strict JSON Default)
    $r25 = Invoke-HydrationChild -ArgsList @('<ul><li>Item One</li><li>ITEM ONE</li></ul>')
    Write-Host "Test 25 (Inline HTML Rejected Without Validator) [EXPECT: FAIL] Exit=$($r25.ExitCode)"
    Assert-Condition ($r25.ExitCode -eq 1) "Test 25 failed"

    $ValExe = Join-Path $PSScriptRoot "test_validator.exe"

# Test 26: @file HTML rejected by strict JSON, accepted by --validator=@test_validator.exe
Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
[System.IO.File]::WriteAllText($SeedFile, "<div>hello</div>", $Utf8NoBom)
$r26a = Invoke-HydrationChild -ArgsList @('@test_cache.json', '--acknowledge=@test_seed.json')
$r26b = Invoke-HydrationChild -ArgsList @('@test_cache.json', "--validator=@$ValExe", '--acknowledge=@test_seed.json')
Write-Host "Test 26 (@file HTML Strict JSON vs --validator) [EXPECT: FAIL/PASS] Exits=$($r26a.ExitCode)/$($r26b.ExitCode)"
Assert-Condition ($r26a.ExitCode -eq 1 -and $r26b.ExitCode -eq 0 -and $r26b.StdOut.Trim() -eq "<div>hello</div>") "Test 26 failed"

    # Create custom validator scripts for Tests 27-30
    [System.IO.File]::WriteAllText($CustomValFile, @'
$data = [Console]::In.ReadToEnd()
if ($data.StartsWith("VALID:")) {
    Write-Output "True"
    exit 0
} else {
    Write-Output "False"
    exit 1
}
'@, $Utf8NoBom)

    [System.IO.File]::WriteAllText($MultiValFile, @'
$data = [Console]::In.ReadToEnd()
Write-Output "[INFO] Inspecting payload length=$($data.Length)"
if ($data.StartsWith("MULTI:")) {
    Write-Output "True"
    exit 0
} else {
    Write-Output "False"
    exit 1
}
'@, $Utf8NoBom)

    [System.IO.File]::WriteAllText($ExitValFile, @'
$data = [Console]::In.ReadToEnd()
if ($data.StartsWith("EXITOK:")) {
    exit 0
} else {
    exit 1
}
'@, $Utf8NoBom)

    # Test 27: Custom Validator Seed (inline custom payload accepted by test_validator.exe)
Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
$r27 = Invoke-HydrationChild -ArgsList @('@test_cache.json', "--validator=@$ValExe", '--acknowledge=VALID_CUSTOM_PAYLOAD')
Write-Host "Test 27 (Custom Validator Seed) [EXPECT: PASS] Exit=$($r27.ExitCode) StdOut=$($r27.StdOut.Trim())"
Assert-Condition ($r27.ExitCode -eq 0 -and $r27.StdOut.Trim() -eq "VALID_CUSTOM_PAYLOAD" -and (Get-Content $CacheFile -Raw).Trim() -eq "VALID_CUSTOM_PAYLOAD") "Test 27 failed"

# Test 28: Custom Validator Reject (non-zero exit code)
Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
$r28 = Invoke-HydrationChild -ArgsList @('@test_cache.json', "--validator=`"$ValExe fail`"", '--acknowledge=VALID_CUSTOM_PAYLOAD')
Write-Host "Test 28 (Custom Validator Reject Exit) [EXPECT: FAIL] Exit=$($r28.ExitCode)"
Assert-Condition ($r28.ExitCode -eq 1 -and -not (Test-Path $CacheFile)) "Test 28 failed"

# Test 29: Custom Validator Reject ('0' or 'false' on stdout)
Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
$r29a = Invoke-HydrationChild -ArgsList @('@test_cache.json', "--validator=`"$ValExe zero`"", '--acknowledge=VALID_CUSTOM_PAYLOAD')
$r29b = Invoke-HydrationChild -ArgsList @('@test_cache.json', "--validator=`"$ValExe false`"", '--acknowledge=VALID_CUSTOM_PAYLOAD')
Write-Host "Test 29 (Custom Validator Reject Verdict) [EXPECT: FAIL/FAIL] Exits=$($r29a.ExitCode)/$($r29b.ExitCode)"
Assert-Condition ($r29a.ExitCode -eq 1 -and $r29b.ExitCode -eq 1 -and -not (Test-Path $CacheFile)) "Test 29 failed"

# Test 30: Warm Cache Hit with Custom Validator (re-validates cached file)
[System.IO.File]::WriteAllText($CacheFile, "VALID_CACHED_ENTRY", $Utf8NoBom)
$r30 = Invoke-HydrationChild -ArgsList @('@test_cache.json', "--validator=@$ValExe")
Write-Host "Test 30 (Warm Cache Custom Validator) [EXPECT: PASS] Exit=$($r30.ExitCode) StdOut=$($r30.StdOut.Trim())"
Assert-Condition ($r30.ExitCode -eq 0 -and $r30.StdOut.Trim() -eq "VALID_CACHED_ENTRY") "Test 30 failed"

    # Test 31: Standalone self-parent cold cache without explicit seed fails immediately without hanging
    Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
    $r31 = Invoke-HydrationChild -ArgsList @('@test_cache.json') -EnvVars @{}
    Write-Host "Test 31 (Standalone cold cache no-hang fail) [EXPECT: FAIL] Exit=$($r31.ExitCode)"
    Assert-Condition ($r31.ExitCode -eq 1 -and -not (Test-Path $CacheFile) -and $r31.StdErr -match "was not created or overwritten") "Test 31 failed"

    # Test 32: Nested / non-string / trailing-comma JSON structures rejected while preserving existing cache
    [System.IO.File]::WriteAllText($CacheFile, "{corrupt_json", $Utf8NoBom)
    $badJsons = @('[["nested"]]', '{"key":"val"}', '[null]', '[true]', '["a",]')
    foreach ($bj in $badJsons) {
        $r32 = Invoke-HydrationChild -ArgsList @('@test_cache.json') -StdinText $bj
        $after32 = [System.IO.File]::ReadAllText($CacheFile, $Utf8NoBom)
        Assert-Condition ($r32.ExitCode -eq 1 -and $after32 -eq "{corrupt_json") "Test 32 failed on $bj"
    }
    Write-Host "Test 32 (Nested/non-string/trailing-comma JSON rejection) [EXPECT: FAIL] Passed all 5 cases"

    # Test 33: Unicode case-folding & deduplication collisions (Latin-1, Greek, Cyrillic, escaped BMP)
    Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
    $r33 = Invoke-HydrationChild -ArgsList @('["CAF\u00C9", "caf\u00E9", "  Caf\u00E9  ", "\u0391\u0392", "\u03B1\u03B2"]')
    $expected33 = '["caf' + [char]0x00E9 + '","' + [char]0x03B1 + [char]0x03B2 + '"]'
    Write-Host "Test 33 (Unicode case-folding collisions) [EXPECT: PASS] Exit=$($r33.ExitCode) StdOut=$($r33.StdOut.Trim())"
    Assert-Condition ($r33.ExitCode -eq 0 -and $r33.StdOut.Trim() -eq $expected33) "Test 33 failed"

    # Test 34: Unicode whitespace (U+00A0, U+2003) & zero-width format characters (U+200B, U+FEFF)
    $r34 = Invoke-HydrationChild -ArgsList @('["\u00A0hello\u2003\u200B\uFEFFworld\u00A0", "\u200B\uFEFF"]')
    Write-Host "Test 34 (Unicode whitespace & zero-width stripping) [EXPECT: PASS] Exit=$($r34.ExitCode) StdOut=$($r34.StdOut.Trim())"
    Assert-Condition ($r34.ExitCode -eq 0 -and $r34.StdOut.Trim() -eq '["hello world"]') "Test 34 failed"

    # Test 35: Escaped control characters (\b, \f, \n, \r, \t, \", \\, \u0001) round-trip through cache
    Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
    $escPayload = '["a\"b\\c\nd\te\u0001f"]'
    $r35a = Invoke-HydrationChild -ArgsList @('@test_cache.json') -StdinText $escPayload
    $r35b = Invoke-HydrationChild -ArgsList @('@test_cache.json')
    Write-Host "Test 35 (Escaped control chars round-trip) [EXPECT: PASS] Exits=$($r35a.ExitCode)/$($r35b.ExitCode)"
    Assert-Condition ($r35a.ExitCode -eq 0 -and $r35b.ExitCode -eq 0 -and $r35a.StdOut.Trim() -eq $r35b.StdOut.Trim()) "Test 35 failed"

    # Test 36: Corrupted cache fails custom validator and heals from stdin
[System.IO.File]::WriteAllText($CacheFile, "CORRUPT_DATA", $Utf8NoBom)
$r36 = Invoke-HydrationChild -ArgsList @('@test_cache.json', "--validator=@$ValExe") -StdinText "VALID_HEALED"
Write-Host "Test 36 (Corrupt Cache Custom Validator Heal) [EXPECT: PASS] Exit=$($r36.ExitCode) StdOut=$($r36.StdOut.Trim())"
Assert-Condition ($r36.ExitCode -eq 0 -and $r36.StdOut.Trim() -eq "VALID_HEALED" -and (Get-Content $CacheFile -Raw).Trim() -eq "VALID_HEALED") "Test 36 failed"

    # Test 37: Concurrent 8 readers + 8 writers contention on shared cache file (direct ProcessStartInfo launch)
    [System.IO.File]::WriteAllText($CacheFile, '["initial_seed"]', $Utf8NoBom)
    $procs37 = foreach ($idx in 1..16) {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $Exe
        $psi.WorkingDirectory = $PSScriptRoot
        $psi.UseShellExecute = $false
        $psi.RedirectStandardInput = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $psi.StandardOutputEncoding = $Utf8NoBom
        $psi.StandardErrorEncoding = $Utf8NoBom
        $psi.EnvironmentVariables.Remove("HYD_NO_PARENT")
        $psi.EnvironmentVariables["HYD_MANAGED"] = "1"
        if ($idx % 2 -eq 0) {
            $psi.Arguments = '@test_cache.json --acknowledge=["writer_' + $idx + '"]'
        } else {
            $psi.Arguments = '@test_cache.json'
        }
        $p = [System.Diagnostics.Process]::Start($psi)
        $p.StandardInput.Close()
        $p
    }
    $rwResults = foreach ($p in $procs37) {
        $out = $p.StandardOutput.ReadToEnd().Trim()
        $null = $p.StandardError.ReadToEnd()
        $p.WaitForExit()
        $code = $p.ExitCode
        $p.Dispose()
        [PSCustomObject]@{ ExitCode = $code; Out = $out }
    }
    $allValid = ($rwResults | Where-Object { $_.ExitCode -ne 0 -or -not ($_.Out.StartsWith('[') -and $_.Out.EndsWith(']')) }).Count -eq 0
    Write-Host "Test 37 (8 concurrent readers + 8 writers) [EXPECT: PASS] AllValid=$allValid"
    Assert-Condition $allValid "Test 37 failed"

    # Test 38: Spaced/Unicode paths, --help/--version (exit 0), and --max-bytes cap
    Remove-Item $CacheFile -Force -ErrorAction SilentlyContinue
    $unicodeRel = "@nested_dir/t" + [char]0x00EB + "st dir/c" + [char]0x00E2 + "ch" + [char]0x00E9 + " file.json"
    $r38a = Invoke-HydrationChild -ArgsList @($unicodeRel, '--acknowledge=["unicode_path_ok"]')
    $r38b = Invoke-HydrationChild -ArgsList @('--help')
    $r38c = Invoke-HydrationChild -ArgsList @('--version')
    $r38d = Invoke-HydrationChild -ArgsList @('@test_cache.json', '--max-bytes', '8') -StdinText '["1234567890"]'
    Write-Host "Test 38 (Unicode path, --help, --version, --max-bytes) [EXPECT: PASS/PASS/PASS/FAIL] Exits=$($r38a.ExitCode)/$($r38b.ExitCode)/$($r38c.ExitCode)/$($r38d.ExitCode)"
    Assert-Condition ($r38a.ExitCode -eq 0 -and $r38a.StdOut.Trim() -eq '["unicode_path_ok"]' -and
                      $r38b.ExitCode -eq 0 -and $r38b.StdOut -match "Usage: hydration" -and
                      $r38c.ExitCode -eq 0 -and $r38c.StdOut.Trim() -eq "hydration 1.0.0" -and
                      $r38d.ExitCode -eq 1 -and -not (Test-Path $CacheFile)) "Test 38 failed"

# Test 39: Unmanaged parent supervisor enforces all-or-nothing rollback on late syntax error
$t39Cache = Join-Path $PSScriptRoot "test_t39_rollback.json"
Remove-Item $t39Cache -Force -ErrorAction SilentlyContinue

$sbBad = New-Object System.Text.StringBuilder
[void]$sbBad.Append('[')
for ($i = 0; $i -lt 5000; $i++) {
    [void]$sbBad.Append('"valid_item_').Append($i).Append('",')
}
[void]$sbBad.Append('UNTERMINATED_TOKEN]') # Error at item #5,001

$psi39 = New-Object System.Diagnostics.ProcessStartInfo
$psi39.FileName = $Exe
$psi39.WorkingDirectory = $PSScriptRoot
$psi39.UseShellExecute = $false
$psi39.RedirectStandardInput = $true
$psi39.RedirectStandardOutput = $true
$psi39.RedirectStandardError = $true
$psi39.EnvironmentVariables.Remove("HYD_MANAGED")
$psi39.EnvironmentVariables.Remove("HYD_NO_PARENT")
$psi39.Arguments = '"@test_t39_rollback.json"'

$p39 = [System.Diagnostics.Process]::Start($psi39)
$out39Task = $p39.StandardOutput.ReadToEndAsync()
$err39Task = $p39.StandardError.ReadToEndAsync()
$badBytes = (New-Object System.Text.UTF8Encoding($false)).GetBytes($sbBad.ToString())
$p39.StandardInput.BaseStream.Write($badBytes, 0, $badBytes.Length)
$p39.StandardInput.Close()
$p39.WaitForExit()

$out39 = $out39Task.GetAwaiter().GetResult()
$err39 = $err39Task.GetAwaiter().GetResult()
$cacheExists39 = Test-Path $t39Cache
Remove-Item $t39Cache -Force -ErrorAction SilentlyContinue

if ($p39.ExitCode -ne 1 -or $out39.Length -ne 0 -or $cacheExists39 -or ($err39 -notmatch "was not created or overwritten")) {
    throw "ASSERTION FAILED: Test 39 supervisor rollback failed (Exit=$($p39.ExitCode), OutLen=$($out39.Length), CacheExists=$cacheExists39)"
}
Write-Host "Test 39 (Unmanaged supervisor all-or-nothing rollback) [PASS]"

    Write-Host "`nALL 39 TESTS PASSED."
}
finally {
    Cleanup-Artifacts
}