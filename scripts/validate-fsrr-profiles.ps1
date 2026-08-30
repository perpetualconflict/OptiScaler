[CmdletBinding()]
param(
    [string]$Path
)

$ErrorActionPreference = 'Stop'
if (-not $Path) {
    $repoRoot = Split-Path -Parent $PSScriptRoot
    $Path = Join-Path $repoRoot 'OptiScaler\profiles\fsrr.json'
}

$document = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
if ($document.schema_version -ne 1) {
    throw 'profiles/fsrr.json must use schema_version 1.'
}

$knownSignals = @(
    'ambient_occlusion',
    'direct_diffuse',
    'direct_specular',
    'dominant_light_visibility',
    'indirect_diffuse',
    'indirect_specular',
    'specular_occlusion'
)

$ids = @{}
foreach ($profile in $document.profiles) {
    if (-not $profile.id -or $ids.ContainsKey($profile.id)) {
        throw "Every profile must have a unique non-empty id; duplicate or missing id '$($profile.id)'."
    }
    $ids[$profile.id] = $true

    if (-not $profile.executables -or $profile.executables.Count -eq 0) {
        throw "Profile '$($profile.id)' must name at least one executable."
    }
    if ($profile.enabled -and -not $profile.validated) {
        throw "Profile '$($profile.id)' cannot be enabled before capture validation."
    }
    if ($profile.linear_depth_min -lt 0 -or $profile.linear_depth_min -ge $profile.linear_depth_max) {
        throw "Profile '$($profile.id)' has invalid linear depth bounds."
    }

    foreach ($signal in @($profile.signals) + @($profile.checkerboard_signals)) {
        if ($signal -notin $knownSignals) {
            throw "Profile '$($profile.id)' names unknown signal '$signal'."
        }
    }
    foreach ($signal in $profile.checkerboard_signals) {
        if ($signal -notin $profile.signals) {
            throw "Profile '$($profile.id)' checkerboards inactive signal '$signal'."
        }
    }
}

Write-Host "Validated $($document.profiles.Count) FSR-RR profile(s) in $Path"
