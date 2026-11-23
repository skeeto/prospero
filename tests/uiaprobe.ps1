# Prospero's window as another process sees it through UI Automation,
# as Voice Access and Narrator do: the GUI checklist's outside check
# (gui.py --mode a11y, on Windows), which runs it once its prompt,
# focused, holds the checklist's text.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File tests\uiaprobe.ps1 PID
#
# Through .NET's System.Windows.Automation, a UI Automation client, of
# the GUI with process id PID:
#
# 1. finds its window (class "prospero") among the desktop's, and dumps
#    the tree under it, control types and names, as the control view
#    walks it, with each element's patterns and, when a hit test at its
#    center finds another, what, and when it pokes out of its parent,
#    where (reported, not checked);
# 2. finds the element with the keyboard focus (HasKeyboardFocus) and
#    requires the prompt: an Edit named Prompt. The window is not the
#    user's (the checklist's never takes the foreground), so not the
#    desktop's FocusedElement. It reads its text through the Text pattern, finds a word of it,
#    selects it and requires the selection and the word's rectangle;
# 3. finds the Button named Send, requires its Invoke pattern, and
#    invokes it; the checklist then requires the prompt empty, as the
#    press sent it;
# 4. finds a point in Send and asks the desktop what is there, which
#    must be Send.
#
# With -DumpOnly, only the tree (step 1), touching nothing: for a user's
# own window, as Voice Access sees it. Without PID, the first window of
# class prospero.
#
# Each check prints "ok    WHAT" or "FAIL  WHAT: DETAIL", the tree
# "tree: ...", and the last line "status: pass|fail: MESSAGE". Exits 1
# if a check failed, else 0. It touches nothing but Send.
#
# This is free and unencumbered software released into the public domain.

param([int]$ProcessId = 0, [switch]$DumpOnly)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$AE     = [System.Windows.Automation.AutomationElement]
$Scope  = [System.Windows.Automation.TreeScope]
$CT     = [System.Windows.Automation.ControlType]
$failed = 0

function Check($ok, $what, $detail) {
    if ($ok) {
        Write-Output "ok    $what"
    } else {
        Write-Output "FAIL  ${what}: $detail"
        $script:failed++
    }
}

function Cond($prop, $value) {
    New-Object System.Windows.Automation.PropertyCondition($prop, $value)
}

function Describe($e) {
    if (-not $e) { return "(none)" }
    $c = $e.Current
    "$($c.ControlType.ProgrammaticName -replace '^ControlType\.', '') '$($c.Name)'"
}

# The window, waited for: it may still be opening
$window = $null
$deadline = (Get-Date).AddSeconds(10)
$both = if ($ProcessId) {
    New-Object System.Windows.Automation.AndCondition(
        (Cond $AE::ProcessIdProperty $ProcessId), (Cond $AE::ClassNameProperty "prospero"))
} else {
    Cond $AE::ClassNameProperty "prospero"  # any Prospero window, the first
}
while (-not $window -and (Get-Date) -lt $deadline) {
    $window = $AE::RootElement.FindFirst($Scope::Children, $both)
    if (-not $window) { Start-Sleep -Milliseconds 200 }
}
Check ($window -ne $null) "the window found" "no window of class prospero for pid $ProcessId"
if (-not $window) {
    Write-Output "status: fail: no window"
    exit 1
}

# An element's patterns, and what a client clicking it by voice would
# find at its center, if not itself (as Voice Access may check)
function Extra($e) {
    $pats = ($e.GetSupportedPatterns() | ForEach-Object {
        $_.ProgrammaticName -replace 'PatternIdentifiers\.Pattern$', '' }) -join ','
    $out = if ($pats) { " [$pats]" } else { "" }
    $r = $e.Current.BoundingRectangle
    if ($r.IsEmpty) {
        return "$out (no rectangle)"
    }
    $pt = New-Object System.Windows.Point(($r.Left + $r.Width/2), ($r.Top + $r.Height/2))
    $at = $AE::FromPoint($pt)
    if (-not ($at -and [System.Windows.Automation.Automation]::Compare($at, $e))) {
        $out += " (its center is " + (Describe $at) + ")"
    }
    $up = $walker.GetParent($e)
    $pr = if ($up) { $up.Current.BoundingRectangle } else { $null }
    if ($pr -and -not $pr.IsEmpty -and -not $pr.Contains($r)) {
        $out += " (outside its parent: $r in $pr)"
    }
    $out
}

