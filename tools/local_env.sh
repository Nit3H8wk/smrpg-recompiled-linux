# tools/local_env.sh: the paths of the tools outside the repository (the Android SDK and NDK, the JDK...), from
# local.env, at the root of the repository (the folder above this one). tools/android/apk.sh, tools/android/deps.sh and
# tools/lsfg/build.sh load it with ".". local.env: NAME=value per line, without quotes (spaces are fine); git ignores it
# (see local.env.example). What is already in the environment wins: the file only fills in what is missing.
# (Found by the path of this file, not by $1: a "." without arguments inherits those of the script that loads it.)
_le_f=$(dirname "${BASH_SOURCE[0]}")/../local.env
if [ -f "$_le_f" ]; then
    while IFS='=' read -r _le_k _le_v || [ -n "$_le_k" ]; do
        _le_v=${_le_v%$'\r'}
        case $_le_k in '' | \#* | *[!A-Za-z0-9_]*) continue ;; esac
        [ -n "${!_le_k:-}" ] || export "$_le_k=$_le_v"
    done < "$_le_f"
fi
unset _le_f _le_k _le_v
