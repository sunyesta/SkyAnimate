# Runs SkyAnimate from a copy of the build, so a rebuild can replace the build's files while
# SkyAnimate is open. Each build gets its own folder under build/run (named after the build's
# time), so an open copy never blocks the next one; old copies are removed once nothing runs
# from them. The desktop and Start menu shortcuts run this script. Arguments are passed on
# (a .sky file to open).

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build\bin\RelWithDebInfo'
$exe = Join-Path $build 'SkyAnimate.exe'
if (-not (Test-Path $exe)) {
    Add-Type -AssemblyName PresentationFramework
    [System.Windows.MessageBox]::Show("SkyAnimate isn't built yet:`n$exe", 'SkyAnimate') | Out-Null
    exit 1
}

$stamp = (Get-Item $exe).LastWriteTimeUtc.ToString('yyyyMMdd-HHmmss')
$runs = Join-Path $root 'build\run'
$target = Join-Path $runs $stamp
$target_exe = Join-Path $target 'SkyAnimate.exe'
if (-not (Test-Path $target_exe)) {
    New-Item -ItemType Directory -Force $target | Out-Null
    # The executable goes last, so a copy that was cut short is never mistaken for a whole one.
    Copy-Item -Path (Join-Path $build '*') -Destination $target -Include *.dll, *.pdb
    Copy-Item -Path $exe -Destination $target
}

# Copies of older builds that are no longer running; one still open stays until next time.
Get-ChildItem $runs -Directory | Where-Object Name -ne $stamp | ForEach-Object {
    Remove-Item $_.FullName -Recurse -Force -ErrorAction SilentlyContinue
}

# Normal window style: the shortcut starts this script minimised to hide its console, and
# without this SkyAnimate would inherit that and open minimised too.
if ($args.Count -gt 0) {
    Start-Process -FilePath $target_exe -WorkingDirectory $target -WindowStyle Normal -ArgumentList $args
}
else {
    Start-Process -FilePath $target_exe -WorkingDirectory $target -WindowStyle Normal
}
