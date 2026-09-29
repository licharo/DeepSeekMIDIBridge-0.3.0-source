<#
    Wraps every string literal that contains non-ASCII (UTF-8) text in dmb::utf8(...),
    because juce::String's const char* constructor only handles ASCII.

        "贝斯"                ->  dmb::utf8 ("贝斯")
        "a" "b中文"           ->  dmb::utf8 ("a" "b中文")      (adjacent literals are grouped)
        L"..."  / R"(...)"    ->  left alone

    Run once; it is idempotent because already wrapped literals are detected by the
    surrounding dmb::utf8 ( .
#>
param(
    [string] $Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = "Stop"

$sourceDir = Join-Path $Root "source"
$files = Get-ChildItem $sourceDir -Recurse -Include *.cpp, *.h

$literalRegex = [regex]'(?<![\w])(?:(u8|u|U|L))?("(?:[^"\\]|\\.)*")'

function Test-NonAscii ([string] $text) {
    foreach ($ch in $text.ToCharArray()) {
        if ([int][char]$ch -gt 127) { return $true }
    }
    return $false
}

$totalWrapped = 0

foreach ($file in $files) {
    $text = [System.IO.File]::ReadAllText($file.FullName, [System.Text.Encoding]::UTF8)
    $lines = $text -split "`r?`n"
    $result = New-Object System.Collections.Generic.List[string]
    $inRawString = $false
    $fileWrapped = 0

    foreach ($line in $lines) {
        $prefix = ""
        $work = $line

        if ($inRawString) {
            $end = $line.IndexOf(')"')
            if ($end -lt 0) { $result.Add($line); continue }
            $prefix = $line.Substring(0, $end + 2)
            $work = $line.Substring($end + 2)
            $inRawString = $false
        } else {
            # detect the start of a raw string literal
            $rawStart = $line.IndexOf('R"(')
            if ($rawStart -ge 0 -and $line.IndexOf(')"', $rawStart) -lt 0) {
                $inRawString = $true
                $result.Add($line)
                continue
            }
        }

        $matches = $literalRegex.Matches($work)
        if ($matches.Count -eq 0) { $result.Add($prefix + $work); continue }

        # group adjacent literals that are only separated by whitespace
        $groups = New-Object System.Collections.Generic.List[object]
        $current = $null

        foreach ($m in $matches) {
            $isNonAscii = (Test-NonAscii $m.Groups[2].Value)
            $prefixed = ($m.Groups[1].Value -ne "")

            if ($null -ne $current -and -not $prefixed) {
                $between = $work.Substring($current.End, $m.Index - $current.End)
                if ($between -match '^\s*$') {
                    $current.End = $m.Index + $m.Length
                    $current.NonAscii = $current.NonAscii -or $isNonAscii
                    continue
                }
            }

            if ($null -ne $current) { $groups.Add($current) }
            $current = [pscustomobject]@{ Start = $m.Index; End = $m.Index + $m.Length; NonAscii = $isNonAscii; Prefixed = $prefixed }
        }

        if ($null -ne $current) { $groups.Add($current) }

        $newWork = $work

        for ($i = $groups.Count - 1; $i -ge 0; $i--) {
            $g = $groups[$i]
            if (-not $g.NonAscii -or $g.Prefixed) { continue }

            # skip literals that are already inside a dmb::utf8 ( or juce::String ( ... call
            $lookBehind = $newWork.Substring([Math]::Max(0, $g.Start - 22), [Math]::Min(22, $g.Start))
            if ($lookBehind -match 'utf8\s*\(\s*$' -or $lookBehind -match 'CharPointer_UTF8\s*\(\s*$') { continue }

            $literal = $newWork.Substring($g.Start, $g.End - $g.Start)
            $replacement = "dmb::utf8 (" + $literal + ")"
            $newWork = $newWork.Substring(0, $g.Start) + $replacement + $newWork.Substring($g.End)
            $fileWrapped++
        }

        $result.Add($prefix + $newWork)
    }

    if ($fileWrapped -gt 0) {
        $joined = [string]::Join("`r`n", $result)
        [System.IO.File]::WriteAllText($file.FullName, $joined, (New-Object System.Text.UTF8Encoding($false)))
        Write-Host ("{0,-40} {1} literals wrapped" -f $file.Name, $fileWrapped)
        $totalWrapped += $fileWrapped
    }
}

Write-Host "total: $totalWrapped"
