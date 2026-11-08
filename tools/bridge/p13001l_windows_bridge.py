#!/usr/bin/env python3
"""Send one bounded command to the explicit P13001L WinPE USB agent.

This protocol is distinct from EFI Shell. Seeing a CDC device is insufficient:
P13WIN READY must be received before any command is transmitted.
"""
import argparse,datetime,json,re,secrets,time
from pathlib import Path
from p13001l_bridge import WinSerial,devices,select_port

def request(serial,command,log,timeout=150,expect_reboot=False,handshake=True,minimum_version=1):
    if not command or '\r' in command or '\n' in command or '\0' in command:
        raise ValueError('Use one nonempty command line without CR/LF/NUL')
    encoded=command.encode('utf-8')
    if len(encoded)>3500:raise ValueError('Command exceeds3500UTF-8 bytes')
    data=bytearray();deadline=time.monotonic()+20
    if handshake:
        while time.monotonic()<deadline:
            chunk=serial.read()
            if chunk:log.write(chunk);log.flush();data.extend(chunk)
            ready=re.search(rb'P13WIN READY ([12])',data)
            if ready:
                serial.p13_windows_version=int(ready[1]);break
            if len(data)>65536:raise RuntimeError('Unexpected output before WinPE handshake')
            time.sleep(.002)
        else:raise TimeoutError('No WinPE agent handshake; EFI CDC is a different interface')
    if getattr(serial,'p13_windows_version',0)<minimum_version:
        raise RuntimeError(f'Windows agent protocol{minimum_version} required before sending this command')
    nonce=secrets.token_hex(4).upper().encode('ascii');data.clear()
    serial.write(b'@P13 '+nonce+b' '+encoded+b'\r\n')
    deadline=time.monotonic()+timeout
    begin=re.compile(rb'\r?\nP13WIN BEGIN '+nonce+rb'\r?\n')
    end=re.compile(rb'\r?\nP13WIN END '+nonce+rb' ([0-9A-Fa-f]{8})\r?\n')
    while time.monotonic()<deadline:
        chunk=serial.read()
        if chunk:
            log.write(chunk);log.flush();data.extend(chunk)
            if len(data)>16*1024**2:raise RuntimeError('Command output exceeds16MiB')
            a=begin.search(data);b=end.search(data)
            if a and b and a.end()<=b.start():
                return data[a.end():b.start()].decode('utf-8',errors='replace'),int(b[1],16)
            if expect_reboot and a:
                return 'Reboot command accepted; firmware return still requires observation.\n',None
        else:time.sleep(.002)
    raise TimeoutError('WinPE command completion not received')

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--port');p.add_argument('--command',required=True)
    p.add_argument('--expect-reboot',action='store_true',help='For wpeutil reboot, return on acceptance; verify fresh firmware separately')
    p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    if a.expect_reboot and a.command.strip().lower()!='wpeutil reboot':p.error('--expect-reboot is only for wpeutil reboot')
    a.output.mkdir(parents=True,exist_ok=False);serial=None
    result={'command':a.command,'started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat()}
    try:
        port=select_port(devices(),a.port);result['port']=port;serial=WinSerial(port)
        with (a.output/'console.raw.log').open('wb') as log:output,code=request(serial,a.command,log,expect_reboot=a.expect_reboot)
        (a.output/'command-output.txt').write_text(output,encoding='utf-8')
        result.update(completed=code is not None,exit_code=code,reboot_command_accepted=a.expect_reboot and code is None);print(output,end='',flush=True)
        return 0 if code==0 or (a.expect_reboot and code is None) else 1
    except (OSError,ValueError,RuntimeError,TimeoutError) as error:
        result.update(completed=False,error=str(error));print(json.dumps(result),flush=True);return 1
    finally:
        if serial:serial.close()
        (a.output/'result.json').write_text(json.dumps(result,indent=2)+'\n')
if __name__=='__main__':raise SystemExit(main())
