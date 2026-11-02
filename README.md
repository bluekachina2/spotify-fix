# spotify-fix

Zygisk experiment: make Spotify's AAudio streams take the normal shared,
power-saving path so system screen recorders can capture them.

Build: push to GitHub, the workflow produces `aaudiocapturefix-v0.3.zip`
(Actions -> run -> Artifacts). Flash it in Magisk and reboot.

Change behaviour without rebuilding (root shell):

    echo 11 > /data/adb/modules/aaudiocapturefix/mode   # POWER_SAVING (default)
    echo 10 > /data/adb/modules/aaudiocapturefix/mode   # NONE
    echo 12 > /data/adb/modules/aaudiocapturefix/mode   # LOW_LATENCY
    echo 0  > /data/adb/modules/aaudiocapturefix/mode   # observe only, change nothing

Force-stop Spotify after changing it. Logs:

    logcat -d | grep AAudioCaptureFix
