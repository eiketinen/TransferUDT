[CmdletBinding()]
param(
    [string]$Owner = "eiketinen",
    [string]$Repository = "TransferUDT",
    [string]$RulesetPath,
    [switch]$ValidateOnly
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($RulesetPath)) {
    $RulesetPath = Join-Path $PSScriptRoot "..\.github\rulesets\main-protection.json"
}

$resolvedRulesetPath = (Resolve-Path -LiteralPath $RulesetPath).Path
$ruleset = Get-Content -LiteralPath $resolvedRulesetPath -Raw | ConvertFrom-Json
$ruleTypes = @($ruleset.rules.type)
$requiredRuleTypes = @("deletion", "non_fast_forward", "pull_request", "required_status_checks")

if ($ruleset.target -ne "branch" -or $ruleset.enforcement -ne "active") {
    throw "The main ruleset must target branches with active enforcement."
}

if (@($ruleset.conditions.ref_name.include) -notcontains "refs/heads/main") {
    throw "The ruleset must include refs/heads/main."
}

foreach ($requiredRuleType in $requiredRuleTypes) {
    if ($ruleTypes -notcontains $requiredRuleType) {
        throw "The ruleset is missing required rule type '$requiredRuleType'."
    }
}

$statusRule = @($ruleset.rules | Where-Object type -eq "required_status_checks")
$requiredChecks = @($statusRule.parameters.required_status_checks.context)
if ($statusRule.Count -ne 1 -or $requiredChecks.Count -eq 0) {
    throw "The ruleset must define one non-empty required_status_checks rule."
}

Write-Host "Validated ruleset '$($ruleset.name)' with $($requiredChecks.Count) required checks."
if ($ValidateOnly) {
    return
}

$token = $env:GITHUB_TOKEN
if ([string]::IsNullOrWhiteSpace($token)) {
    throw "Set GITHUB_TOKEN to a token with repository Administration write permission."
}

$headers = @{
    Accept = "application/vnd.github+json"
    Authorization = "Bearer $token"
    "X-GitHub-Api-Version" = "2026-03-10"
    "User-Agent" = "TransferUDT-ruleset-manager"
}
$baseUri = "https://api.github.com/repos/$Owner/$Repository/rulesets"
$existingRulesets = @(Invoke-RestMethod -Headers $headers -Uri $baseUri -Method Get)
$matchingRulesets = @($existingRulesets | Where-Object {
    $_ -and $_.PSObject.Properties.Name -contains "name" -and $_.name -eq $ruleset.name
})

if ($matchingRulesets.Count -gt 1) {
    throw "More than one ruleset named '$($ruleset.name)' exists. Resolve duplicates before applying."
}

$body = $ruleset | ConvertTo-Json -Depth 20 -Compress
if ($matchingRulesets.Count -eq 1) {
    $uri = "$baseUri/$($matchingRulesets[0].id)"
    $result = Invoke-RestMethod -Headers $headers -Uri $uri -Method Put -ContentType "application/json" -Body $body
    $operation = "Updated"
} else {
    $result = Invoke-RestMethod -Headers $headers -Uri $baseUri -Method Post -ContentType "application/json" -Body $body
    $operation = "Created"
}

if ($result.enforcement -ne "active" -or $result.target -ne "branch") {
    throw "GitHub returned an unexpected ruleset state after the update."
}

Write-Host "$operation ruleset '$($result.name)' (id $($result.id), enforcement $($result.enforcement))."
Write-Host $result._links.html.href
