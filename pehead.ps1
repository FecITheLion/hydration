$bytes = [System.IO.File]::ReadAllBytes((Resolve-Path .\hydration.exe))
$peOff = [BitConverter]::ToInt32($bytes, 0x3C)
$numSections = [BitConverter]::ToUInt16($bytes, $peOff + 6)
$optHdrSize  = [BitConverter]::ToUInt16($bytes, $peOff + 20)
$magic       = [BitConverter]::ToUInt16($bytes, $peOff + 24)
$isPE32Plus  = ($magic -eq 0x20B)

Write-Host ("File Size : {0:N0} bytes ({1:N2} KB)" -f $bytes.Length, ($bytes.Length / 1KB))
Write-Host ("PE Format : {0}" -f $(if ($isPE32Plus) { "PE32+ (x64)" } else { "PE32 (x86)" }))

# Parse PE Sections
$secStart = $peOff + 24 + $optHdrSize
$sections = for ($i = 0; $i -lt $numSections; $i++) {
    $off = $secStart + ($i * 40)
    $name = [System.Text.Encoding]::ASCII.GetString($bytes, $off, 8).TrimEnd([char]0)
    [PSCustomObject]@{
        Section     = $name
        VirtualSize = [BitConverter]::ToUInt32($bytes, $off + 8)
        VirtualAddr = [BitConverter]::ToUInt32($bytes, $off + 12)
        RawSize     = [BitConverter]::ToUInt32($bytes, $off + 16)
        RawPtr      = [BitConverter]::ToUInt32($bytes, $off + 20)
    }
}
$sections | Format-Table Section, @{N='VirtualKB';E={[Math]::Round($_.VirtualSize/1KB,2)}}, @{N='RawBytes';E={$_.RawSize}} -AutoSize

# Resolve Import Directory RVA -> File Offset & list DLL names
$impDirOffset = $peOff + 24 + $(if ($isPE32Plus) { 120 } else { 104 })
$impRva = [BitConverter]::ToUInt32($bytes, $impDirOffset)

function RvaToOffset([uint32]$rva) {
    foreach ($s in $sections) {
        $span = [Math]::Max($s.VirtualSize, $s.RawSize)
        if ($rva -ge $s.VirtualAddr -and $rva -lt ($s.VirtualAddr + $span)) {
            return ($rva - $s.VirtualAddr) + $s.RawPtr
        }
    }
    return 0
}

$impOff = RvaToOffset $impRva
Write-Host "Imported DLLs:"
while ($impOff -gt 0) {
    $nameRva = [BitConverter]::ToUInt32($bytes, $impOff + 12)
    if ($nameRva -eq 0) { break }
    $strOff = RvaToOffset $nameRva
    $endOff = $strOff
    while ($bytes[$endOff] -ne 0) { $endOff++ }
    $dllName = [System.Text.Encoding]::ASCII.GetString($bytes, $strOff, $endOff - $strOff)
    Write-Host "  - $dllName"
    $impOff += 20
}