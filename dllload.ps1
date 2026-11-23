param(
    [string]$BinaryPath = ".\hydration.exe"
)

$bytes = [System.IO.File]::ReadAllBytes((Resolve-Path $BinaryPath))
$peOff = [BitConverter]::ToInt32($bytes, 0x3C)
$numSec = [BitConverter]::ToUInt16($bytes, $peOff + 6)
$optSize = [BitConverter]::ToUInt16($bytes, $peOff + 20)
$is64 = ([BitConverter]::ToUInt16($bytes, $peOff + 24) -eq 0x20B)

$secStart = $peOff + 24 + $optSize
$sections = for ($i = 0; $i -lt $numSec; $i++) {
    $o = $secStart + ($i * 40)
    [PSCustomObject]@{
        VSize = [BitConverter]::ToUInt32($bytes, $o + 8)
        VAddr = [BitConverter]::ToUInt32($bytes, $o + 12)
        RSize = [BitConverter]::ToUInt32($bytes, $o + 16)
        RPtr  = [BitConverter]::ToUInt32($bytes, $o + 20)
    }
}

function RvaToOff([uint32]$rva) {
    foreach ($s in $sections) {
        if ($rva -ge $s.VAddr -and $rva -lt ($s.VAddr + [Math]::Max($s.VSize, $s.RSize))) {
            return ($rva - $s.VAddr) + $s.RPtr
        }
    }
    return 0
}

function ReadAscii([int]$off) {
    $end = $off
    while ($bytes[$end] -ne 0) { $end++ }
    [System.Text.Encoding]::ASCII.GetString($bytes, $off, $end - $off)
}

$impOff = RvaToOff ([BitConverter]::ToUInt32($bytes, $peOff + 24 + $(if ($is64) { 120 } else { 104 })))
$step = if ($is64) { 8 } else { 4 }
$totalDlls = 0
$totalImports = 0

while ($impOff -gt 0) {
    $nameRva = [BitConverter]::ToUInt32($bytes, $impOff + 12)
    if ($nameRva -eq 0) { break }

    $dllName = ReadAscii (RvaToOff $nameRva)
    $iltRva = [BitConverter]::ToUInt32($bytes, $impOff)
    if ($iltRva -eq 0) { $iltRva = [BitConverter]::ToUInt32($bytes, $impOff + 16) }
    $iltOff = RvaToOff $iltRva

    $symbols = for ($p = $iltOff; ; $p += $step) {
        $raw = if ($is64) { [BitConverter]::ToInt64($bytes, $p) } else { [BitConverter]::ToInt32($bytes, $p) }
        if ($raw -eq 0) { break }
        if ($raw -gt 0) {
            $hintNameRva = [BitConverter]::ToUInt32($bytes, $p)
            ReadAscii ((RvaToOff $hintNameRva) + 2)
        } else {
            $ordinal = [BitConverter]::ToUInt16($bytes, $p)
            "Ordinal #$ordinal"
        }
    }

    $sorted = @($symbols | Sort-Object -Unique)
    $totalDlls++
    $totalImports += $sorted.Count

    Write-Host "`n[$dllName] ($($sorted.Count) imports)" -ForegroundColor Cyan
    foreach ($sym in $sorted) {
        Write-Host "  - $sym"
    }

    $impOff += 20
}

Write-Host "`nTotal: $totalImports imported functions across $totalDlls DLLs" -ForegroundColor Green