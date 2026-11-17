#!/bin/sh
# Use disposable settings/session folders and a private graphical display.
set -eu
build=$1
program=$2
state=$(mktemp -d)
trap 'rm -rf "$state"' EXIT HUP INT TERM
mkdir -p "$state/data" "$state/cache" "$state/config"
export GSETTINGS_SCHEMA_DIR="$build/data"
export GSETTINGS_BACKEND=memory
export XDG_DATA_HOME="$state/data"
export XDG_CACHE_HOME="$state/cache"
export XDG_CONFIG_HOME="$state/config"
export NO_AT_BRIDGE=1
exec_status=0
dbus-run-session -- xvfb-run -a "$program" || exec_status=$?
exit "$exec_status"
