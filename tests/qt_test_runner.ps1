param(
    [Parameter(Mandatory = $true)][string]$Executable,
    [Parameter(Mandatory = $true)][string]$QtBin
)

$env:Path = "$QtBin;$env:Path"
& $Executable
exit $LASTEXITCODE
