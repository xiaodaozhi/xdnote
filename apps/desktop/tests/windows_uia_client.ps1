param([Parameter(Mandatory = $true)][long]$WindowHandle)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
$timer = [System.Diagnostics.Stopwatch]::StartNew()
function Mark([string]$phase) { Write-Output ("{0}: {1} ms" -f $phase, $timer.ElapsedMilliseconds) }

function Find-Element([System.Windows.Automation.AutomationElement]$root, [string]$name) {
    $condition = New-Object System.Windows.Automation.PropertyCondition ([System.Windows.Automation.AutomationElement]::NameProperty, $name)
    $element = $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants, $condition)
    if (-not $element) { throw "Expected element not found: $name" }
    return $element
}
function Wait-Condition([scriptblock]$condition, [string]$failure) {
    for ($attempt = 0; $attempt -lt 100; ++$attempt) {
        if (& $condition) { return }
        Start-Sleep -Milliseconds 50
    }
    throw $failure
}
function Invoke-Element([System.Windows.Automation.AutomationElement]$element) {
    $pattern = $element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
    if (-not $pattern) { throw 'InvokePattern is unavailable' }
    $pattern.Invoke()
}

$root = [System.Windows.Automation.AutomationElement]::FromHandle([IntPtr]$WindowHandle)
if (-not $root) { throw 'Could not inspect the test window' }
$editor = Find-Element $root ([regex]::Unescape('Markdown \u6e90\u6587'))
$undo = Find-Element $root ([regex]::Unescape('\u64a4\u9500'))
$redo = Find-Element $root ([regex]::Unescape('\u91cd\u505a'))
$readOnly = Find-Element $root ([regex]::Unescape('\u8bbe\u4e3a\u53ea\u8bfb'))
Mark 'tree'
$text = $editor.GetCurrentPattern([System.Windows.Automation.TextPattern]::Pattern)
$value = $editor.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
if (-not $text -or -not $value) { throw 'TextPattern or ValuePattern is unavailable' }
if ($text.DocumentRange.GetText(-1) -ne '# fixture') { throw 'Unexpected initial fixture text' }
if ($value.Current.IsReadOnly) { throw 'Editable fixture reported read-only' }
$value.SetValue('UIA fixture updated')
Wait-Condition { $value.Current.Value -eq 'UIA fixture updated' } 'ValuePattern did not update the fixture'
Mark 'write'
Invoke-Element $undo
Wait-Condition { $value.Current.Value -eq '# fixture' } 'Undo did not restore the fixture'
Mark 'undo'
Invoke-Element $redo
Wait-Condition { $value.Current.Value -eq 'UIA fixture updated' } 'Redo did not restore the edit'
Mark 'redo'
Invoke-Element $readOnly
Wait-Condition { $value.Current.IsReadOnly } 'Read-only state was not exposed'
Mark 'readonly'
try { $value.SetValue('blocked') } catch { }
if ($value.Current.Value -ne 'UIA fixture updated') { throw 'Read-only fixture changed through ValuePattern' }
$editable = Find-Element $root ([regex]::Unescape('\u6062\u590d\u7f16\u8f91'))
Invoke-Element $editable
Wait-Condition { -not $value.Current.IsReadOnly } 'Editable state was not restored'
Mark 'editable'
Write-Output 'UIA tree, text, actions, undo, redo and read-only checks passed'
