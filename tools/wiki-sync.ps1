<#
Publishes the generated pages (tools/wiki_build.py) into this project's wiki.
PowerShell twin of wiki-sync.sh; both do the same steps in the same order.

    tools\wiki-sync.ps1            build, commit locally, show what would change
    tools\wiki-sync.ps1 -Push      the same, and push it

GitLab keeps a project wiki in a git repository of its own, right next to the
project: https://<host>/<namespace>/<project>.wiki.git. Publishing is therefore
an ordinary clone-commit-push, which is why this needs no API calls.

Credentials. CI_JOB_TOKEN may read repositories but not write to them, so it
cannot be used here - a push with it fails with 403. Provide instead:

    WIKI_TOKEN        a Project Access Token (role Developer, scope
                      write_repository), stored as a masked CI variable
    WIKI_TOKEN_USER   only for a deploy token: its username (default: oauth2)
    WIKI_URL          overrides the whole URL, for anything unusual
    WIKI_BRANCH       branch to publish on when the wiki is still empty

Run from a normal Windows shell the token is usually not needed at all: without
WIKI_TOKEN the URL is derived from the origin remote and git uses whatever
credential helper you already push the code with.

The token never reaches a log line or .git/config: it lives in the remote URL
only while the push runs, and every message prints a scrubbed URL.

The wiki must be enabled for the project (Settings -> General -> Visibility).
A wiki that has never had a page has no repository yet and the clone fails;
this starts an empty one in that case, so the first run also works.

Pages that the build no longer produces are deleted, because the repository is
the single source: a page renamed at the source would otherwise stay published
forever under its old name. Nothing is lost - the wiki is a git repository, and
its history keeps every removed page. -NoPrune leaves them alone.
#>

[CmdletBinding()]
param(
    # Push the commit. Without it the run stops after committing locally, which
    # is a full dry run: the diff is printed and nothing leaves the machine.
    [switch] $Push,

    # Keep wiki pages that the build no longer produces.
    [switch] $NoPrune,

    # Where to build the pages.
    [string] $Pages = '',

    [switch] $Help
)

$ErrorActionPreference = 'Continue'

$Repo = Split-Path -Parent $PSScriptRoot
if (-not $Pages) { $Pages = Join-Path $Repo 'build_wiki\pages' }
$Clone = Join-Path $Repo 'build_wiki\repo'

# Printed by -Help. Get-Help would only show the syntax line: the header above
# is prose, deliberately the same text as wiki-sync.sh, not the .SYNOPSIS /
# .PARAMETER form Get-Help can parse.
if ($Help) {
    Write-Host @"
Publishes the generated wiki pages into the project's wiki repository.

Usage: tools\wiki-sync.ps1 [-Push] [-NoPrune] [-Pages DIR]

  -Push         push the commit (default: build and commit locally only)
  -NoPrune      keep wiki pages that the build no longer produces
  -Pages DIR    where to build the pages (default: build_wiki\pages)
  -Help         this text

Environment: WIKI_TOKEN, WIKI_TOKEN_USER, WIKI_URL, WIKI_BRANCH - see the
comment at the top of this file.
"@
    exit 0
}

# --- helpers ------------------------------------------------------------------

function Write-Note([string] $Text, [string] $Color = 'Cyan') {
    Write-Host "wiki-sync: " -ForegroundColor $Color -NoNewline
    Write-Host $Text
}

function Stop-WithError([string] $Text) {
    Write-Host "wiki-sync: $Text" -ForegroundColor Red
    exit 1
}

function Get-EnvOr([string] $Name, [string] $Fallback) {
    $value = [Environment]::GetEnvironmentVariable($Name)
    if ([string]::IsNullOrWhiteSpace($value)) { return $Fallback }
    return $value
}

# Everything below prints this, never the URL itself.
function Hide-Credentials([string] $Url) {
    return ($Url -replace '://[^@/]*@', '://')
}

# --- 1. build the pages -------------------------------------------------------

# Ask each candidate to run something, rather than trusting that it exists:
# Windows ships a python3 on PATH that is only an App Store shortcut and fails
# the moment it is used.
$py = ''
foreach ($candidate in @('python3', 'python')) {
    if (Get-Command $candidate -ErrorAction SilentlyContinue) {
        & $candidate -c 'import sys' *> $null
        if ($LASTEXITCODE -eq 0) { $py = $candidate; break }
    }
}
if (-not $py) { Stop-WithError 'no working python3 on PATH - needed to build the pages' }

if (Test-Path $Pages) { Remove-Item -Recurse -Force $Pages }
& $py (Join-Path $PSScriptRoot 'wiki_build.py') -o $Pages
if ($LASTEXITCODE -ne 0) { Stop-WithError 'building the pages failed' }

# --- 2. work out where the wiki lives -----------------------------------------

$wikiUrl = ''
$wikiBranch = Get-EnvOr 'WIKI_BRANCH' 'main'

