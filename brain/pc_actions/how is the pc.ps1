# Report the PC's free disk space, GPU memory and whether LM Studio is up (short, for speaking aloud).
$d = Get-PSDrive C; "C drive: {0:N1} GB free" -f ($d.Free/1GB)
try { $g = (nvidia-smi --query-gpu=memory.used,memory.total --format=csv,noheader) -split ","; "GPU: $($g[0].Trim()) of $($g[1].Trim()) used" } catch {}
try { Invoke-WebRequest -UseBasicParsing -TimeoutSec 6 http://localhost:1234/v1/models | Out-Null; "LM Studio: up" } catch { "LM Studio: not answering" }
