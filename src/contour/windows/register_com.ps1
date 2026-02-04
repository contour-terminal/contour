# Register COM servers and App Extensions for Contour sparse package
$binPath = "D:\contour\out\build\windows-msvc-ninja-debug-user\bin"
$contourExe = Join-Path $binPath "contour.exe"
$openConsoleExe = Join-Path $binPath "OpenConsole.exe"

# Terminal Handoff CLSID
$terminalCLSID = "{B178D323-E77D-4C67-AF21-AE2B81F269F0}"
# Console Handoff CLSID
$consoleCLSID = "{F00DCAFE-0000-0000-0000-000000000001}"

Write-Host "Registering Contour COM servers..."

# Register Terminal Handoff (contour.exe)
$regPath = "HKCU:\Software\Classes\CLSID\$terminalCLSID"
New-Item -Path $regPath -Force | Out-Null
New-ItemProperty -Path $regPath -Name "(Default)" -Value "Contour Terminal Handoff" -Force | Out-Null

$localServerPath = "$regPath\LocalServer32"
New-Item -Path $localServerPath -Force | Out-Null
New-ItemProperty -Path $localServerPath -Name "(Default)" -Value "`"$contourExe`"" -Force | Out-Null

Write-Host "Registered Terminal CLSID: $terminalCLSID"

# Register Console Handoff (OpenConsole.exe)
$regPath2 = "HKCU:\Software\Classes\CLSID\$consoleCLSID"
New-Item -Path $regPath2 -Force | Out-Null
New-ItemProperty -Path $regPath2 -Name "(Default)" -Value "Contour Console Handoff" -Force | Out-Null

$localServerPath2 = "$regPath2\LocalServer32"
New-Item -Path $localServerPath2 -Force | Out-Null
New-ItemProperty -Path $localServerPath2 -Name "(Default)" -Value "`"$openConsoleExe`"" -Force | Out-Null

Write-Host "Registered Console CLSID: $consoleCLSID"

# Register App Extensions manually since sparse packages don't auto-register them
Write-Host "Registering App Extensions..."

# Get package family name
$package = Get-AppxPackage -Name "Contour.Terminal"
if ($package) {
    $pfn = $package.PackageFamilyName

    # Register terminal.host extension
    $extensionPath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\App Extensions\Catalog\PackageContract\com.microsoft.windows.terminal.host\$pfn!ContourTerminalExtension"
    New-Item -Path $extensionPath -Force | Out-Null
    New-ItemProperty -Path $extensionPath -Name "DisplayName" -Value "Contour Terminal" -Force | Out-Null
    New-ItemProperty -Path $extensionPath -Name "Clsid" -Value $terminalCLSID -Force | Out-Null
    Write-Host "Registered terminal.host extension"

    # Register console.host extension
    $consolePath = "HKCU:\Software\Microsoft\Windows\CurrentVersion\App Extensions\Catalog\PackageContract\com.microsoft.windows.console.host\$pfn!ContourConsole"
    New-Item -Path $consolePath -Force | Out-Null
    New-ItemProperty -Path $consolePath -Name "DisplayName" -Value "Contour Console" -Force | Out-Null
    New-ItemProperty -Path $consolePath -Name "Clsid" -Value $consoleCLSID -Force | Out-Null
    Write-Host "Registered console.host extension"
}
else {
    Write-Host "WARNING: Contour.Terminal package not found. Register the package first." -ForegroundColor Yellow
}

Write-Host "Registration complete!" -ForegroundColor Green
