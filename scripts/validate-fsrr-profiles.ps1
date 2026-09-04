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

$knownRecompositionModes = @(
    'denoised',
    'depth_delta_current_color'
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
    if ($null -ne $profile.PSObject.Properties['specular_holdout_max_roughness'] -and
        ($profile.specular_holdout_max_roughness -lt 0 -or $profile.specular_holdout_max_roughness -gt 1)) {
        throw "Profile '$($profile.id)' must use a specular holdout roughness in [0, 1]."
    }
    if ($profile.recomposition_mode -notin $knownRecompositionModes) {
        throw "Profile '$($profile.id)' names unknown recomposition mode '$($profile.recomposition_mode)'."
    }
    if ($profile.recomposition_mode -eq 'depth_delta_current_color') {
        if ($profile.depth_delta_current_color_scale -le 0) {
            throw "Profile '$($profile.id)' must use a positive depth-delta current-color scale."
        }
        if ($profile.depth_delta_current_color_strength -le 0 -or
            $profile.depth_delta_current_color_strength -gt 1) {
            throw "Profile '$($profile.id)' must use a depth-delta current-color strength in (0, 1]."
        }
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
