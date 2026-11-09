#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('Inspect', 'Configure', 'Protect', 'Verify')]
    [string]$Mode = 'Inspect'
)

# Repository administration only; never call from CI. No token arguments/files/logs.
$ErrorActionPreference = 'Stop'
$repository = 'kkiwkK1/desk'
$apiRoot = 'https://api.github.com'
$checkName = 'Windows regression'
$headers = $null
$credential = @{}
$raw = $null
$oldPrompt = $env:GIT_TERMINAL_PROMPT
$oldInteractive = $env:GCM_INTERACTIVE

function Invoke-GitHub([string]$Path, [string]$Method = 'GET', $Body = $null) {
    $parameters = @{
        Uri = "$apiRoot/$Path"
        Method = $Method
        Headers = $headers
        TimeoutSec = 30
        ErrorAction = 'Stop'
    }
    if ($null -ne $Body) {
        $parameters.ContentType = 'application/json; charset=utf-8'
        $parameters.Body = [Text.Encoding]::UTF8.GetBytes(($Body | ConvertTo-Json -Depth 15 -Compress))
    }
    try { Invoke-RestMethod @parameters }
    catch {
        $status = 'unknown'
        if ($_.Exception.Response) { $status = [int]$_.Exception.Response.StatusCode }
        $detail = ''
        try {
            $errorJson = $_.ErrorDetails.Message
            if (-not $errorJson -and $_.Exception.Response) {
                $reader = New-Object IO.StreamReader($_.Exception.Response.GetResponseStream())
                try { $errorJson = $reader.ReadToEnd() } finally { $reader.Dispose() }
            }
            $diagnostic = $errorJson | ConvertFrom-Json
            $detail = [string]$diagnostic.message
            foreach ($item in @($diagnostic.errors)) {
                $detail += ' ' + (($item | Select-Object resource, field, code, message | ConvertTo-Json -Compress))
            }
            if ($credential['password']) { $detail = $detail.Replace($credential['password'], '[redacted]') }
            if ($detail.Length -gt 2000) { $detail = $detail.Substring(0, 2000) }
        } catch { $detail = '' }
        throw "GitHub API $Method $Path failed (HTTP $status): $detail (authentication details suppressed)."
    }
}

