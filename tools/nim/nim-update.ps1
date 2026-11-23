param(
    [Parameter(Mandatory=$true)][version]$Version,
    [Parameter(Mandatory=$true)][int]$ViewerPid,
    [switch]$Automatic
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$dir = $PSScriptRoot
$viewerPath = Join-Path $dir 'nim.exe'
$statusPath = Join-Path $dir 'nim-update-status.txt'
$api = 'https://api.github.com/repos/bata-san/nim'
function Set-Status([string]$value) { [IO.File]::WriteAllText($statusPath, $value) }

try {
    $lock = [IO.File]::Open((Join-Path $dir '.nim-update.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
} catch { exit 0 }
$stage = $null
$keepBackup = $false
$phase = 'check'
try {
    # Open the process handle before querying GitHub, so a reused PID cannot authorize an update.
    $viewer = Get-Process -Id $ViewerPid
    if ($viewer.Path -ne $viewerPath) { throw 'Unexpected viewer process.' }
    $null = $viewer.Handle
    Set-Status 'checking'
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $token = $env:NIM_GITHUB_TOKEN
    if (!$token) {
        $git = Get-Command git.exe -CommandType Application -ErrorAction SilentlyContinue
        if ($git) {
            $env:GIT_TERMINAL_PROMPT = '0'
            $env:GCM_INTERACTIVE = 'Never'
            $cred = "protocol=https`nhost=github.com`npath=bata-san/nim.git`n`n" |
                & $git.Source -C $env:SystemRoot -c credential.interactive=false credential fill 2>$null |
                Where-Object { $_ -like 'password=*' } | Select-Object -First 1
            if ($cred) { $token = $cred.Substring(9) }
        }
    }
    if (!$token) { Set-Status 'auth'; exit 2 }
    $headers = @{
        Authorization = "Bearer $token"
        'User-Agent' = 'nim-updater'
        Accept = 'application/vnd.github+json'
        'X-GitHub-Api-Version' = '2022-11-28'
    }
    try { $release = Invoke-RestMethod "$api/releases/latest" -Headers $headers -TimeoutSec 25 }
    catch {
        if ($_.Exception.Response -and [int]$_.Exception.Response.StatusCode -in 401,403,404) {
            Set-Status 'auth'
            exit 2
        }
        throw
    }
    if ($release.tag_name -notmatch '^v(\d+\.\d+\.\d+)$') { throw 'Invalid release tag.' }
    $next = [version]$Matches[1]
    if ($next -le $Version) { Set-Status 'current'; exit 0 }
    $asset = @($release.assets | Where-Object { $_.name -ceq "nim-v$next-win64.zip" -and $_.state -eq 'uploaded' })
    if ($asset.Count -ne 1 -or $asset[0].digest -notmatch '^sha256:([0-9a-fA-F]{64})$') { throw 'Invalid release asset.' }
    $digest = $Matches[1]
    $asset = $asset[0]
    if ($asset.size -le 0 -or $asset.size -gt 200MB) { throw 'Invalid release size.' }

    $phase = 'download'
    Set-Status "downloading $next"
    $stage = Join-Path $dir ('.nim-update-' + [guid]::NewGuid())
    $null = [IO.Directory]::CreateDirectory($stage)
    $zip = Join-Path $stage 'release.zip'
    Add-Type -AssemblyName System.Net.Http
    $handler = New-Object Net.Http.HttpClientHandler
    $handler.AllowAutoRedirect = $false
    $client = [Net.Http.HttpClient]::new($handler)
    $client.Timeout = [TimeSpan]::FromMinutes(4)
    $client.MaxResponseContentBufferSize = $asset.size
    try {
        $request = [Net.Http.HttpRequestMessage]::new('GET', "$api/releases/assets/$([long]$asset.id)")
        $request.Headers.Add('Authorization', "Bearer $token")
        $request.Headers.Add('User-Agent', 'nim-updater')
        $request.Headers.Add('Accept', 'application/octet-stream')
        try { $response = $client.SendAsync($request).GetAwaiter().GetResult() } finally { $request.Dispose() }
        if ([int]$response.StatusCode -eq 302) {
            $url = $response.Headers.Location
            $response.Dispose()
            if (!$url.IsAbsoluteUri -or $url.Scheme -ne 'https' -or
                $url.Host -notmatch '^(release-assets|objects)\.githubusercontent\.com$') { throw 'Untrusted asset redirect.' }
            # The redirected request uses GitHub's temporary URL without forwarding the token.
            $response = $client.GetAsync($url).GetAwaiter().GetResult()
        }
        try {
            $null = $response.EnsureSuccessStatusCode()
            $bytes = $response.Content.ReadAsByteArrayAsync().GetAwaiter().GetResult()
            if ($bytes.LongLength -ne $asset.size) { throw 'Asset size mismatch.' }
            [IO.File]::WriteAllBytes($zip, $bytes)
        } finally { $response.Dispose() }
    } finally { $client.Dispose() }
    if ((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ne $digest) { throw 'Asset digest mismatch.' }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $required = @('nim-update.ps1', 'nim.exe')
    $files = @('README.md', 'LICENSE-imgui.txt') + $required
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        foreach ($name in $files) {
            $entry = $archive.GetEntry($name)
            if (!$entry -and $name -notin $required) { continue }
            if (!$entry -or $entry.Length -le 0 -or $entry.Length -gt 64MB) { throw 'Invalid package entry.' }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $stage $name))
        }
    } finally { $archive.Dispose() }

    Set-Status "ready $next"
    $viewer.WaitForExit()
    if ($Automatic -and !(Select-String -LiteralPath (Join-Path $dir 'nim_settings.ini') -Pattern '^auto_update=1$' -Quiet)) {
        Set-Status 'cancelled'
        exit 0
    }
    if (Get-Process nim -ErrorAction SilentlyContinue | Where-Object Path -EQ $viewerPath) {
        Set-Status 'running'
        exit 3
    }
    $phase = 'install'
    $changed = @()
    try {
        foreach ($name in $files) {
            $source = Join-Path $stage $name
            if (![IO.File]::Exists($source)) { continue }
            $destination = Join-Path $dir $name
            $backup = Join-Path $stage "$name.previous"
            if ([IO.File]::Exists($destination)) { [IO.File]::Replace($source, $destination, $backup) }
            else { [IO.File]::Move($source, $destination) }
            $changed += $name
        }
    } catch {
        $keepBackup = $true
        [array]::Reverse($changed)
        foreach ($name in $changed) {
            $destination = Join-Path $dir $name
            $backup = Join-Path $stage "$name.previous"
            if ([IO.File]::Exists($backup)) { [IO.File]::Replace($backup, $destination, $null) }
            else { [IO.File]::Delete($destination) }
        }
        $keepBackup = $false
        throw
    }
    Set-Status "installed $next"
} catch {
    Set-Status "error $phase"
    exit 1
} finally {
    if (!$keepBackup -and $stage -and (Test-Path -LiteralPath $stage)) {
        # The directory name is generated above. Remove only the known files.
        Get-ChildItem -LiteralPath $stage -File | Remove-Item -ErrorAction SilentlyContinue
        [IO.Directory]::Delete($stage)
    }
    $lock.Dispose()
}
