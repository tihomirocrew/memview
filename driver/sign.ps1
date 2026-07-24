[CmdletBinding()]
param(
    [string]$Subject = "CN=MemView Test Cert",
    [string]$SysPath
)

$ErrorActionPreference = "Stop"

# $PSScriptRoot can be empty here when run via `powershell -File`; fall back to $MyInvocation.
if (-not $SysPath)
{
    $scriptDir = $PSScriptRoot
    if (-not $scriptDir)
    {
        $scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
    }
    $SysPath = Join-Path $scriptDir "build\memview.sys"
}

try
{
    # VsDevCmd.bat (build.cmd uses it for cl/ninja) can pollute PSModulePath enough
    # to break Import-Module below; reset it to the persisted Machine/User values.
    $env:PSModulePath = @(
        [Environment]::GetEnvironmentVariable("PSModulePath", "Machine")
        [Environment]::GetEnvironmentVariable("PSModulePath", "User")
    ) -join ";"

    try
    {
        Import-Module Microsoft.PowerShell.Security -ErrorAction Stop
        Import-Module PKI -ErrorAction Stop
    }
    catch
    {
        # Fallback: relax $ErrorActionPreference itself, since the type-data
        # loader ignores plain -ErrorAction, then just check the result below.
        $previousEap = $ErrorActionPreference
        $ErrorActionPreference = "SilentlyContinue"
        try
        {
            Import-Module Microsoft.PowerShell.Security -ErrorAction SilentlyContinue -WarningAction SilentlyContinue
            Import-Module PKI -ErrorAction SilentlyContinue -WarningAction SilentlyContinue
        }
        finally
        {
            $ErrorActionPreference = $previousEap
        }
    }

    if (-not (Get-PSDrive -Name Cert -ErrorAction SilentlyContinue))
    {
        throw "Could not load the Certificate provider (the 'Cert:' PSDrive)."
    }
    if (-not (Get-Command New-SelfSignedCertificate -ErrorAction SilentlyContinue))
    {
        throw "Could not load New-SelfSignedCertificate (the 'PKI' module)."
    }

    $identity  = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator))
    {
        throw "Run this from an elevated PowerShell - it needs to write to the LocalMachine certificate stores."
    }

    if (-not (Test-Path $SysPath))
    {
        throw "$SysPath not found - build the driver first (driver\build.cmd)."
    }

    # Reuse an existing certificate rather than creating a new one every run.
    $myStore = [Security.Cryptography.X509Certificates.X509Store]::new("My", "LocalMachine")
    $myStore.Open("ReadOnly")
    $cert = $myStore.Certificates | Where-Object { $_.Subject -eq $Subject } | Select-Object -First 1
    $myStore.Close()

    if (-not $cert)
    {
        Write-Host "Creating test certificate '$Subject' in LocalMachine\My..."
        $cert = New-SelfSignedCertificate `
            -Type Custom `
            -Subject $Subject `
            -KeyUsage DigitalSignature `
            -FriendlyName "MemView driver test signing" `
            -CertStoreLocation Cert:\LocalMachine\My `
            -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}") `
            -NotAfter (Get-Date).AddYears(10)
    }

    # Self-signed, so it needs to go in both: Root as a trust anchor, TrustedPublisher
    # so a signature from it doesn't prompt.
    foreach ($storeName in "Root", "TrustedPublisher")
    {
        $store = [Security.Cryptography.X509Certificates.X509Store]::new($storeName, "LocalMachine")
        $store.Open("ReadWrite")
        if (-not ($store.Certificates | Where-Object { $_.Thumbprint -eq $cert.Thumbprint }))
        {
            Write-Host "Adding certificate to LocalMachine\$storeName..."
            $store.Add($cert)
        }
        $store.Close()
    }

    $signtool = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($signtool)
    {
        $signtool = $signtool.Source
    }
    else
    {
        $found = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
            Sort-Object FullName -Descending |
            Select-Object -First 1
        if (-not $found)
        {
            throw "signtool.exe not found. Run from a 'x64 Native Tools Command Prompt for VS', or install the Windows SDK."
        }
        $signtool = $found.FullName
    }

    # By thumbprint, not subject name: signtool's /n only matches the bare CN.
    Write-Host "Signing $SysPath..."
    & $signtool sign /v /sm /s My /sha1 $cert.Thumbprint /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 $SysPath
    if ($LASTEXITCODE -ne 0)
    {
        Write-Warning "Timestamped signing failed (no network?) - retrying without a timestamp."
        & $signtool sign /v /sm /s My /sha1 $cert.Thumbprint /fd SHA256 $SysPath
        if ($LASTEXITCODE -ne 0)
        {
            throw "signtool failed."
        }
    }

    Write-Host ""
    Write-Host "Done: $SysPath is test-signed with '$Subject'."
    Write-Host "Make sure test signing is on: bcdedit /set testsigning on (needs a reboot to take effect)."
}
catch
{
    # exit 1 explicitly: a throw alone doesn't reliably set the process exit code.
    Write-Error $_
    exit 1
}
