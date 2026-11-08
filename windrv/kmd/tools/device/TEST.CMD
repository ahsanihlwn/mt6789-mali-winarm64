@echo off
rem TEST.CMD - hardware bring-up steps that touch the GPU.
rem Run only after INSTALL.CMD reports the device started.
setlocal
set S=C:\P13Stage\MTKMALI
cd /d "%S%"

echo ==== P13 MALI KMD BRING-UP ====
echo [before]
"%S%\malitest.exe" --irq
echo [power on]
"%S%\malitest.exe" --on
echo [after]
"%S%\malitest.exe" --irq
echo [job manager status]
"%S%\malitest.exe" --jstatus
echo [mmu + buffer object]
"%S%\malitest.exe" --bo
echo [null job + irq]
"%S%\malitest.exe" --job --irq
echo ==== BRING-UP DONE ====