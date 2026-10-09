DlssnrAmdRuntime.dll test kit
==============================

This checks the Windows side of DlssnrAmdRuntime.dll, the neural runtime of this project for AMDNR's
OptiScaler: that a Vulkan device of its own can share memory and a fence with a D3D12 device on your
Radeon, and that the network then runs and gives the right picture.

What you need
  - A Radeon card and the current Adrenalin driver.
  - The one extra file you were sent separately, dlssnr.bin.

Steps
  1. Unzip this whole folder somewhere (not from inside the zip).
  2. Put dlssnr.bin into the folder dlssnr-amd, next to the folder shaders.
  3. Close games and other programs that use the graphics card.
  4. Double-click "Run test.bat". If Windows says "Windows protected your PC", click "More info", then
     "Run anyway" (the programs are not signed). It takes 1 to 5 minutes; the network compiles on the
     first run and the screen may flicker for a moment.
  5. Send back results.txt and dlssnr-amd.log, both in the same folder.

What the answer looks like
  - "native transport ready" near the top of dlssnr-amd.log: D3D12 and Vulkan share memory and a fence.
  - "PSNR against the reference: answer 45 dB or so": the network ran and the picture is right.
  - If the second does not show but the first does, send the results anyway: the line "session:" says why.
  - If Create fails, results.txt names the step that failed (share a buffer, import it, import the fence).
