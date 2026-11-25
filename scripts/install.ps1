$ErrorActionPreference = "Stop"

$repo = if ($env:SABLE_REPO) { $env:SABLE_REPO } else { "ak495867/Sable" }
$tag = if ($env:SABLE_VERSION) { $env:SABLE_VERSION } else { "latest" }

$apiUrl = if ($tag -eq "latest") {
    "https://api.github.com/repos/$repo/releases/latest"
} else {
    "https://api.github.com/repos/$repo/releases/tags/$tag"
}

Write-Host "Querying release from $apiUrl..."
$headers = @{ "User-Agent" = "Sable-Installer" }
if ($env:GITHUB_TOKEN) {
    $headers["Authorization"] = "Bearer $env:GITHUB_TOKEN"
}

$release = Invoke-RestMethod -Uri $apiUrl -Headers $headers
$targetAsset = $release.assets | Where-Object { $_.name -like "*windows-x86_64.zip" } | Select-Object -First 1

if (-not $targetAsset) {
    Write-Error "Could not find a Windows release asset in release $($release.tag_name)"
    exit 1
}

$downloadUrl = $targetAsset.browser_download_url
$installDir = Join-Path $env:LOCALAPPDATA "sable\bin"
if (-not (Test-Path $installDir)) {
    New-Item -ItemType Directory -Path $installDir -Force | Out-Null
}

$zipPath = Join-Path $env:TEMP "sable-windows-x86_64.zip"
Write-Host "Downloading $($targetAsset.name)..."
Invoke-WebRequest -Uri $downloadUrl -OutFile $zipPath -Headers $headers

Write-Host "Extracting to $installDir..."
Expand-Archive -Path $zipPath -DestinationPath $installDir -Force
Remove-Item -Force $zipPath

$userPath = [Environment]::GetEnvironmentVariable("Path", [EnvironmentVariableTarget]::User)
if ($userPath -split ';' -notcontains $installDir) {
    Write-Host "Adding $installDir to User PATH..."
    [Environment]::SetEnvironmentVariable("Path", "$userPath;$installDir", [EnvironmentVariableTarget]::User)
    $env:Path += ";$installDir"
}

$exePath = Join-Path $installDir "sable.exe"
if (Test-Path $exePath) {
    Write-Host "Successfully installed SABLE!" -ForegroundColor Green
    & $exePath --version
} else {
    Write-Error "Failed to locate sable.exe in $installDir"
    exit 1
}
