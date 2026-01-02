# Register-ContourPackage.ps1
# Helper script to register Contour as a Sparse Package
# REQUIRES: The AppxManifest.xml must be signed with a trusted certificate before running this.

param(
    [string]$ManifestPath = "$PSScriptRoot\AppxManifest.xml"
)

if (-not (Test-Path $ManifestPath)) {
    Write-Error "Manifest not found at $ManifestPath"
    exit 1
}

Write-Host "Registering Contour Sparse Package from: $ManifestPath" -ForegroundColor Cyan

# NOTE: Add-AppxPackage -Register requires the manifest to be signed.
# If the signature is missing or untrusted, this command will fail.
try {
    Add-AppxPackage -Register $ManifestPath -DisableDevelopmentMode -ErrorAction Stop
    Write-Host "Successfully registered Contour package!" -ForegroundColor Green
}
catch {
    Write-Error "Failed to register package. Ensure AppxManifest.xml is signed with a trusted certificate."
    Write-Error $_
    exit 1
}
