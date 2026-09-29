<#
    Third pass of the UTF-8 literal fix: collapses expressions that consist of
    several adjacent string literals some of which are already wrapped, e.g.

        p << dmb::utf8 ("第一段"
             "第二段")
             dmb::utf8 ("第三段");

    into

        p << dmb::utf8 ("第一段"
             "第二段"
             "第三段");
#>
param(
    [string] $Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = "Stop"

$files = Get-ChildItem (Join-Path $Root "source") -Recurse -Include *.cpp, *.h

$litSeq = '(?:"(?:[^"\\]|\\.)*"\s*)+'
$pattern = [regex]("dmb::utf8 \(\s*(" + $litSeq + ")\)\s*dmb::utf8 \(\s*(" + $litSeq + ")\)")

foreach ($file in $files) {
    $text = [System.IO.File]::ReadAllText($file.FullName, [System.Text.Encoding]::UTF8)
    $count = 0

    while ($true) {
        $new = $pattern.Replace($text, {
            param($m)
            $inner = ($m.Groups[1].Value.TrimEnd() + "`r`n" + $m.Groups[2].Value.TrimEnd())
            "dmb::utf8 (" + $inner + ")"
        }, 1)

        if ($new -eq $text) { break }
        $text = $new
        $count++
    }

    # leave a space between the trailing quote and the closing paren
    $text = $text -replace '"\)(\s*[;,)])', '")$1'

    if ($count -gt 0) {
        [System.IO.File]::WriteAllText($file.FullName, $text, (New-Object System.Text.UTF8Encoding($false)))
        Write-Host ("{0,-40} {1} merges" -f $file.Name, $count)
    }
}
