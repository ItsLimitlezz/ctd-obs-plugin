<#
Sets up macOS signing and notarization for CI from a Windows PC, without a Mac (Keychain Access isn't needed).

1. .\tools\macos-signing.ps1 -Request
   Makes a signing key (kept in this Windows account's key store, as "ctd-obs-plugin-macos-signing") and writes
   ctd-signing.csr. At developer.apple.com > Certificates, make a "Developer ID Application" and a "Developer ID
   Installer" certificate (G2 Sub-CA), uploading that .csr for both, and download the two .cer files.

2. .\tools\macos-signing.ps1 -Application <application .cer> -Installer <installer .cer>
   Puts the key and both certificates in a .p12 with a random password, asks for the Apple ID and an app-specific
   password (account.apple.com > Sign-In and Security > App-Specific Passwords) for notarization, and stores it all as
   the repository's MACOS_* secrets with the GitHub CLI. Nothing is printed or written to disk.
#>
param(
	[switch]$Request,
	[string]$Application,
	[string]$Installer,
	[string]$Repo = 'ItsLimitlezz/ctd-obs-plugin'
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Security
$keyName = 'ctd-obs-plugin-macos-signing'
$sha256 = [System.Security.Cryptography.HashAlgorithmName]::SHA256
$pkcs1 = [System.Security.Cryptography.RSASignaturePadding]::Pkcs1

function Open-Key {
	if (-not [System.Security.Cryptography.CngKey]::Exists($keyName)) {
		throw "No signing key yet: run with -Request first"
	}
	[System.Security.Cryptography.RSACng]::new([System.Security.Cryptography.CngKey]::Open($keyName))
}

function Get-Plain([string]$prompt) {
	$secure = Read-Host -AsSecureString $prompt
	$ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
	try { [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr) } finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr) }
}

function New-Password {
	$bytes = New-Object byte[] 24
	[System.Security.Cryptography.RandomNumberGenerator]::Create().GetBytes($bytes)
	[Convert]::ToBase64String($bytes)
}

if ($Request) {
	if ([System.Security.Cryptography.CngKey]::Exists($keyName)) {
		Write-Host "Using the signing key made earlier ($keyName)"
	} else {
		$parameters = New-Object System.Security.Cryptography.CngKeyCreationParameters
		$parameters.ExportPolicy = [System.Security.Cryptography.CngExportPolicies]::AllowExport
		$parameters.Parameters.Add((New-Object System.Security.Cryptography.CngProperty('Length',
						[BitConverter]::GetBytes(2048), [System.Security.Cryptography.CngPropertyOptions]::None)))
		[void][System.Security.Cryptography.CngKey]::Create([System.Security.Cryptography.CngAlgorithm]::Rsa, $keyName, $parameters)
	}
	$rsa = Open-Key
	$csr = [System.Security.Cryptography.X509Certificates.CertificateRequest]::new('CN=CTD OBS plugin', $rsa, $sha256, $pkcs1)
	$base64 = [Convert]::ToBase64String($csr.CreateSigningRequest(), 'InsertLineBreaks')
	$path = Join-Path (Get-Location) 'ctd-signing.csr'
	Set-Content -Path $path -Encoding ascii -Value "-----BEGIN CERTIFICATE REQUEST-----`r`n$base64`r`n-----END CERTIFICATE REQUEST-----"
	Write-Host "Wrote $path. Upload it to Apple for both Developer ID certificates, then run step 2."
	return
}

if (-not $Application -or -not $Installer) {
	throw 'Run with -Request first, then with -Application <.cer> -Installer <.cer>'
}
$gh = (Get-Command gh -ErrorAction SilentlyContinue).Source
if (-not $gh) { $gh = Join-Path $env:LOCALAPPDATA 'Programs\gh\bin\gh.exe' }

$rsa = Open-Key
$extensions = [System.Security.Cryptography.X509Certificates.RSACertificateExtensions]
$certs = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2Collection
$identities = @{}
foreach ($kind in 'Application', 'Installer') {
	$path = if ($kind -eq 'Application') { $Application } else { $Installer }
	$cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new((Resolve-Path $path).Path)
	$name = $cert.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false)
	if ($name -notlike "Developer ID ${kind}:*") { throw "$path is '$name', not a Developer ID $kind certificate" }
	[void]$certs.Add($extensions::CopyWithPrivateKey($cert, $rsa)) # throws if it isn't for this key
	$identities[$kind] = $name
	$issuer = $cert.Issuer
	Write-Host "$kind certificate: $name (valid until $($cert.NotAfter.ToString('yyyy-MM-dd')))"
}
# Apple's intermediate, so codesign can build the chain whatever the CI image has installed
$intermediate = if ($issuer -like '*G2*') { 'DeveloperIDG2CA.cer' } else { 'DeveloperIDCA.cer' }
try {
	$bytes = (Invoke-WebRequest -UseBasicParsing "https://www.apple.com/certificateauthority/$intermediate").Content
	[void]$certs.Add([System.Security.Cryptography.X509Certificates.X509Certificate2]::new([byte[]]$bytes))
	Write-Host "Added Apple's $intermediate"
} catch {
	Write-Warning "Couldn't download Apple's $intermediate ($_); signing will rely on the CI image having it"
}

$p12Password = New-Password
$p12 = [Convert]::ToBase64String($certs.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Pkcs12, $p12Password))
$appleId = Read-Host 'Apple ID (email) for notarization'
$appPassword = Get-Plain 'App-specific password for that Apple ID'

$secrets = [ordered]@{
	MACOS_SIGNING_APPLICATION_IDENTITY = $identities['Application']
	MACOS_SIGNING_INSTALLER_IDENTITY   = $identities['Installer']
	MACOS_SIGNING_CERT                 = $p12
	MACOS_SIGNING_CERT_PASSWORD        = $p12Password
	MACOS_KEYCHAIN_PASSWORD            = New-Password
	MACOS_NOTARIZATION_USERNAME        = $appleId
	MACOS_NOTARIZATION_PASSWORD        = $appPassword
}
foreach ($name in $secrets.Keys) {
	& $gh secret set $name -R $Repo --body $secrets[$name] | Out-Null
	if ($LASTEXITCODE -ne 0) { throw "couldn't set $name" }
	Write-Host "set $name"
}
Write-Host "Done. The next push to main builds a signed .pkg; a version tag (e.g. 0.1.0) also notarizes it."
