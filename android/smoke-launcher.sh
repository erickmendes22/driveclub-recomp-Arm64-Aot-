#!/usr/bin/env bash
set -euo pipefail
mkdir -p out/launcher-preview
adb_binary=$(type -P adb)
# A hung instrumentation runner or accessibility dump must leave evidence and
# fail this gate instead of consuming the entire job timeout.
adb() { timeout --foreground 30s "$adb_binary" "$@"; }
phase() {
    printf '[launcher-smoke] %s\n' "$1"
    printf '%s\n' "$1" >out/launcher-preview/last-phase.txt
}
finish() {
    local result=$1
    if [ -n "${controls_log_pid:-}" ]; then kill "$controls_log_pid" 2>/dev/null || true; fi
    if [ "$result" -ne 0 ]; then
        adb exec-out screencap -p >out/launcher-preview/failure.png 2>/dev/null || true
        adb shell dumpsys activity activities >out/launcher-preview/activities.txt 2>&1 || true
        adb shell dumpsys window >out/launcher-preview/windows.txt 2>&1 || true
        adb shell dumpsys activity instrumentation >out/launcher-preview/runners.txt 2>&1 || true
    fi
    adb logcat -d -t 5000 >out/launcher-preview/logcat.txt 2>&1 || true
}
trap 'finish "$?"' EXIT
phase 'Installing the launcher and test APKs'
adb install -r android/build/outputs/apk/debug/*.apk
adb install -r android/build/outputs/apk/androidTest/debug/*.apk
adb shell wm size 1080x2160
adb shell wm density 420
"$adb_binary" logcat -v threadtime DriveclubControlsTest:I '*:S' >out/launcher-preview/control-test-phases.txt 2>&1 &
controls_log_pid=$!
phase 'Running six Android integration tests (180-second deadline)'
timeout --foreground 180s "$adb_binary" shell am instrument -w -r org.driveclub.port.test/android.test.InstrumentationTestRunner \
    | tee out/launcher-preview/instrumentation.txt
grep -Fq 'OK (6 tests)' out/launcher-preview/instrumentation.txt
phase 'Opening the game library'
adb shell am start -W -n org.driveclub.port/.PortActivity
# Read accessibility labels and tap their bounds independently of screen density.
query_node() {
    adb shell uiautomator dump /sdcard/launcher-window.xml >/dev/null || return
    adb pull /sdcard/launcher-window.xml /tmp/launcher-window.xml >/dev/null || return
    python3 - "$1" "${2:-find}" <<'PY'
import re, subprocess, sys, xml.etree.ElementTree as ET
nodes = list(ET.parse('/tmp/launcher-window.xml').iter('node'))
if any(n.get('resource-id') == 'android:id/immersive_cling_title' for n in nodes):
    acknowledge = next(n for n in nodes if n.get('resource-id') == 'android:id/ok')
    l,t,r,b = map(int, re.findall(r'\d+', acknowledge.get('bounds')))
    subprocess.run(['adb','shell','input','tap',str((l+r)//2),str((t+b)//2)],check=True,timeout=30)
    print('Dismissed the Android first-use full-screen tutorial.')
    sys.exit(1)
if any(n.get('text') == "Pixel Launcher isn't responding" for n in nodes):
    close = next(n for n in nodes if n.get('resource-id') == 'android:id/aerr_close')
    l,t,r,b = map(int, re.findall(r'\d+', close.get('bounds')))
    subprocess.run(['adb','shell','input','tap',str((l+r)//2),str((t+b)//2)],check=True,timeout=30)
    sys.exit(1)
for n in nodes:
    if sys.argv[1].casefold() in [n.get(k, '').casefold() for k in ('text', 'content-desc', 'resource-id')]:
        if sys.argv[2] == 'tap':
            l,t,r,b = map(int, re.findall(r'\d+', n.get('bounds')))
            subprocess.run(['adb','shell','input','tap',str((l+r)//2),str((t+b)//2)],check=True,timeout=30)
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
scroll_to() {
    for attempt in 1 2 3 4 5 6; do
        if query_node "$@"; then return 0; fi
        adb shell input swipe 540 1750 540 850 350
    done
    cat /tmp/launcher-window.xml
    return 1
}
scroll_top() {
    for attempt in 1 2 3; do adb shell input swipe 540 500 540 1800 250; done
}
node 'Atualização separada (opcional)'
adb exec-out screencap -p > out/launcher-preview/biblioteca.png
phase 'Checking the folder picker'
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
phase 'Checking the PKG picker'
scroll_to 'Instalar / atualizar por PKG' tap
for attempt in 1 2 3 4 5; do
    adb shell uiautomator dump /sdcard/launcher-window.xml >/dev/null
    adb pull /sdcard/launcher-window.xml /tmp/launcher-window.xml >/dev/null
    if grep -qi 'package="[^"]*documentsui"' /tmp/launcher-window.xml; then break; fi
    sleep 1
done
grep -qi 'package="[^"]*documentsui"' /tmp/launcher-window.xml
adb shell input keyevent BACK
scroll_top
phase 'Checking system information'
node 'Sistema' tap
node 'Processador gráfico'
adb exec-out screencap -p > out/launcher-preview/sistema.png
phase 'Checking racing controls'
scroll_to 'Testar controle na tela' tap
node 'Mostrar botões de menu'
adb exec-out screencap -p > out/launcher-preview/controles-corrida.png
phase 'Checking menu controls'
node 'Mostrar botões de menu' tap
node 'Mostrar controles de corrida'
adb exec-out screencap -p > out/launcher-preview/controles-menu.png
node 'Voltar à garagem' tap
phase 'Checking persistent diagnostics'
scroll_top
scroll_to 'Ver logs' tap
node 'Logs recentes'
node 'Fechar' tap
phase 'Exporting the diagnostic log'
scroll_to 'Exportar logs' tap
node 'Save' tap
# DocumentsUI can create an empty file before delivering the URI to our activity.
for attempt in $(seq 1 20); do
    exported=$(adb shell find /sdcard/ -name driveclub-diagnostico.txt | tr -d '\r' | head -1)
    if [ -n "$exported" ] && adb shell cat "$exported" > /tmp/driveclub-export.txt &&
        grep -Fq '[Sessão]' /tmp/driveclub-export.txt && grep -Fq '[Vulkan]' /tmp/driveclub-export.txt; then
        phase 'All integration and launcher checks passed'
        echo 'Verified session and Vulkan records in the exported document.'
        exit 0
    fi
    sleep 1
done
adb shell run-as org.driveclub.port cat files/driveclub.log
adb shell ls -l /sdcard/Download/
exit 1
