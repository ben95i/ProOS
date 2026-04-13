#KeyHistory 0

; Disable Ctrl+Alt+Tab (Mouse Side Button)
^!Tab::
return

F1::
Run, C:\PROGRA~1\Automate\curl.exe "http://automate.home/api?arg=lrlightsoff&var=&action=main", , hide
return

^F1::
Run, C:\PROGRA~1\Automate\curl.exe "http://automate.home/api?arg=lroff&var=&action=main", , hide
return

F2::
Run, C:\PROGRA~1\Automate\curl.exe "http://automate.home/api?arg=lrlightson&var=&action=main", , hide
return

^F2::
Run, C:\PROGRA~1\Automate\curl.exe "http://automate.home/api?arg=lron&var=&action=main", , hide
return

F3::
Run, C:\PROGRA~1\Automate\curl.exe "http://automate.home/api?arg=shuffle&var=&action=leds", , hide
return

!F3::
Run, C:\PROGRA~1\Automate\curl.exe "http://automate.home/api?arg=pause&var=&action=leds", , hide
return

^F3::
Run, C:\PROGRA~1\Automate\curl.exe "http://automate.home/api?arg=stop&var=&action=leds", , hide
return

F5::
Run, PowerShell.exe -ExecutionPolicy Bypass -File "C:\Program Files\Automate\mouse_acc.ps1"
return

^F5::
Run, "C:\Program Files\Automate\p2p_toggle.bat"
return

F11::Volume_Mute
return

!F11::
Run, C:\PROGRA~1\Automate\curl.exe "http://automate.home/api?arg=lrxmit&var=mute&action=main", , hide
return

^F11::
KeyWait,F11,T0.5
If (ErrorLevel)
{
  Run, C:\PROGRA~1\Automate\curl.exe "http://automate.home/api?arg=lrxmit&var=hifitoggle&action=main", , hide
}
return

F12::Volume_Down
return

!F12::
Run, C:\PROGRA~1\Automate\curl.exe "http://automate.home/api?arg=lrxmit&var=voldwn&action=main", , hide
return

^F12::
Run, C:\PROGRA~1\Automate\curl.exe "http://automate.home/api?arg=lrxmit&var=voldwnc&action=main", , hide
return

Insert::Volume_Up
return

!Insert::
Run, C:\PROGRA~1\Automate\curl.exe "http://automate.home/api?arg=lrxmit&var=volup&action=main", , hide
return

^Insert::
Run, C:\PROGRA~1\Automate\curl.exe "http://automate.home/api?arg=lrxmit&var=volupc&action=main", , hide
return