if (-not [string]::IsNullOrWhiteSpace($env:WIKI_URL)) {
    $wikiUrl = $env:WIKI_URL
} elseif ($env:WIKI_TOKEN -and $env:CI_SERVER_HOST -and $env:CI_PROJECT_PATH) {
    # Same construction as .fetch-sources in .gitlab-ci.yml: host and path from
    # the predefined variables, so nothing is hard-wired to one instance.
    $tokenUser = Get-EnvOr 'WIKI_TOKEN_USER' 'oauth2'
    $wikiUrl = "https://${tokenUser}:$($env:WIKI_TOKEN)@$($env:CI_SERVER_HOST)/$($env:CI_PROJECT_PATH).wiki.git"
} else {
    $origin = & git -C $Repo remote get-url origin
    if ($LASTEXITCODE -ne 0 -or -not $origin) {
        Stop-WithError 'no WIKI_URL, no WIKI_TOKEN and no origin remote - nowhere to publish to'
    }
    $wikiUrl = ($origin -replace '\.git$', '') + '.wiki.git'
    if ($Push -and -not $env:WIKI_TOKEN) {
        Write-Note "pushing to $(Hide-Credentials $wikiUrl) with your own git credentials" 'Yellow'
    }
}
$cleanUrl = Hide-Credentials $wikiUrl

# --- 3. get the wiki repository -----------------------------------------------

# Never sit at a username prompt: in CI there is nobody to answer it, and a job
# that hangs for an hour is worse than one that fails in a second. An existing
# value is respected, for a local run that means to use a credential helper.
if (-not $env:GIT_TERMINAL_PROMPT) { $env:GIT_TERMINAL_PROMPT = '0' }

if (Test-Path $Clone) { Remove-Item -Recurse -Force $Clone }
New-Item -ItemType Directory -Force (Split-Path -Parent $Clone) | Out-Null

# git reports a failed clone on stderr, which is left visible on purpose: if it
# is a credentials problem, its message says so far better than a guess here.
#
# core.autocrlf=false belongs on the clone command, not on the clone afterwards:
# the checkout happens during the clone, so setting it later would leave a
# working tree full of CRLF (autocrlf=true is the usual Windows setting). Every
# page the build does not overwrite would then be committed again with changed
# line endings, and the next run from CI would flip it straight back.
& git clone --quiet -c core.autocrlf=false -c core.eol=lf $wikiUrl $Clone
if ($LASTEXITCODE -eq 0) {
    $branch = & git -C $Clone symbolic-ref --short HEAD
    if ($LASTEXITCODE -ne 0 -or -not $branch) { $branch = $wikiBranch }
    Write-Note "cloned $cleanUrl (branch $branch)"
} else {
    # Either the wiki has never been created, or the credentials are wrong. The
    # first case is normal on a first run and recoverable; the second shows up
    # as a failed push below.
    $branch = $wikiBranch
    Write-Note "$cleanUrl could not be cloned - starting an empty wiki on $branch" 'Yellow'
    & git init --quiet $Clone
    if ($LASTEXITCODE -ne 0) { Stop-WithError 'git init failed' }
    # Nothing was checked out here, so setting it now is in time.
    & git -C $Clone config core.autocrlf false
    & git -C $Clone config core.eol lf
    & git -C $Clone symbolic-ref HEAD "refs/heads/$branch"
    & git -C $Clone remote add origin $wikiUrl
}

# --- 4. sync the tree ---------------------------------------------------------

if (-not $NoPrune) {
    foreach ($existing in (Get-ChildItem -Path $Clone -Filter '*.md' -File)) {
        if (-not (Test-Path (Join-Path $Pages $existing.Name))) {
            Write-Note "removing $($existing.Name) - no longer generated"
            Remove-Item -Force $existing.FullName
        }
    }
}

$built = @(Get-ChildItem -Path $Pages -Filter '*.md' -File)
if ($built.Count -eq 0) { Stop-WithError 'no pages to publish' }
Copy-Item -Path (Join-Path $Pages '*.md') -Destination $Clone -Force

# --- 5. commit and push -------------------------------------------------------

$sourceSha = $env:CI_COMMIT_SHA
if (-not $sourceSha) {
    $sourceSha = & git -C $Repo rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or -not $sourceSha) { $sourceSha = 'unknown' }
}
$shortSha = $sourceSha.Substring(0, [Math]::Min(12, $sourceSha.Length))

& git -C $Clone add -A
if ($LASTEXITCODE -ne 0) { Stop-WithError 'git add failed' }

& git -C $Clone diff --cached --quiet
if ($LASTEXITCODE -eq 0) {
    Write-Note 'already up to date - nothing to publish' 'Green'
    exit 0
}

& git -C $Clone --no-pager diff --cached --stat

$message = "Regenerate wiki from $shortSha"
if ($env:CI_PIPELINE_URL) {
    $project = Get-EnvOr 'CI_PROJECT_PATH' 'this repository'
    $message = @"
Regenerate wiki from $shortSha

Generated by $($env:CI_PIPELINE_URL) from $project@$sourceSha.
Do not edit here: change the source file and let the pipeline republish.
"@
}

$authorName = Get-EnvOr 'GITLAB_USER_NAME' 'esp_crsf wiki bot'
$authorMail = Get-EnvOr 'GITLAB_USER_EMAIL' "ci@$(Get-EnvOr 'CI_SERVER_HOST' 'localhost')"

& git -C $Clone -c "user.name=$authorName" -c "user.email=$authorMail" commit --quiet -m $message
if ($LASTEXITCODE -ne 0) { Stop-WithError 'commit failed' }

if (-not $Push) {
    Write-Note "committed in $Clone but not pushed (add -Push)"
    exit 0
}

& git -C $Clone push --quiet origin "HEAD:refs/heads/$branch"
$pushed = ($LASTEXITCODE -eq 0)

# The token has done its job; leave no copy behind in .git/config.
& git -C $Clone remote set-url origin $cleanUrl

if (-not $pushed) {
    Stop-WithError "push to $cleanUrl failed - is WIKI_TOKEN valid and scoped write_repository?"
}
Write-Note "published $shortSha to $cleanUrl ($branch)" 'Green'
