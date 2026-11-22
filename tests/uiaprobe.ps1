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
#    walks it;
# 2. finds the element with the keyboard focus (HasKeyboardFocus) and
#    requires the prompt: an Edit named Prompt. The window is not the
#    user's (the checklist's never takes the foreground), so not the
#    desktop's FocusedElement;
# 3. finds the Button named Send, requires its Invoke pattern, and
#    invokes it; the checklist then requires the prompt empty, as the
#    press sent it;
# 4. finds a point in Send and asks the desktop what is there, which
#    must be Send.
#
# Each check prints "ok    WHAT" or "FAIL  WHAT: DETAIL", the tree
# "tree: ...", and the last line "status: pass|fail: MESSAGE". Exits 1
# if a check failed, else 0. It touches nothing but Send.
#
# This is free and unencumbered software released into the public domain.

param([Parameter(Mandatory=$true)][int]$ProcessId)

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
$both = New-Object System.Windows.Automation.AndCondition(
    (Cond $AE::ProcessIdProperty $ProcessId), (Cond $AE::ClassNameProperty "prospero"))
while (-not $window -and (Get-Date) -lt $deadline) {
    $window = $AE::RootElement.FindFirst($Scope::Children, $both)
    if (-not $window) { Start-Sleep -Milliseconds 200 }
}
Check ($window -ne $null) "the window found" "no window of class prospero for pid $ProcessId"
if (-not $window) {
    Write-Output "status: fail: no window"
    exit 1
}

# 1. The tree, as the control view walks it
$walker = [System.Windows.Automation.TreeWalker]::ControlViewWalker
$count = 0
function Dump($e, $depth) {
    $c = $walker.GetFirstChild($e)
    while ($c -and $script:count -lt 2000) {
        $script:count++
        Write-Output ("tree: " + ("  " * $depth) + (Describe $c))
        if ($depth -lt 24) { Dump $c ($depth + 1) }
        $c = $walker.GetNextSibling($c)
    }
}
Dump $window 0
Check ($count -gt 10) "the tree has elements" "$count"

# 2. The keyboard's focus: the prompt
$focus = $window.FindFirst($Scope::Descendants, (Cond $AE::HasKeyboardFocusProperty $true))
$isprompt = $focus -and $focus.Current.ControlType -eq $CT::Edit -and $focus.Current.Name -eq "Prompt"
Check $isprompt "the focus is the prompt, an Edit" (Describe $focus)

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