try {
    $env:GIT_TERMINAL_PROMPT = '0'
    $env:GCM_INTERACTIVE = 'Never'
    $raw = @("protocol=https`nhost=github.com`nusername=kkiwkK1`n`n" |
        & git -c credential.interactive=never credential fill 2>$null)
    if ($LASTEXITCODE -ne 0) { throw 'Git credential helper has no noninteractive GitHub login. Authenticate in your browser; do not paste tokens into chat.' }
    foreach ($line in $raw) {
        $separator = $line.IndexOf('=')
        if ($separator -gt 0) { $credential[$line.Substring(0, $separator)] = $line.Substring($separator + 1) }
    }
    if (-not $credential['password']) { throw 'GitHub credential helper returned no authentication.' }
    $headers = @{
        Authorization = 'Bearer ' + $credential['password']
        Accept = 'application/vnd.github+json'
        'X-GitHub-Api-Version' = '2022-11-28'
        'User-Agent' = 'Desktop-Desk-Repository-Setup'
    }
    $repo = Invoke-GitHub "repos/$repository"
    if ($repo.full_name -cne $repository -or -not $repo.permissions.admin) {
        throw 'Expected repository/admin permission not confirmed; refusing changes.'
    }

    if ($Mode -eq 'Configure') {
        $null = Invoke-GitHub "repos/$repository" 'PATCH' @{
            default_branch = 'main'
            description = 'Desktop Desk: Windows desktop board, C++20 / Win32, assistant and memory cards.'
            allow_merge_commit = $true
            allow_squash_merge = $false
            allow_rebase_merge = $false
            allow_auto_merge = $false
            allow_update_branch = $true
            delete_branch_on_merge = $false
            has_issues = $true
        }
        $null = Invoke-GitHub "repos/$repository/topics" 'PUT' @{
            names = @('windows', 'win32', 'cpp20', 'desktop-widgets', 'ai-assistant')
        }
        # Public repositories support these protections; failures remain visible.
        $null = Invoke-GitHub "repos/$repository" 'PATCH' @{
            security_and_analysis = @{
                secret_scanning = @{ status = 'enabled' }
                secret_scanning_push_protection = @{ status = 'enabled' }
            }
        }
        Write-Output 'REPOSITORY_SETTINGS=applied (visibility and LICENSE unchanged)'
    }

    if ($Mode -eq 'Protect') {
        # Bind the required check to its actual GitHub Actions app, not a spoofable context alone.
        $main = Invoke-GitHub "repos/$repository/branches/main"
        $runs = Invoke-GitHub "repos/$repository/commits/$($main.commit.sha)/check-runs"
        $green = @($runs.check_runs | Where-Object {
            $_.name -ceq $checkName -and $_.app.slug -ceq 'github-actions' -and $_.conclusion -ceq 'success'
        } | Sort-Object completed_at -Descending)
        if ($green.Count -eq 0) { throw 'Current main must first pass Windows regression in GitHub Actions; no protection changes made.' }
        $null = Invoke-GitHub "repos/$repository/branches/dev"
        $null = Invoke-GitHub "repos/$repository/branches/main/protection" 'PUT' @{
            required_status_checks = @{
                strict = $true
                # GitHub's schema treats legacy contexts and app-bound checks as alternatives.
                checks = @(@{ context = $checkName; app_id = [int]$green[0].app.id })
            }
            enforce_admins = $true
            required_pull_request_reviews = @{
                dismiss_stale_reviews = $true
                require_code_owner_reviews = $false
                required_approving_review_count = 0
                require_last_push_approval = $false
            }
            restrictions = $null
            required_linear_history = $false
            required_conversation_resolution = $true
            allow_force_pushes = $false
            allow_deletions = $false
        }
        # dev is long-lived and permits ordinary development pushes, not mandatory feature PRs.
        $null = Invoke-GitHub "repos/$repository/branches/dev/protection" 'PUT' @{
            required_status_checks = $null
            enforce_admins = $true
            required_pull_request_reviews = $null
            restrictions = $null
            required_linear_history = $false
            required_conversation_resolution = $false
            allow_force_pushes = $false
            allow_deletions = $false
        }
        Write-Output 'BRANCH_PROTECTION=applied (main PR+Actions; dev normal pushes; neither force/delete)'
    }

    $repo = Invoke-GitHub "repos/$repository"
    [pscustomobject]@{
        Repository = $repo.full_name
        Visibility = $repo.visibility
        DefaultBranch = $repo.default_branch
        MergeCommit = $repo.allow_merge_commit
        Squash = $repo.allow_squash_merge
        Rebase = $repo.allow_rebase_merge
        AutoMerge = $repo.allow_auto_merge
        DeleteBranchOnMerge = $repo.delete_branch_on_merge
        SecretScanning = $repo.security_and_analysis.secret_scanning.status
        PushProtection = $repo.security_and_analysis.secret_scanning_push_protection.status
    } | Format-List
    if ($Mode -eq 'Verify' -or $Mode -eq 'Protect') {
        foreach ($branch in @('main', 'dev')) {
            $protection = Invoke-GitHub "repos/$repository/branches/$branch/protection"
            [pscustomobject]@{
                Branch = $branch
                EnforceAdmins = $protection.enforce_admins.enabled
                RequiredPullRequest = ($null -ne $protection.required_pull_request_reviews)
                RequiredApprovals = $protection.required_pull_request_reviews.required_approving_review_count
                StrictChecks = $protection.required_status_checks.strict
                RequiredChecks = @($protection.required_status_checks.checks | Where-Object { $null -ne $_ } | ForEach-Object { "$($_.context) [app:$($_.app_id)]" }) -join ', '
                ResolveConversations = $protection.required_conversation_resolution.enabled
                ForcePush = $protection.allow_force_pushes.enabled
                Delete = $protection.allow_deletions.enabled
            } | Format-List
        }
    }
}
finally {
    if ($headers) { $headers.Clear() }
    $credential.Clear()
    $raw = $null
    $env:GIT_TERMINAL_PROMPT = $oldPrompt
    $env:GCM_INTERACTIVE = $oldInteractive
}
