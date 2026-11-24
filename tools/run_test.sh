#!/bin/sh
# tools/run_test.sh <seconds> <log> [VARIABLE=value ...]
# A test run that leaves any other open copy of the game alone (the one somebody is playing): at the end it closes only
# its own, by its Windows PID (before: taskkill by name, which closed them all). Its PID is the smrpg-recomp.exe that
# shows up when it is started and was not there before ($! is env's, and /proc/$!/winpid is its own, not the game's).
# Example: tools/run_test.sh 60 build/boot_X.log LARECOMP_NO_AUDIO=1 LARECOMP_INPUT="1300:A:8"
cd "$(dirname "$0")/.." || exit 1
secs=$1
log=$2
shift 2
running() { tasklist //FI "IMAGENAME eq smrpg-recomp.exe" //FO CSV //NH 2>/dev/null | grep -i smrpg-recomp; }
pids() { running | cut -d, -f2 | tr -d '"'; }
# With another copy open (also under the old name, lasrecomp.exe) it only starts with a separate data folder
# (LARECOMP_DATA_ROOT=... among the arguments: its own save/ and larecomp.ini): they share no saves or settings.
isolated=0
for a in "$@"; do case "$a" in LARECOMP_DATA_ROOT=?*) isolated=1 ;; esac; done
before=" $(pids | tr '\n' ' ') "
if [ $isolated = 0 ] && { [ -n "$(echo $before)" ] || tasklist //FI "IMAGENAME eq lasrecomp.exe" //NH 2>/dev/null | grep -qi lasrecomp; }; then
    echo "[test] the game is already open: not starting (with LARECOMP_DATA_ROOT=<separate folder>, it does)"
    exit 2
fi
if [ -d /ucrt64/bin ]; then export PATH="/ucrt64/bin:$PATH"; else export PATH="/c/msys64/ucrt64/bin:$PATH"; fi
env LARECOMP_FORCE_LAUNCHER=0 "$@" ./build/smrpg-recomp.exe --play extracted > "$log" 2>&1 &
pid=$!
winpid=
for i in $(seq 1 40); do
    for p in $(pids); do
        case "$before" in *" $p "*) ;; *) winpid=$p; break ;; esac
    done
    [ -n "$winpid" ] && break
    timeout 0.25 tail -f /dev/null
done
timeout "$secs" tail --pid="$pid" -f /dev/null
if [ -n "$winpid" ]; then taskkill //PID "$winpid" //T //F > /dev/null 2>&1; fi  # (/T: its child too - plat_exec restarts the program and waits)
wait "$pid" 2>/dev/null
echo "[test] done (Windows PID ${winpid:-?})"
