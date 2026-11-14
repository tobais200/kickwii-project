param([Parameter(Mandatory=$true)][string]$SD)
$ErrorActionPreference = 'Stop'
$folder = Join-Path $SD 'apps\KickWii'
New-Item -ItemType Directory -Force $folder | Out-Null
$id = (Read-Host 'Client ID de TU app Kick').Trim()
if ($id -notmatch '^[A-Za-z0-9_-]{4,128}$') { throw 'Client ID invalido' }
$secure = Read-Host 'Client Secret de TU app Kick (no se mostrara)' -AsSecureString
$secret = [System.Net.NetworkCredential]::new('', $secure).Password
if ($secret -notmatch '^[!-~]{1,256}$') { throw 'Client Secret invalido' }
[IO.File]::WriteAllText((Join-Path $folder 'oauth.ini'), "client_id=$id`nclient_secret=$secret`n", [Text.Encoding]::ASCII)
$seedPath = Join-Path $folder 'pkce-seed.bin'
if (-not (Test-Path $seedPath)) {
    $seed = New-Object byte[] 32
    $rng = [Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($seed); [IO.File]::WriteAllBytes($seedPath, $seed) }
    finally { $rng.Dispose(); [Array]::Clear($seed, 0, $seed.Length) }
}
$secret = $null
Write-Host 'Listo: configuracion privada y semilla PKCE en tu SD. No compartas esa carpeta.'
