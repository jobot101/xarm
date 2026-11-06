@echo off
rem Open the folder of webcam snapshots in Explorer.
if not exist "C:\Users\mark\Documents\Ragamuffin Workshop\johnny\shots" mkdir "C:\Users\mark\Documents\Ragamuffin Workshop\johnny\shots"
start "" explorer.exe "C:\Users\mark\Documents\Ragamuffin Workshop\johnny\shots"
echo opened the shots folder