# 1. The tree, as the control view walks it
$walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
$count = 0
function Dump($e, $depth) {
    $c = $walker.GetFirstChild($e)
    while ($c -and $script:count -lt 2000) {
        $script:count++
        Write-Output ("tree: " + ("  " * $depth) + (Describe $c) + (Extra $c))
        if ($depth -lt 24) { Dump $c ($depth + 1) }
        $c = $walker.GetNextSibling($c)
    }
}
Dump $window 0
Check ($count -gt 10) "the tree has elements" "$count"
if ($DumpOnly) {
    exit 0
}

# 2. The keyboard's focus: the prompt
$focus = $window.FindFirst($Scope::Descendants, (Cond $AE::HasKeyboardFocusProperty $true))
$isprompt = $focus -and $focus.Current.ControlType -eq $CT::Edit -and $focus.Current.Name -eq "Prompt"
Check $isprompt "the focus is the prompt, an Edit" (Describe $focus)

# 2b. The prompt's text through the Text pattern, as Voice Access reads
# it, and a word of it selected as "select WORD" does
if ($isprompt) {
    $tp = $null
    $hastext = $focus.TryGetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern, [ref]$tp)
    Check $hastext "the prompt has the Text pattern" "no"
    if ($hastext) {
        $all = $tp.DocumentRange.GetText(-1)
        Check ($all -eq "pressed from another process") "the prompt's text through it" "'$all'"
        $found = $tp.DocumentRange.FindText("another", $false, $true)
        Check ($found -ne $null) "the word found in it" "none"
        if ($found) {
            $found.Select()
            Start-Sleep -Milliseconds 500
            $sel = $tp.GetSelection()
            $got = if ($sel.Length -eq 1) { $sel[0].GetText(-1) } else { "$($sel.Length) ranges" }
            Check ($got -eq "another") "the word selected" "'$got'"
            $rects = $found.GetBoundingRectangles()
            $r = if ($rects.Length) { $rects[0] } else { $null }
            $pr = $focus.Current.BoundingRectangle
            $within = $r -and $r.Width -gt 0 -and $r.Left -ge $pr.Left -and $r.Right -le $pr.Right -and
                      $r.Top -ge $pr.Top -and $r.Bottom -le $pr.Bottom
            Check $within "the word's rectangle in the prompt's" "$r in $pr"
        }
    }
}

# 3. Send, invoked
$sendcond = New-Object System.Windows.Automation.AndCondition(
    (Cond $AE::ControlTypeProperty $CT::Button), (Cond $AE::NameProperty "Send"))
$send = $window.FindFirst($Scope::Descendants, $sendcond)
Check ($send -ne $null) "Send found, a Button" "none"
if ($send) {
    $rect = $send.Current.BoundingRectangle
    $wrect = $window.Current.BoundingRectangle
    $inside = -not $rect.IsEmpty -and $rect.Left -ge $wrect.Left -and $rect.Top -ge $wrect.Top -and
              $rect.Right -le $wrect.Right -and $rect.Bottom -le $wrect.Bottom
    Check $inside "Send is where the window is" "$rect in $wrect"

    # 4. What is at a point of it: Send
    if (-not $rect.IsEmpty) {
        $pt = New-Object System.Windows.Point(($rect.Left + $rect.Width/2), ($rect.Top + $rect.Height/2))
        $at = $AE::FromPoint($pt)
        $same = $at -and [System.Windows.Automation.Automation]::Compare($at, $send)
        Check $same "the element at Send's center is Send" (Describe $at)
    }

    $pattern = $null
    $has = $send.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$pattern)
    Check $has "Send has the Invoke pattern" "no"
    if ($has) {
        try {
            $pattern.Invoke()
            Check $true "Send invoked" ""
        } catch {
            Check $false "Send invoked" $_.Exception.Message
        }
    }
}

if ($failed) {
    Write-Output "status: fail: $failed failed"
    exit 1
}
Write-Output "status: pass: all passed"
exit 0
