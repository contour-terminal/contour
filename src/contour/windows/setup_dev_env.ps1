# basic_sign_test.ps1
# Helper to set up a testing environment for Sparse Packages
# 1. Creates a Self-Signed Certificate
# 2. Installs it to Trusted People
# 3. Signs the Package (technically, creates a signed signature file)

param(
    [string]$Action = "Sign", # "Sign" or "Setup"
    [string]$ManifestPath = "bin\AppxManifest.xml"
)

$CertName = "ContourDevCert"
$Publisher = "CN=Christian Parpart"

function Setup-Certificate {
    Write-Host "Checking for existing certificate..." -ForegroundColor Cyan
    $cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $Publisher } | Select-Object -First 1

    if (-not $cert) {
        Write-Host "Creating new self-signed certificate..." -ForegroundColor Yellow
        $cert = New-SelfSignedCertificate -Type Custom -Subject $Publisher `
            -KeyUsage DigitalSignature `
            -FriendlyName "Contour Dev Certificate" `
            -CertStoreLocation "Cert:\CurrentUser\My" `
            -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
    }

    Write-Host "Certificate thumbprint: $($cert.Thumbprint)" -ForegroundColor Green

    Write-Host "Installing certificate to Trusted People store (Requires Admin)..." -ForegroundColor Yellow

    # Export public key
    $certPath = "$env:TEMP\ContourDevCert.cer"
    Export-Certificate -Cert $cert -FilePath $certPath -Type CERT

    # Import to Trusted People
    try {
        Import-Certificate -FilePath $certPath -CertStoreLocation Cert:\LocalMachine\TrustedPeople
        Write-Host "Certificate installed to Trusted People." -ForegroundColor Green
    }
    catch {
        Write-Error "Failed to install certificate. Please run this script as Administrator."
        exit 1
    }

    return $cert
}

function Sign-Package {
    param($Certificate)

    # Sparse Package signing is tricky.
    # The 'proper' way is to use `signtool` on a package, but we are unpackaged.
    #
    # WORKAROUND for Testing:
    # 1. Enable Developer Mode in Windows Settings.
    # 2. Register WITHOUT signing.

    Write-Host "`n=== SIGNING GUIDE ===" -ForegroundColor Cyan
    Write-Host "For Sparse Packages (Unpackaged), the easiest way to test is enabling 'Developer Mode'."
    Write-Host "Settings > Privacy & security > For developers > Developer Mode (ON)"
    Write-Host "--------------------------------------------------------"
    Write-Host "If Developer Mode is ON, you do NOT need to sign the manifest manually."
    Write-Host "Just run: .\register_package.ps1"
    Write-Host "--------------------------------------------------------"

    # If the user REALLY wants to sign, they need to sign the binaries and have a catalog.
    # But simply signing the manifest file doesn't work with SignTool alone.
}

# Main execution
if (-not (Test-Path $ManifestPath)) {
    # Try looking in src/contour/windows if not in bin
    if (Test-Path "src\contour\windows\AppxManifest.xml") {
        $ManifestPath = "src\contour\windows\AppxManifest.xml"
    }
}

try {
    $cert = Setup-Certificate
    Sign-Package -Certificate $cert
}
catch {
    Write-Error $_
}
