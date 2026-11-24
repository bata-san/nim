param(
    [Parameter(Mandatory=$true)][version]$Version,
    [Parameter(Mandatory=$true)][int]$ViewerPid,
    [switch]$Automatic
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$dir = $PSScriptRoot
$viewerPath = [IO.Path]::GetFullPath((Join-Path $dir 'nim.exe'))
$statusPath = Join-Path $dir 'nim-update-status.txt'
$logPath = Join-Path $dir 'nim-update.log'
$relaunchPath = Join-Path $dir '.nim-update-relaunch'
$api = 'https://api.github.com/repos/bata-san/nim'
function Set-Status([string]$value) { [IO.File]::WriteAllText($statusPath, $value) }
function Write-Log([string]$msg) {
    try { [IO.File]::AppendAllText($logPath, "$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss') $msg`r`n") } catch {}
}
function Replace-WithRetry([string]$source, [string]$destination, [string]$backup) {
    for ($attempt = 1; $attempt -le 30; $attempt++) {
        try {
            if ([IO.File]::Exists($destination)) { [IO.File]::Replace($source, $destination, $backup) }
            else { [IO.File]::Move($source, $destination) }
            return
        } catch [IO.IOException], [UnauthorizedAccessException] {
            if ($attempt -eq 30) { throw }
            Start-Sleep -Milliseconds 200
        }
    }
}

try {
    $lock = [IO.File]::Open((Join-Path $dir '.nim-update.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
} catch { exit 0 }
$stage = $null
$keepBackup = $false
$phase = 'check'
try {
    # Open the process handle before querying GitHub, so a reused PID cannot authorize an update.
    $viewer = Get-Process -Id $ViewerPid
    if ([IO.Path]::GetFullPath($viewer.Path) -ine $viewerPath) { throw "Unexpected viewer process: $($viewer.Path)" }
    $null = $viewer.Handle
    Set-Status 'checking'
    Write-Log "check start current=$Version pid=$ViewerPid auto=$Automatic"
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
    if (!$token) {
        $gh = Get-Command gh.exe -CommandType Application -ErrorAction SilentlyContinue
        if ($gh) {
            $ghToken = & $gh.Source auth token 2>$null | Select-Object -First 1
            if ($ghToken) { $token = $ghToken.Trim() }
        }
    }
    # The repository is public: a token only raises the API rate limit.
    if (!$token) { Write-Log 'no token: anonymous request' }
    $headers = @{
        'User-Agent' = 'nim-updater'
        Accept = 'application/vnd.github+json'
        'X-GitHub-Api-Version' = '2022-11-28'
    }
    if ($token) { $headers.Authorization = "Bearer $token" }
    try { $release = Invoke-RestMethod "$api/releases/latest" -Headers $headers -TimeoutSec 25 }
    catch {
        if ($_.Exception.Response -and [int]$_.Exception.Response.StatusCode -in 401,403,404) {
            Set-Status 'auth'
            Write-Log "auth: HTTP $([int]$_.Exception.Response.StatusCode)"
            exit 2
        }
        throw
    }
    if ($release.tag_name -notmatch '^v(\d+\.\d+\.\d+)$') { throw "Invalid release tag: $($release.tag_name)" }
    $next = [version]$Matches[1]
    # Releases up to v0.3.15 shipped without the NVIDIA runtime DLLs; fetch the current package once
    # more when they are missing so DLSS can load.
    $repair = $next -eq $Version -and ![IO.File]::Exists((Join-Path $dir 'nvngx_dlss.dll'))
    if ($next -lt $Version -or ($next -eq $Version -and !$repair)) { Set-Status 'current'; Write-Log "current: latest=$next"; exit 0 }
    if ($repair) { Write-Log "repair: $next is current but nvngx_dlss.dll is missing" }
    $asset = @($release.assets | Where-Object { $_.name -ceq "nim-v$next-win64.zip" -and $_.state -eq 'uploaded' })
    if ($asset.Count -ne 1 -or $asset[0].digest -notmatch '^sha256:([0-9a-fA-F]{64})$') { throw 'Invalid release asset.' }
    $digest = $Matches[1]
    $asset = $asset[0]
    if ($asset.size -le 0 -or $asset.size -gt 200MB) { throw 'Invalid release size.' }

    $phase = 'download'
    Set-Status "downloading $next"
    Write-Log "downloading $next ($($asset.size) bytes)"
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
        if ($token) { $request.Headers.Add('Authorization', "Bearer $token") }
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
    $files = @('README.md', 'LICENSE-imgui.txt', 'nvngx_dlss.dll', 'nvngx_dlssg.dll', 'sl.interposer.dll', 'sl.common.dll',
               'sl.dlss_g.dll', 'sl.reflex.dll', 'sl.pcl.dll') + $required
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        foreach ($name in $files) {
            $entry = $archive.GetEntry($name)
            if (!$entry -and $name -notin $required) { continue }
            if (!$entry -or $entry.Length -le 0 -or $entry.Length -gt 128MB) { throw "Invalid package entry: $name" }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, (Join-Path $stage $name))
        }
    } finally { $archive.Dispose() }

    Set-Status "ready $next"
    Write-Log "ready $next; waiting for viewer exit"
    $viewer.WaitForExit()
    Start-Sleep -Milliseconds 250
    $settingsPath = Join-Path $dir 'nim_settings.ini'
    if ($Automatic -and (!(Test-Path -LiteralPath $settingsPath) -or !(Select-String -LiteralPath $settingsPath -Pattern '^auto_update=1$' -Quiet))) {
        Set-Status 'cancelled'
        Write-Log 'cancelled: auto_update disabled'
        exit 0
    }
    # Wait briefly for any other nim.exe instance at the same path to finish closing.
    for ($w = 0; $w -lt 25; $w++) {
        $others = @(Get-Process nim -ErrorAction SilentlyContinue | Where-Object { $_.Path -and ([IO.Path]::GetFullPath($_.Path) -ieq $viewerPath) })
        if ($others.Count -eq 0) { break }
        Start-Sleep -Milliseconds 200
    }
    $others = @(Get-Process nim -ErrorAction SilentlyContinue | Where-Object { $_.Path -and ([IO.Path]::GetFullPath($_.Path) -ieq $viewerPath) })
    if ($others.Count -gt 0) {
        Set-Status 'running'
        Write-Log 'running: another nim.exe at the same path is still open'
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
            Replace-WithRetry $source $destination $backup
            $changed += $name
        }
    } catch {
        $keepBackup = $true
        Write-Log "install failed: $($_.Exception.Message); rolling back"
        [array]::Reverse($changed)
        foreach ($name in $changed) {
            $destination = Join-Path $dir $name
            $backup = Join-Path $stage "$name.previous"
            if ([IO.File]::Exists($backup)) { Replace-WithRetry $backup $destination $null }
            elseif ([IO.File]::Exists($destination)) { [IO.File]::Delete($destination) }
        }
        $keepBackup = $false
        throw
    }
    Set-Status "installed $next"
    Write-Log "installed $next"
    if ([IO.File]::Exists($relaunchPath)) {
        try { [IO.File]::Delete($relaunchPath) } catch {}
        Write-Log "relaunching $viewerPath"
        Start-Process -FilePath $viewerPath -WorkingDirectory $dir
    }
} catch {
    Set-Status "error $phase"
    Write-Log "error $phase : $($_.Exception.Message)"
    exit 1
} finally {
    if (!$keepBackup -and $stage -and (Test-Path -LiteralPath $stage)) {
        # The directory name is generated above. Remove only the known files.
        Get-ChildItem -LiteralPath $stage -File | Remove-Item -ErrorAction SilentlyContinue
        try { [IO.Directory]::Delete($stage) } catch {}
    }
    $lock.Dispose()
}
