@echo off
rem INSTALL.CMD - stage + install MaliKmd on the tablet (run elevated via the agent).
rem Order matters: a test-signed driver only loads after its cert is trusted.
setlocal
set S=C:\P13Stage\MTKMALI
cd /d "%S%"

echo ==== P13 MALI KMD INSTALL ====
echo [elevation]
whoami /groups | findstr /i "12288 High"
echo [secure boot state - UEFISecureBootEnabled must be 0 or absent]
reg query "HKLM\SYSTEM\CurrentControlSet\Control\SecureBoot\State" /v UEFISecureBootEnabled 2>nul
echo [testsigning]
bcdedit /enum {current} | findstr /i "testsigning"
echo [trust test cert - Root]
certutil -addstore -f Root "%S%\P13Test.cer"
echo root_exit=%errorlevel%
echo [trust test cert - TrustedPublisher]
certutil -addstore -f TrustedPublisher "%S%\P13Test.cer"
echo publisher_exit=%errorlevel%
echo [stage files]
dir /b "%S%"
echo [pnputil add-driver]
pnputil /add-driver "%S%\MaliKmd.inf" /install
echo pnputil_exit=%errorlevel%
echo [devcon install]
"%S%\devcon.exe" install "%S%\MaliKmd.inf" ROOT\MaliMtk
if errorlevel 1 "%S%\devcon.exe" update "%S%\MaliKmd.inf" ROOT\MaliMtk
echo devcon_exit=%errorlevel%
echo [devcon status]
"%S%\devcon.exe" status ROOT\MaliMtk
echo [service]
sc start MaliKmd >nul 2>&1
sc query MaliKmd
echo [malitest: version, caps, SPM power status - read only]
"%S%\malitest.exe" --irq
echo ==== INSTALL DONE ====