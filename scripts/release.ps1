# Releases an rcx version: shows the current version, asks for the new one and a commit message, writes the version
# into module/src/module.cpp, builds the module, checks the built DLL reports that version, commits everything in the
# working tree, pushes the branch, tags that commit, pushes the tag, creates the GitHub release with both DLLs, and
# offers to publish the launcher's manifest. README.md, Releases.
#
#   ./scripts/release.ps1            asks, builds, confirms, releases
#   ./scripts/release.ps1 -DryRun    asks, builds and shows what it would do; changes nothing in git or on GitHub
#   ./scripts/release.ps1 -Clean     builds from an empty CMake tree
#
# Needs: git with push rights, and the GitHub CLI signed in once (winget install GitHub.cli, then gh auth login).
# It never installs the module: the build runs without -Install, so garrysmod/lua/bin is not touched.
param(
	[switch]$DryRun,
	[switch]$Clean,
	[string]$LauncherRepo = "C:\Users\bizzc\source\repos\resurgence-launcher"
)

$ErrorActionPreference = "Stop"

$Repo = Split-Path -Parent $PSScriptRoot
$GitHubRepo = "resurgence-dev/resurgence-client-expansion"
$VersionFile = Join-Path $Repo "module\src\module.cpp"
$BuildScript = Join-Path $Repo "module\tools\build_module.ps1"
$BuildDir = Join-Path $Repo "module\builds\win64"
$NotesFile = Join-Path $Repo ".github\release-install.md"
$WorkflowDir = Join-Path $Repo ".github\workflows"
$Assets = @("gmcl_rcx_win64.dll", "openvr_api.dll")
$Utf8 = New-Object System.Text.UTF8Encoding($false)
$Latin1 = [System.Text.Encoding]::GetEncoding(28591)
$ApiHeaders = @{ "User-Agent" = "rcx-release"; "Accept" = "application/vnd.github+json" }

# The first capture group ends right before the version and the third starts right after it.
$VersionPattern = [regex]'(kVersion = "rcx )(\d+\.\d+)( \()'

