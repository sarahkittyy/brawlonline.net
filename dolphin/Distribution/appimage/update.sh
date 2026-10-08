  
#!/bin/bash

	if [ -f /usr/bin/zenity ]; then
		zenity --question --timeout=10 --title="dolphin updater" --text="New update available. Update now?" \
		--icon-name=project-plus-dolphin --window-icon=project-plus-dolphin.svg --height=80 --width=400
		answer=$?	
	else
		dialog --title dolphin --timeout 10 --yesno "New update available. Update now?" 0 0
		answer=$?
	fi

if [ "$answer" -eq 0 ]; then 
	$APPDIR/usr/bin/AppImageUpdate $PWD/Project+_Dolphin-x86_64.AppImage "$@" && $PWD/Project+_Dolphin-x86_64.AppImage "$@"
else
	$APPDIR/AppRun-patched "$@"
fi
exit 0


