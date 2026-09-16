#!/usr/bin/env bash
# Copies a built plugin into the directory OBS actually scans on Windows.
#
# Not %APPDATA%. OBS's AddExtraModulePaths() uses GetProgramDataPath() on
# Windows and GetAppConfigPath() only on macOS/Linux, so a plugin dropped in
# Roaming is never even opened — it does not fail to load, it is never seen.
# Failures here are logged by OBS at LOG_DEBUG, so run obs64.exe --verbose
# when a plugin does not appear in "Loaded Modules".
set -euo pipefail

config="${1:-RelWithDebInfo}"
src="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/build_x64/rundir/${config}"
dest="/c/ProgramData/obs-studio/plugins/obs-tm-director"

[ -f "${src}/obs-tm-director.dll" ] || { echo "no build at ${src}" >&2; exit 1; }

mkdir -p "${dest}/bin/64bit" "${dest}/data"
cp "${src}/obs-tm-director.dll" "${dest}/bin/64bit/"
[ -f "${src}/obs-tm-director.pdb" ] && cp "${src}/obs-tm-director.pdb" "${dest}/bin/64bit/"
cp -r "${src}/obs-tm-director/." "${dest}/data/"

echo "deployed ${config} to ${dest}"