if ($PSVersionTable.PSVersion.Major -lt 6)
{
	[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
}

function Invoke-Native([string]$Exe, [string[]]$Arguments)
{
	# git and gh write progress to stderr, which Windows PowerShell 5 turns into an error under "Stop".
	$ErrorActionPreference = "Continue"
	$output = & $Exe @Arguments 2>&1
	$code = $LASTEXITCODE
	$ErrorActionPreference = "Stop"

	if ($code -ne 0)
	{
		throw "$Exe $($Arguments -join ' ') failed:`n$($output -join "`n")"
	}

	return $output
}

function Invoke-Git
{
	return Invoke-Native "git" (@("-C", $Repo) + $args)
}

function Invoke-Gh
{
	return Invoke-Native "gh" $args
}

function Get-Sha256([string]$Path)
{
	return (Get-FileHash $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Read-SourceVersion
{
	$match = $VersionPattern.Match([System.IO.File]::ReadAllText($VersionFile))

	if (!$match.Success)
	{
		throw "kVersion = `"rcx X.YY (`" not found in $VersionFile"
	}

	return $match.Groups[2].Value
}

function Set-SourceVersion([string]$Version)
{
	$text = [System.IO.File]::ReadAllText($VersionFile)
	$text = $VersionPattern.Replace($text, { param($m) $m.Groups[1].Value + $Version + $m.Groups[3].Value }, 1)
	[System.IO.File]::WriteAllText($VersionFile, $text, $Utf8)
}

# The same check the launcher's publish-manifest.ps1 makes on a release's DLL.
function Read-DllVersion([string]$Path)
{
	$text = $Latin1.GetString([System.IO.File]::ReadAllBytes($Path))
	$match = [regex]::Match($text, "rcx (\d+\.\d+(?:\.\d+)?) \(")

	if (!$match.Success)
	{
		return ""
	}

	return $match.Groups[1].Value
}

# The workflow files that would start on the tag push and upload their own build over this release's files.
function Get-TagWorkflows
{
	$found = @()

	if (!(Test-Path $WorkflowDir))
	{
		return $found
	}

	foreach ($file in Get-ChildItem $WorkflowDir -File | Where-Object { $_.Extension -in ".yml", ".yaml" })
	{
		$text = [System.IO.File]::ReadAllText($file.FullName)

		if ($text -match '(?m)^\s*push\s*:' -and $text -match '(?m)^\s*tags\s*:')
		{
			$found += $file.Name
		}
	}

	return $found
}

function Get-GhProblem
{
	if (!(Get-Command gh -ErrorAction SilentlyContinue))
	{
		return "the GitHub CLI (gh) isn't installed. Once: winget install GitHub.cli, open a new terminal, gh auth login"
	}

	$ErrorActionPreference = "Continue"
	& gh auth status --hostname github.com *> $null
	$code = $LASTEXITCODE
	$ErrorActionPreference = "Stop"

	if ($code -ne 0)
	{
		return "the GitHub CLI isn't signed in. Once: gh auth login"
	}

	return ""
}

# Problems that stop a real release before anything is asked. A dry run lists them and carries on.
function Assert-Ready([string[]]$Problems)
{
	if ($Problems.Count -eq 0)
	{
		return
	}

	if ($DryRun)
	{
		Write-Host "A real run would stop here:" -ForegroundColor Yellow
		$Problems | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
		Write-Host ""

		return
	}

	throw "Can't release:`n  - $($Problems -join "`n  - ")"
}

foreach ($path in $VersionFile, $BuildScript, $NotesFile)
{
	if (!(Test-Path $path))
	{
		throw "$path is missing."
	}
}

$branch = (Invoke-Git rev-parse --abbrev-ref HEAD | Select-Object -First 1).ToString().Trim()

if ($branch -eq "HEAD")
{
	throw "No branch is checked out (detached HEAD). Releases are made from a branch."
}

Invoke-Git fetch --quiet --tags origin | Out-Null

$ErrorActionPreference = "Continue"
& git -C $Repo rev-parse --verify --quiet "origin/$branch" *> $null
$hasRemoteBranch = $LASTEXITCODE -eq 0
$ErrorActionPreference = "Stop"

if ($hasRemoteBranch)
{
	$behind = [int](Invoke-Git rev-list --count "HEAD..origin/$branch" | Select-Object -First 1).ToString()

	if ($behind -gt 0)
	{
		throw "$branch is $behind commit(s) behind origin/$branch. Pull first."
	}
}

$current = Read-SourceVersion
$tags = @(Invoke-Git tag --list "v*" | ForEach-Object { $_.ToString().Trim() } | Where-Object { $_ -match '^v\d+\.\d+$' })
$newestTag = $tags | Sort-Object { [version]$_.Substring(1) } -Descending | Select-Object -First 1
$newest = if ($newestTag) { [version]$newestTag.Substring(1) } else { [version]"0.0" }

Write-Host ""
Write-Host "Version in module.cpp: $current"
Write-Host "Newest release tag:    $(if ($newestTag) { $newestTag } else { 'none' })"
Write-Host "Branch:                $branch"
Write-Host ""

$problems = @()
$ghProblem = Get-GhProblem

if ($ghProblem)
{
	$problems += $ghProblem
}

$tagWorkflows = @(Get-TagWorkflows)

if ($tagWorkflows.Count -gt 0)
{
	$problems += ".github/workflows/$($tagWorkflows -join ', ') starts on a tag push and would upload its own build over this release's files, " +
		"changing the hashes the launcher's manifest holds. Remove it or change its trigger to workflow_dispatch first"
}

Assert-Ready $problems

$changes = @(Invoke-Git status --short)

if ($changes.Count -gt 0)
{
	Write-Host "Uncommitted changes that will go into the release commit:"
	$changes | ForEach-Object { Write-Host "  $_" }
}
else
{
	Write-Host "No uncommitted changes."
}

Write-Host ""

$default = if ([version]$current -gt $newest) { $current } else { "" }
$prompt = if ($default) { "New version (Enter for $default, the version module.cpp already has)" } else { "New version (for example $($newest.Major).$($newest.Minor + 1))" }
$entered = (Read-Host $prompt).Trim().TrimStart("v")

if ($entered -eq "" -and $default)
{
	$entered = $default
}

if ($entered -notmatch '^\d+\.\d+$')
{
	throw "'$entered' isn't a version like 0.61."
}

$new = [version]$entered
$tag = "v$entered"

if ($new -le $newest)
{
	throw "$entered isn't newer than the newest release, $newestTag."
}

if ($new -lt [version]$current)
{
	throw "$entered is older than the $current module.cpp already has."
}

if (@(Invoke-Git tag --list $tag).Count -gt 0)
{
	throw "Tag $tag already exists."
}

$versionChanges = $entered -ne $current
$message = ""

if ($versionChanges -or $changes.Count -gt 0)
{
	$message = (Read-Host "Commit message").Trim()

	if ($message -eq "")
	{
		throw "The commit message is empty."
	}
}

$title = if ($new.Major -lt 1) { "RCX Alpha $entered" } else { "RCX $entered" }
$originalSource = [System.IO.File]::ReadAllBytes($VersionFile)
$restoreSource = $false
$done = @()

try
{
	if ($versionChanges)
	{
		$restoreSource = $true
		Set-SourceVersion $entered
		Write-Host "Set kVersion to $entered in module/src/module.cpp."
	}

	Write-Host ""
	Write-Host "Building..."
	$buildArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $BuildScript)

	if ($Clean)
	{
		$buildArgs += "-Clean"
	}

	& (Get-Process -Id $PID).Path @buildArgs

	if ($LASTEXITCODE -ne 0)
	{
		throw "The build failed (exit $LASTEXITCODE)."
	}

	$assetInfo = @(foreach ($name in $Assets)
	{
		$path = Join-Path $BuildDir $name

		if (!(Test-Path $path))
		{
			throw "$path was not built."
		}

		[pscustomobject]@{ Name = $name; Path = $path; Size = (Get-Item $path).Length; Sha256 = Get-Sha256 $path }
	})

	$dllVersion = Read-DllVersion $assetInfo[0].Path

	if ($dllVersion -ne $entered)
	{
		throw "The built $($Assets[0]) reports '$dllVersion', not $entered."
	}

	$toCommit = @(Invoke-Git status --short)
	$step = 1

	Write-Host ""
	Write-Host "The built $($Assets[0]) reports rcx $dllVersion."
	Write-Host ""
	Write-Host "This will:"

	if ($toCommit.Count -gt 0)
	{
		Write-Host "  $step. commit these files as: $message"
		$toCommit | ForEach-Object { Write-Host "       $_" }
		$step++
	}
	else
	{
		Write-Host "  (nothing to commit: the release is the current commit)"
	}

	Write-Host "  $step. push $branch to origin"
	$step++
	Write-Host "  $step. create tag $tag (`"rcx $entered`") on that commit and push it"
	$step++
	Write-Host "  $step. create the GitHub release `"$title`" on $GitHubRepo$(if ($new.Major -lt 1) { ', a pre-release,' }) with:"
	$assetInfo | ForEach-Object { Write-Host ("       {0,-20} {1,9:N0} bytes  sha256 {2}" -f $_.Name, $_.Size, $_.Sha256) }
	Write-Host ""

	if ($DryRun)
	{
		Write-Host "Dry run: nothing changed in git or on GitHub."

		if ($versionChanges)
		{
			Write-Host "module.cpp is back at $current; the DLL in module/builds/win64 is the $entered trial build."
		}

		exit 0
	}

	$answer = (Read-Host "Go ahead? (y/n)").Trim().ToLower()

	if ($answer -ne "y")
	{
		Write-Host "Stopped. Nothing changed in git or on GitHub."
		exit 0
	}

	$restoreSource = $false

	if ($toCommit.Count -gt 0)
	{
		$messageFile = Join-Path ([System.IO.Path]::GetTempPath()) "rcx-release-message-$PID.txt"
		[System.IO.File]::WriteAllText($messageFile, $message + "`n", $Utf8)

		try
		{
			Invoke-Git add -A | Out-Null
			Invoke-Git commit -F $messageFile | Out-Null
		}
		finally
		{
			Remove-Item $messageFile -Force -ErrorAction SilentlyContinue
		}

		$done += "committed"
		Write-Host "Committed."
	}

	Invoke-Git push origin $branch | Out-Null
	$done += "pushed $branch"
	Write-Host "Pushed $branch."

	Invoke-Git tag -a $tag -m "rcx $entered" | Out-Null
	$done += "created tag $tag locally"
	Invoke-Git push origin $tag | Out-Null
	$done += "pushed tag $tag"
	Write-Host "Pushed $tag."

	$releaseArgs = @("release", "create", $tag) + @($assetInfo | ForEach-Object { $_.Path }) +
		@("--repo", $GitHubRepo, "--verify-tag", "--title", $title, "--notes-file", $NotesFile, "--generate-notes")

	if ($new.Major -lt 1)
	{
		$releaseArgs += "--prerelease"
	}

	Invoke-Native "gh" $releaseArgs | Out-Null
	$done += "created the release"
	Write-Host "Created the release."

	$release = Invoke-RestMethod "https://api.github.com/repos/$GitHubRepo/releases/tags/$tag" -Headers $ApiHeaders

	foreach ($asset in $assetInfo)
	{
		$remote = $release.assets | Where-Object { $_.name -eq $asset.Name } | Select-Object -First 1

		if (!$remote)
		{
			throw "The release has no asset $($asset.Name)."
		}

		if ($remote.size -ne $asset.Size -or ($remote.digest -and $remote.digest -ne "sha256:$($asset.Sha256)"))
		{
			throw "$($asset.Name) on the release ($($remote.size) bytes, $($remote.digest)) isn't the file that was built."
		}
	}

	Write-Host "Both files on the release match the build: $($release.html_url)"
}
catch
{
	if ($done.Count -gt 0)
	{
		Write-Host ""
		Write-Host "Stopped part way. Done so far: $($done -join ', '). The rest is left to finish by hand." -ForegroundColor Red
	}

	throw
}
finally
{
	if ($restoreSource)
	{
		[System.IO.File]::WriteAllBytes($VersionFile, $originalSource)
	}
}

Write-Host ""
Write-Host "Players get rcx $entered only once the launcher's manifest is published."
$manifestScript = Join-Path $LauncherRepo "scripts\publish-manifest.ps1"

if (!(Test-Path $manifestScript))
{
	Write-Host "$manifestScript not found. Run the launcher's scripts/publish-manifest.ps1 -RcxTag $tag yourself."
	exit 0
}

$answer = (Read-Host "Publish the manifest now? (y/n)").Trim().ToLower()

if ($answer -ne "y")
{
	Write-Host "Not published. Later: $manifestScript -RcxTag $tag"
	exit 0
}

# publish-manifest.ps1 needs PowerShell 7.
$pwsh = Get-Command pwsh -ErrorAction SilentlyContinue
$shell = if ($pwsh) { $pwsh.Source } else { (Get-Process -Id $PID).Path }
& $shell -NoProfile -ExecutionPolicy Bypass -File $manifestScript -RcxTag $tag

if ($LASTEXITCODE -ne 0)
{
	throw "Publishing the manifest failed (exit $LASTEXITCODE). The release itself is done; run $manifestScript -RcxTag $tag again."
}
