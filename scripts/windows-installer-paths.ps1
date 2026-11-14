# Read-only Unicode shortcut metadata and handle-based target comparison.
# WSH exposes ANSI target paths on some hosts; question marks must not be treated as wildcards.
Add-Type @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
public static class XdnoteInstallerPath {
 [DllImport("kernel32.dll", CharSet=CharSet.Unicode, ExactSpelling=true, SetLastError=true)]
 private static extern uint GetFinalPathNameByHandleW(IntPtr file, StringBuilder output, uint length, uint flags);
 [ComImport, Guid("000214F9-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
 private interface IShellLinkUnicode {
   [PreserveSig] int GetPath([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder output, int length, IntPtr findData, uint flags);
 }
 public static string ReadShortcut(string name) {
   object link = Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("00021401-0000-0000-C000-000000000046")));
   // IPersistFile and IShellLinkW are read-only here. Release the COM object even on failure.
   try {
     ((System.Runtime.InteropServices.ComTypes.IPersistFile)link).Load(name, 0);
     var output = new StringBuilder(32768);
     Marshal.ThrowExceptionForHR(((IShellLinkUnicode)link).GetPath(output, 32768, IntPtr.Zero, 0));
     return output.ToString();
   } finally { Marshal.FinalReleaseComObject(link); }
 }
 public static string Canonical(string path) {
   // Open only existing QA-owned files, permit other readers/writers, never mutate.
   // The using scope keeps the handle live for the native query and always closes it.
   using (var stream = new FileStream(Path.GetFullPath(path), FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete)) {
     const int capacity = 32768;
     var buffer = new StringBuilder(capacity);
     uint size = GetFinalPathNameByHandleW(stream.SafeFileHandle.DangerousGetHandle(), buffer, capacity, 0);
     if (size == 0 || size >= capacity) throw new Win32Exception(Marshal.GetLastWin32Error());
     return buffer.ToString();
   }
 }
}
'@
function Get-XdnoteCanonicalPath([string]$Path) {
    return [XdnoteInstallerPath]::Canonical($Path)
}
function Assert-XdnoteShortcut([string]$Executable, [string]$EvidenceDirectory = 'build/installer-smoke') {
    $link = Join-Path ([Environment]::GetFolderPath('Programs')) 'xdnote.lnk'
    $exists = Test-Path -LiteralPath $link
    $target = if ($exists) { [XdnoteInstallerPath]::ReadShortcut($link) } else { '' }
    $diagnostic = @{ link_exists=$exists; target_exists=($target -and (Test-Path -LiteralPath $target)); target_uses_versions=($target -match '\\versions\\'); target_uses_short_alias=($target -match '~[0-9]'); expected_exists=(Test-Path -LiteralPath $Executable) }
    New-Item -ItemType Directory -Force $EvidenceDirectory | Out-Null
    $diagnostic | ConvertTo-Json | Set-Content (Join-Path $EvidenceDirectory 'windows-shortcut-diagnostic.json')
    if (!$exists -or !$target) { throw 'Installed Start menu shortcut is missing or has no target' }
    $actual = Get-XdnoteCanonicalPath $target
    $expected = Get-XdnoteCanonicalPath $Executable
    $diagnostic.canonical_target_matches = $actual -eq $expected
    $diagnostic.expected_uses_versions = $expected -match '\\versions\\'
    $diagnostic | ConvertTo-Json | Set-Content (Join-Path $EvidenceDirectory 'windows-shortcut-diagnostic.json')
    if ($actual -ne $expected) { throw 'Shortcut did not migrate to the app root' }
}
