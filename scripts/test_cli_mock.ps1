<#
    End-to-end check of dmb-cli: starts a tiny TCP/HTTP mock of the DeepSeek API,
    runs the CLI against a real reference .mid file, and verifies the generated
    .mid files.

    Usage: pwsh -File scripts/test_cli_mock.ps1
#>
param(
    [string] $Reference = "D:\zhuomian\deepseek\midi_demo\lost_butterfly\split_tracks\lost_butterfly_04_Bass.mid",
    [string] $Prompt = "根据贝斯轨写 4 小节 Drum & Bass 鼓组"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$cli = Get-ChildItem (Join-Path $root "build") -Recurse -Filter "dmb-cli.exe" -ErrorAction SilentlyContinue |
       Select-Object -First 1 -Expand FullName

if (-not $cli) { throw "dmb-cli.exe not found - build it first (scripts/build.ps1 -Target cli)" }
if (-not (Test-Path $Reference)) { throw "reference file not found: $Reference" }

$outDir = Join-Path $root "build\cli_test"
if (Test-Path $outDir) { Remove-Item $outDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

# ---- the answer the mock API will return -----------------------------------
$inner = [ordered]@{
    name        = "CLI测试鼓组"
    explanation = "跟随贝斯律动的切分鼓组"
    bpm         = 174
    bars        = 4
    unit        = "beats"
    tracks      = @(
        [ordered]@{
            name    = "Drums"
            channel = 10
            notes   = @(
                [ordered]@{ start = 0.0; pitch = 36; length = 0.25;  velocity = 120 },
                [ordered]@{ start = 0.5; pitch = 38; length = 0.25;  velocity = 110 },
                [ordered]@{ start = 1.0; pitch = 42; length = 0.125; velocity = 80  },
                [ordered]@{ start = 2.0; pitch = 36; length = 0.25;  velocity = 118 }
            )
        },
        [ordered]@{
            name    = "Bass"
            channel = 2
            notes   = @(
                [ordered]@{ start = 0.0; pitch = 40; length = 0.5; velocity = 100 },
                [ordered]@{ start = 1.5; pitch = 43; length = 0.5; velocity = 96  }
            )
        }
    )
}

$innerJson = $inner | ConvertTo-Json -Depth 10 -Compress

$responseObject = [ordered]@{
    id      = "chat-mock-1"
    choices = @([ordered]@{ index = 0; finish_reason = "stop"; message = [ordered]@{ role = "assistant"; content = $innerJson } })
    usage   = [ordered]@{ prompt_tokens = 321; completion_tokens = 123; total_tokens = 444 }
}

$responseJson  = $responseObject | ConvertTo-Json -Depth 10 -Compress
$responseBytes = [System.Text.Encoding]::UTF8.GetBytes($responseJson)

# ---- mock server -----------------------------------------------------------
$listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Loopback, 0)
$listener.Start()
$port = ([System.Net.IPEndPoint] $listener.LocalEndpoint).Port
Write-Host "mock api on http://127.0.0.1:$port/" -ForegroundColor Cyan

$stdoutPath = Join-Path $outDir "cli_stdout.txt"
$stderrPath = Join-Path $outDir "cli_stderr.txt"

# NOTE: Start-Process joins an array without quoting, so arguments containing spaces
# have to be quoted by hand here.
$argLine = '--reference "' + $Reference + '" --prompt "' + $Prompt + '"' +
           ' --endpoint "http://127.0.0.1:' + $port + '/v1/chat/completions"' +
           ' --api-key "mock-key" --out "' + $outDir + '" --bars 4' +
           ' --style "Drum & Bass" --instrument "鼓组"'

# .NET's Process is used directly: Start-Process -PassThru does not report ExitCode
# reliably in Windows PowerShell 5.1 when output is redirected.
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName               = $cli
$psi.Arguments              = $argLine
$psi.UseShellExecute        = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError  = $true
$psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
$psi.StandardErrorEncoding  = [System.Text.Encoding]::UTF8

$proc = New-Object System.Diagnostics.Process
$proc.StartInfo = $psi
$proc.Start() | Out-Null

$stdoutTask = $proc.StandardOutput.ReadToEndAsync()
$stderrTask = $proc.StandardError.ReadToEndAsync()

# serve exactly one request - with a deadline, and bail out early if the CLI died
$deadline = (Get-Date).AddSeconds(30)
$client = $null

while ((Get-Date) -lt $deadline) {
    if ($listener.Pending()) { $client = $listener.AcceptTcpClient(); break }
    if ($proc.HasExited) { break }
    Start-Sleep -Milliseconds 100
}

if (-not $client) {
    $listener.Stop()
    if (-not $proc.HasExited) { $proc.Kill(); $proc.WaitForExit() }
    Write-Host "FAIL: CLI never connected to the mock server (exit code $($proc.ExitCode))" -ForegroundColor Red
    Write-Host "--- cli stdout ---" -ForegroundColor Yellow
    $stdoutTask.Result -split "`r?`n" | Select-Object -First 6
    Write-Host "--- cli stderr ---" -ForegroundColor Yellow
    $stderrTask.Result -split "`r?`n" | Select-Object -First 6
    exit 1
}

$stream = $client.GetStream()
$stream.ReadTimeout = 15000

$buffer = New-Object byte[] 65536
$requestText = ""
$headerEnd = -1

while ($headerEnd -lt 0) {
    $read = $stream.Read($buffer, 0, $buffer.Length)
    if ($read -le 0) { break }
    $requestText += [System.Text.Encoding]::UTF8.GetString($buffer, 0, $read)
    $headerEnd = $requestText.IndexOf("`r`n`r`n")
}

$contentLength = 0
foreach ($line in ($requestText -split "`r`n")) {
    if ($line -match '^(?i)content-length:\s*(\d+)') { $contentLength = [int]$Matches[1] }
}

$bodySoFar = if ($headerEnd -ge 0) { $requestText.Substring($headerEnd + 4) } else { "" }

while ([System.Text.Encoding]::UTF8.GetByteCount($bodySoFar) -lt $contentLength) {
    $read = $stream.Read($buffer, 0, $buffer.Length)
    if ($read -le 0) { break }
    $bodySoFar += [System.Text.Encoding]::UTF8.GetString($buffer, 0, $read)
}

[System.IO.File]::WriteAllText((Join-Path $outDir "received_request.txt"), $requestText,
                               (New-Object System.Text.UTF8Encoding($false)))

$header = "HTTP/1.1 200 OK`r`nContent-Type: application/json`r`nContent-Length: $($responseBytes.Length)`r`nConnection: close`r`n`r`n"
$headerBytes = [System.Text.Encoding]::ASCII.GetBytes($header)
$stream.Write($headerBytes, 0, $headerBytes.Length)
$stream.Write($responseBytes, 0, $responseBytes.Length)
$stream.Flush()
$stream.Close()
$client.Close()
$listener.Stop()

if (-not $proc.WaitForExit(60000)) { $proc.Kill(); $proc.WaitForExit() }

$stdoutText = $stdoutTask.Result
$stderrText = $stderrTask.Result
$exitCode   = $proc.ExitCode

[System.IO.File]::WriteAllText($stdoutPath, $stdoutText, (New-Object System.Text.UTF8Encoding($false)))
[System.IO.File]::WriteAllText($stderrPath, $stderrText, (New-Object System.Text.UTF8Encoding($false)))

# ---- report ----------------------------------------------------------------
Write-Host "cli exit code: $exitCode" -ForegroundColor Cyan
$stdoutText -split "`r?`n" | Where-Object { $_ -ne "" } | ForEach-Object { Write-Host $_ }
if ($stderrText.Trim()) { Write-Host "--- stderr ---" -ForegroundColor Yellow; Write-Host $stderrText }

$midiFiles = Get-ChildItem $outDir -Filter *.mid
Write-Host "midi files written: $($midiFiles.Count)" -ForegroundColor Cyan
$midiFiles | ForEach-Object { Write-Host ("  {0}  ({1} bytes)" -f $_.Name, $_.Length) }

$ok = $true

if ($exitCode -ne 0)                { Write-Host "FAIL: exit code" -ForegroundColor Red; $ok = $false }
if ($midiFiles.Count -lt 3)         { Write-Host "FAIL: expected 3 .mid files" -ForegroundColor Red; $ok = $false }
if (-not ($midiFiles | Where-Object { $_.Name -notmatch '__' })) { Write-Host "FAIL: no combined file" -ForegroundColor Red; $ok = $false }

foreach ($file in $midiFiles) {
    $bytes = [System.IO.File]::ReadAllBytes($file.FullName)

    if ($bytes.Length -lt 14) { Write-Host "FAIL: $($file.Name) too small" -ForegroundColor Red; $ok = $false; continue }

    $magic = [System.Text.Encoding]::ASCII.GetString($bytes, 0, 4)
    if ($magic -ne "MThd") { Write-Host "FAIL: $($file.Name) is not a MIDI file" -ForegroundColor Red; $ok = $false }
}

if ($stdoutText -match 'CLI测试鼓组') {
    Write-Host "ok: the model supplied (Chinese) title made it into the output" -ForegroundColor Green
} else {
    Write-Host "WARN: could not find the Chinese title in the CLI output" -ForegroundColor Yellow
}

if ($stdoutText -notmatch '174\.0 BPM') {
    Write-Host "WARN: the BPM from the mock answer is not in the output" -ForegroundColor Yellow
}

Write-Host $(if ($ok) { "CLI END-TO-END: PASS" } else { "CLI END-TO-END: FAIL" }) -ForegroundColor $(if ($ok) { "Green" } else { "Red" })
exit $(if ($ok) { 0 } else { 1 })
