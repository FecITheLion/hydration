$isolatedDir = Join-Path $env:TEMP ("hydration_isolated_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $isolatedDir -Force | Out-Null

try {
    $isolatedExe = Join-Path $isolatedDir "hydration.exe"
    $isolatedCache = Join-Path $isolatedDir "isolated_cache.json"
    Copy-Item ".\hydration.exe" $isolatedExe -Force

    function Invoke-SanitizedExe {
        param([string]$Arguments, [string]$StdinText = "")

        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = $isolatedExe
        $psi.WorkingDirectory = $isolatedDir
        $psi.Arguments = $Arguments
        $psi.UseShellExecute = $false
        $psi.RedirectStandardInput = $true
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true

        # Strip MSYS2/MinGW/toolchain paths; expose ONLY Windows core System32
        $psi.EnvironmentVariables["PATH"] = "$env:SystemRoot\System32"

        $p = [System.Diagnostics.Process]::Start($psi)
        $outTask = $p.StandardOutput.ReadToEndAsync()
        $errTask = $p.StandardError.ReadToEndAsync()

        if ($StdinText.Length -gt 0) {
            $bytes = (New-Object System.Text.UTF8Encoding($false)).GetBytes($StdinText)
            $p.StandardInput.BaseStream.Write($bytes, 0, $bytes.Length)
        }
        $p.StandardInput.Close()
        $p.WaitForExit()

        [PSCustomObject]@{
            ExitCode = $p.ExitCode
            StdOut   = $outTask.GetAwaiter().GetResult().Trim()
            StdErr   = $errTask.GetAwaiter().GetResult().Trim()
        }
    }

    # 1. Version check under System32-only PATH (catches 0xC0000135 / -1073741515 missing DLL crashes)
    $vRes = Invoke-SanitizedExe -Arguments "--version"
    if ($vRes.ExitCode -ne 0) {
        throw ("DLL DEPENDENCY FAILURE: ExitCode={0} (0x{1:X8}), StdErr={2}" -f $vRes.ExitCode, $vRes.ExitCode, $vRes.StdErr)
    }

    # 2. Cold cache seed + warm cache hit under System32-only PATH (tests supervisor + CreateThread + UCRT)
    $coldRes = Invoke-SanitizedExe -Arguments '"@isolated_cache.json"' -StdinText '["alpha","beta","alpha"]'
    if ($coldRes.ExitCode -ne 0 -or $coldRes.StdOut -ne '["alpha","beta"]') {
        throw "ISOLATED COLD RUN FAILED: Exit=$($coldRes.ExitCode), Out='$($coldRes.StdOut)', Err='$($coldRes.StdErr)'"
    }

    $warmRes = Invoke-SanitizedExe -Arguments '"@isolated_cache.json"'
    if ($warmRes.ExitCode -ne 0 -or $warmRes.StdOut -ne '["alpha","beta"]') {
        throw "ISOLATED WARM RUN FAILED: Exit=$($warmRes.ExitCode), Out='$($warmRes.StdOut)', Err='$($warmRes.StdErr)'"
    }

    Write-Host "[PASS] Zero external DLL dependencies verified under PATH='$env:SystemRoot\System32'"
    Write-Host "       Version : $($vRes.StdOut)"
    Write-Host "       Output  : $($warmRes.StdOut)"
}
finally {
    Remove-Item $isolatedDir -Recurse -Force -ErrorAction SilentlyContinue
}