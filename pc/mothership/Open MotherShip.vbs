' Opens the Shop MotherShip control panel in its own window (like Ragamuffin Video Control).
' Starts the little local server first if it isn't running.
Option Explicit
Dim sh, fso, here, url, py, chrome, i
Set sh = CreateObject("WScript.Shell")
Set fso = CreateObject("Scripting.FileSystemObject")
here = fso.GetParentFolderName(WScript.ScriptFullName)
url = "http://127.0.0.1:47620/"

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

If Not Up() Then
  py = fso.GetParentFolderName(here) & "\.venv\Scripts\pythonw.exe"
  If Not fso.FileExists(py) Then py = "pythonw.exe"
  sh.CurrentDirectory = here
  sh.Run """" & py & """ ""server.py"" --no-browser", 0, False
  For i = 1 To 40
    WScript.Sleep 250
    If Up() Then Exit For
  Next
End If

chrome = sh.ExpandEnvironmentStrings("%ProgramFiles%") & "\Google\Chrome\Application\chrome.exe"
If Not fso.FileExists(chrome) Then chrome = sh.ExpandEnvironmentStrings("%ProgramFiles(x86)%") & "\Microsoft\Edge\Application\msedge.exe"
sh.Run """" & chrome & """ --app=" & url & " --user-data-dir=""" & sh.ExpandEnvironmentStrings("%LOCALAPPDATA%") & "\ShopMotherShipPanel"" --window-size=1400,920", 1, False
