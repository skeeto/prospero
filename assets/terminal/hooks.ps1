# Prospero's terminal integration for PowerShell, which a terminal tab
# started with -NoProfile runs first (psmarkboot): it loads the profiles
# as PowerShell would have, then wraps prompt and PSConsoleHostReadLine
# to mark each prompt and each line run.
#
# This is free and unencumbered software released into the public domain.

$global:__prospero_nonce = $env:PROSPERO_TERM_NONCE
Remove-Item Env:PROSPERO_TERM_NONCE, Env:PROSPERO_TERM_HOOKS -ErrorAction SilentlyContinue

foreach ($__prospero_p in @($PROFILE.AllUsersAllHosts, $PROFILE.AllUsersCurrentHost,
                            $PROFILE.CurrentUserAllHosts, $PROFILE.CurrentUserCurrentHost)) {
    if ($__prospero_p -and (Test-Path -LiteralPath $__prospero_p)) {
        try {
            . $__prospero_p
        } catch {
            [Console]::Error.WriteLine(($_ | Out-String).Trim())
        }
    }
}
Remove-Variable __prospero_p -ErrorAction SilentlyContinue

if ($global:__prospero_nonce) {
    $global:__prospero_ran  = $false  # a line was read since the last prompt
    $global:__prospero_seen = $false  # a prompt was shown
    $global:__prospero_code = $null   # LASTEXITCODE when it was read
    $global:__prospero_rl   = $null   # our PSConsoleHostReadLine
    $global:__prospero_prev = $function:prompt

    function global:__prospero_mark([string]$m) {
        [Console]::Write([char]27 + ']133;' + $m + ';nonce=' + $global:__prospero_nonce + [char]7)
    }

    # PSReadLine's, once it is loaded, which may be after this runs
    function global:__prospero_wrap {
        $f = Get-Item Function:\PSConsoleHostReadLine -ErrorAction SilentlyContinue
        if (!$f -or [object]::ReferenceEquals($f.ScriptBlock, $global:__prospero_rl)) {
            return
        }
        $global:__prospero_readline = $f.ScriptBlock
        function global:PSConsoleHostReadLine {
            $line = & $global:__prospero_readline
            if ($line -and $line.Trim()) {
                $global:__prospero_ran  = $true
                $global:__prospero_code = $global:LASTEXITCODE
                __prospero_mark 'C'
            }
            $line
        }
        $global:__prospero_rl = $function:PSConsoleHostReadLine
    }

    function global:prompt {
        $ok   = $global:?
        $code = $global:LASTEXITCODE
        if ($global:__prospero_ran) {
            $s = if ($ok) { 0 } elseif ($code -and $code -ne $global:__prospero_code) { $code } else { 1 }
            __prospero_mark "D;$s"
        } elseif ($global:__prospero_seen) {
            __prospero_mark 'D'
        }
        $global:__prospero_ran  = $false
        $global:__prospero_seen = $true
        __prospero_wrap
        # A theme reads $? to color its prompt: false again if it was
        if (!$ok) {
            Write-Error '' -ErrorAction Ignore
        }
        $p = if ($global:__prospero_prev) { & $global:__prospero_prev } else { 'PS ' + $PWD + '> ' }
        __prospero_mark 'A'
        $p
    }
    __prospero_wrap
}
