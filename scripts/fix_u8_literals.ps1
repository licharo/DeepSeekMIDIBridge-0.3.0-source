<#
    Wraps u8"..." literals containing non-ASCII text in juce::String::fromUTF8(...),
    because in C++17 u8"" is still a plain const char* and juce::String then treats
    it as ASCII. (u8R"(...)" literals must already be wrapped by hand.)
#>
param(
    [string] $Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = "Stop"

$files = Get-ChildItem (Join-Path $Root "source") -Recurse -Include *.cpp, *.h
$pattern = [regex]'(?<![\w])u8("(?:[^"\\]|\\.)*")'

foreach ($file in $files) {
    $text = [System.IO.File]::ReadAllText($file.FullName, [System.Text.Encoding]::UTF8)
    $count = 0

    $new = $pattern.Replace($text, {
        param($m)
        $index = $m.Index
        $before = $text.Substring([Math]::Max(0, $index - 12), [Math]::Min(12, $index))
        if ($before -match 'fromUTF8\s*\(\s*$') { return $m.Value }
        $script:count++
        "juce::String::fromUTF8 (u8" + $m.Groups[1].Value + ")"
    })

    if ($new -ne $text) {
        [System.IO.File]::WriteAllText($file.FullName, $new, (New-Object System.Text.UTF8Encoding($false)))
        Write-Host ("{0,-40} wrapped" -f $file.Name)
    }
}
