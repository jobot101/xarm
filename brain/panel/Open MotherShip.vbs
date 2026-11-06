' Opens the Shop MotherShip control panel in its own window (built like Ragamuffin Video Control).
' Starts the little local server first if it isn't running. If the panel is already open, brings it to the front.
Option Explicit
Dim sh, fso, here, url, py, chrome, profile, i
Set sh = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")
here = fso.GetParentFolderName(WScript.ScriptFullName)
url = "http://127.0.0.1:47620/"
profile = sh.ExpandEnvironmentStrings("%LOCALAPPDATA%") & "\ShopMotherShipPanel"

Function Up()
  Dim x
  Up = False
  On Error Resume Next
  Set x = CreateObject("MSXML2.ServerXMLHTTP.6.0")
  x.setTimeouts 2000, 2000, 3000, 3000
  x.Open "GET", url & "ping", False
  x.Send
  If Err.Number = 0 Then If x.Status = 200 Then Up = True
  On Error GoTo 0
End Function

' Already open? Bring that window to the front instead of opening a second one.
' (Matches the window's own browser process by its private profile folder.)
Dim wmi, procs, p
Set wmi = GetObject("winmgmts:\\.\root\cimv2")
Set procs = wmi.ExecQuery("SELECT ProcessId, CommandLine FROM Win32_Process WHERE Name='chrome.exe' OR Name='msedge.exe'")
For Each p In procs
  If Not IsNull(p.CommandLine) Then
    If InStr(1, p.CommandLine, "ShopMotherShipPanel", 1) > 0 And InStr(1, p.CommandLine, "--type=", 1) = 0 Then
      If Up() Then
        If sh.AppActivate(p.ProcessId) Then WScript.Quit
      End If
    End If
  End If
Next

If Not Up() Then
  py = fso.GetParentFolderName(here) & "\.venv\Scripts\pythonw.exe"
  If Not fso.FileExists(py) Then py = "pythonw.exe"
  sh.CurrentDirectory = here
  sh.Run """" & py & """ ""server.py"" --no-browser", 0, False
  For i = 1 To 40
    WScript.Sleep 250
    If Up() Then Exit For
  Next
  If Not Up() Then
    MsgBox "The MotherShip panel didn't start." & vbCrLf & vbCrLf & _
      "To see why, open PowerShell in" & vbCrLf & here & vbCrLf & "and run:  ..\.venv\Scripts\python.exe server.py", 48, "Shop MotherShip"
    WScript.Quit 1
  End If
End If

chrome = sh.ExpandEnvironmentStrings("%ProgramFiles%") & "\Google\Chrome\Application\chrome.exe"
If Not fso.FileExists(chrome) Then chrome = sh.ExpandEnvironmentStrings("%LOCALAPPDATA%") & "\Google\Chrome\Application\chrome.exe"
If Not fso.FileExists(chrome) Then chrome = sh.ExpandEnvironmentStrings("%ProgramFiles(x86)%") & "\Microsoft\Edge\Application\msedge.exe"
If fso.FileExists(chrome) Then
  sh.Run """" & chrome & """ --app=" & url & " --user-data-dir=""" & profile & """ --no-first-run --window-size=1400,920", 1, False
Else
  sh.Run url, 1, False
End If
