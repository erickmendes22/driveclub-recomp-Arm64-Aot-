#!/usr/bin/env bash
set -eu
mkdir -p out/launcher-preview
adb install -r android/build/outputs/apk/debug/*.apk
adb shell wm size 1080x2160
adb shell wm density 420
adb shell am start -W -n org.driveclub.port/.PortActivity
# Read accessibility labels and tap their bounds independently of screen density.
query_node() {
    adb shell uiautomator dump /sdcard/launcher-window.xml >/dev/null
    adb pull /sdcard/launcher-window.xml /tmp/launcher-window.xml >/dev/null
    python3 - "$1" "${2:-find}" <<'PY'
import re, subprocess, sys, xml.etree.ElementTree as ET
nodes = list(ET.parse('/tmp/launcher-window.xml').iter('node'))
if any(n.get('text') == "Pixel Launcher isn't responding" for n in nodes):
    close = next(n for n in nodes if n.get('resource-id') == 'android:id/aerr_close')
    l,t,r,b = map(int, re.findall(r'\d+', close.get('bounds')))
    subprocess.run(['adb','shell','input','tap',str((l+r)//2),str((t+b)//2)],check=True)
    sys.exit(1)
for n in nodes:
    if sys.argv[1].casefold() in [n.get(k, '').casefold() for k in ('text', 'content-desc', 'resource-id')]:
        if sys.argv[2] == 'tap':
            l,t,r,b = map(int, re.findall(r'\d+', n.get('bounds')))
            subprocess.run(['adb','shell','input','tap',str((l+r)//2),str((t+b)//2)],check=True)
        break
else:
    sys.exit('Missing UI element: ' + sys.argv[1])
PY
}
node() {
    for attempt in 1 2 3 4 5; do
        if query_node "$@"; then return 0; fi
        sleep 1
    done
    cat /tmp/launcher-window.xml
    return 1
}
node 'Atualização 1.28'
adb exec-out screencap -p > out/launcher-preview/biblioteca.png
node 'Escolher pasta  ›' tap
# Wait for DocumentsUI to become visible after the asynchronous activity transition.
for attempt in 1 2 3 4 5; do
    adb shell uiautomator dump /sdcard/launcher-window.xml >/dev/null
    adb pull /sdcard/launcher-window.xml /tmp/launcher-window.xml >/dev/null
    if grep -qi 'package="[^"]*documentsui"' /tmp/launcher-window.xml; then break; fi
    sleep 1
done
grep -qi 'package="[^"]*documentsui"' /tmp/launcher-window.xml
adb shell input keyevent BACK
node 'Sistema' tap
node 'Processador gráfico'
adb exec-out screencap -p > out/launcher-preview/sistema.png
node 'Ver logs' tap
node 'Logs recentes'
node 'Fechar' tap
node 'Exportar logs' tap
node 'Save' tap
for attempt in 1 2 3 4 5; do
    exported=$(adb shell find /sdcard/ -name driveclub-diagnostico.txt | tr -d '\r' | head -1)
    if [ -n "$exported" ]; then break; fi
    sleep 1
done
test -n "$exported"
adb shell cat "$exported" > /tmp/driveclub-export.txt
grep -F '[Sessão]' /tmp/driveclub-export.txt
grep -F '[Vulkan]' /tmp/driveclub-export.txt
