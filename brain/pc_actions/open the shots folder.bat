@echo off
rem Open the folder of webcam snapshots in Explorer.
if not exist "C:\Users\mark\Documents\Ragamuffin Workshop\brain\shots" mkdir "C:\Users\mark\Documents\Ragamuffin Workshop\brain\shots"
start "" explorer.exe "C:\Users\mark\Documents\Ragamuffin Workshop\brain\shots"
echo opened the shots folder
