# Metro Exodus (DX12, m12 proxy) - live test

**Result: PASS** (the launch-time silent-death bug is fixed)

The previous failure signature was: process exits on the first processed
frame. This run: process stayed alive for minutes, the m12 DXGI proxy
loaded the in-process engine and processed thousands of presents.

Evidence (%TEMP%\m12_dxgi.log):

    [m12_dxgi] in-process DLSS 5 active (TCP bypassed, scale=1.00
               gain=1.00 blend=1.00)   <- twice (game rebuilt its swapchain)
    [m12_dxgi] in-process processed 800x600 (frame 7441 ... 7526+)
    present #7500+ on sc ...           <- continuous stream, ~4.5 proc/s

- Process MetroExodus.exe (PID 13940, window "4A Engine") was Responding
  the whole time; killed cleanly via Stop-Process after evidence capture.
- Harmless noise: `stub export called: CompatValue` lines are the proxy's
  placeholder DXGI exports; unrelated to processing.
- The overseer agent supervising this test hung on its polling loop; the
  verdict was confirmed directly from the process list + log tail.
