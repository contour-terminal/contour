# troubleshoot_package.ps1
param(
    [string]$BinDir = "$PSScriptRoot"
)

$ManifestPath = Join-Path $BinDir "AppxManifest.xml"
$LogPath = Join-Path $BinDir "troubleshoot_log.txt"

Start-Transcript -Path $LogPath -Force

Write-Host "--- Troubleshooting Contour Package Registration ---" -ForegroundColor Cyan
Write-Host "Bin Directory: $BinDir"
Write-Host "Manifest Path: $ManifestPath"

# 1. Check File Existence
$RequiredFiles = @(
    "contour.exe",
    "Assets\StoreLogo.png",
    "Assets\Square150x150Logo.png",
    "Assets\Square44x44Logo.png"
)

$MissingFiles = $false
foreach ($file in $RequiredFiles) {
    $fullPath = Join-Path $BinDir $file
    if (-not (Test-Path $fullPath)) {
        Write-Error "MISSING FILE: $file at $fullPath"
        $MissingFiles = $true
    }
    else {
        Write-Host "FOUND: $file" -ForegroundColor Green
    }
}

if ($MissingFiles) {
    Write-Error "Critical files are missing. Cannot proceed."
    Stop-Transcript
    exit 1
}

# 2. Check for conflicting manifests in parent directories
Write-Host "`nchecking for conflicting manifests..."
$parent = Split-Path $BinDir -Parent
while ($parent -and (Split-Path $parent -Parent) -ne $parent) {
    if (Test-Path (Join-Path $parent "AppxManifest.xml")) {
        Write-Warning "CONFLICT WARNING: Found AppxManifest.xml in parent: $parent"
    }
    $parent = Split-Path $parent -Parent
}

# 3. Try Minimal Manifest
Write-Host "`nAttempting registration with MINIMAL manifest..."
$OriginalManifest = Get-Content $ManifestPath -Raw
$MinimalManifest = @"
<?xml version="1.0" encoding="utf-8"?>
<Package
  xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"
  xmlns:uap="http://schemas.microsoft.com/appx/manifest/uap/windows10"
  xmlns:rescap="http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities"
  IgnorableNamespaces="uap rescap">

  <Identity
    Name="Contour.Terminal.Debug"
    Publisher="CN=Christian Parpart"
    Version="1.0.0.0" />

  <Properties>
    <DisplayName>Contour Debug</DisplayName>
    <PublisherDisplayName>Christian Parpart</PublisherDisplayName>
    <Logo>Assets\StoreLogo.png</Logo>
  </Properties>

  <Dependencies>
    <TargetDeviceFamily Name="Windows.Desktop" MinVersion="10.0.19041.0" MaxVersionTested="10.0.22000.0" />
  </Dependencies>

  <Capabilities>
    <rescap:Capability Name="runFullTrust" />
  </Capabilities>

  <Applications>
    <Application Id="Contour" Executable="contour.exe" EntryPoint="Windows.FullTrustApplication">
      <uap:VisualElements
        DisplayName="Contour Debug"
        Description="Contour Terminal Debug"
        BackgroundColor="transparent"
        Square150x150Logo="Assets\Square150x150Logo.png"
        Square44x44Logo="Assets\Square44x44Logo.png">
      </uap:VisualElements>
    </Application>
  </Applications>
</Package>
"@

$DebugManifestPath = Join-Path $BinDir "AppxManifest_Debug.xml"
Set-Content -Path $DebugManifestPath -Value $MinimalManifest

try {
    Write-Host "Registering Minimal Manifest..."
    Add-AppxPackage -Register $DebugManifestPath -DisableDevelopmentMode -ErrorAction Stop
    Write-Host "SUCCESS: Minimal manifest registered!" -ForegroundColor Green
    Write-Host "Unregistering..."
    Remove-AppxPackage -Package "Contour.Terminal.Debug_1.0.0.0_x64__*" -ErrorAction SilentlyContinue
}
catch {
    Write-Error "FAILURE: Minimal manifest failed to register."
    Write-Error $_
}

Start-Sleep -Seconds 1
Remove-Item $DebugManifestPath -ErrorAction SilentlyContinue
Stop-Transcript
