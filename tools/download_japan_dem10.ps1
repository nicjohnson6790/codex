[CmdletBinding()]
param(
    [string]$Destination,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = Join-Path $PSScriptRoot '..\assets\source\japan-dem10'
}
$Destination = [IO.Path]::GetFullPath($Destination)
$manifestUrl = 'https://data.source.coop/smartmaps/japan-geotiff-dem/10/latest_file_list.csv.gz'
$manifestGz = Join-Path $Destination 'latest_file_list.csv.gz'
$manifestCsv = Join-Path $Destination 'latest_file_list.csv'
New-Item -ItemType Directory -Force -Path $Destination | Out-Null

Invoke-WebRequest -Uri $manifestUrl -OutFile "$manifestGz.part"
Move-Item -Force -LiteralPath "$manifestGz.part" -Destination $manifestGz
$inputStream = [IO.File]::OpenRead($manifestGz)
try {
    $gzip = [IO.Compression.GzipStream]::new($inputStream, [IO.Compression.CompressionMode]::Decompress)
    try {
        $outputStream = [IO.File]::Create("$manifestCsv.part")
        try { $gzip.CopyTo($outputStream) } finally { $outputStream.Dispose() }
    } finally { $gzip.Dispose() }
} finally { $inputStream.Dispose() }
Move-Item -Force -LiteralPath "$manifestCsv.part" -Destination $manifestCsv
(Get-FileHash -Algorithm SHA256 -LiteralPath $manifestCsv).Hash.ToLowerInvariant() | Set-Content -Encoding ascii -NoNewline -LiteralPath (Join-Path $Destination 'latest_file_list.csv.sha256')

$rows = @(Import-Csv -LiteralPath $manifestCsv)
$number = 0
foreach ($row in $rows) {
    $number++
    $uri = [Uri]$row.url
    $name = [IO.Path]::GetFileName($uri.AbsolutePath)
    $target = Join-Path $Destination $name
    $valid = !$Force -and (Test-Path -LiteralPath $target) -and ((Get-Item -LiteralPath $target).Length -eq [Int64]$row.size)
    if ($valid) { $valid = (Get-FileHash -Algorithm MD5 -LiteralPath $target).Hash.ToLowerInvariant() -eq $row.md5.ToLowerInvariant() }
    if (!$valid) {
        Write-Host "[$number/$($rows.Count)] Downloading $name"
        Invoke-WebRequest -Uri $row.url -OutFile "$target.part"
        if ((Get-Item -LiteralPath "$target.part").Length -ne [Int64]$row.size) { throw "Size mismatch: $name" }
        if ((Get-FileHash -Algorithm MD5 -LiteralPath "$target.part").Hash.ToLowerInvariant() -ne $row.md5.ToLowerInvariant()) { throw "MD5 mismatch: $name" }
        Move-Item -Force -LiteralPath "$target.part" -Destination $target
    }
}
Write-Host "Verified $($rows.Count) current DEM10 files in $Destination"
