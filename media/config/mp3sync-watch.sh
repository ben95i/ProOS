#!/bin/bash
#
# Watches MP3SYNC_INPUT recursively and triggers a single mp3sync run once
# the tree has been quiet (no writes/creates/moves) for MP3SYNC_QUIET_SECONDS.
# The inotifywait monitor is never stopped/restarted, so events that occur
# while a conversion pass is running are queued, not lost.

set -uo pipefail

: "${MP3SYNC_INPUT:?MP3SYNC_INPUT is not set}"
: "${MP3SYNC_OUTPUT:?MP3SYNC_OUTPUT is not set}"
: "${MP3SYNC_BITRATE:?MP3SYNC_BITRATE is not set}"
: "${MP3SYNC_THREADS:?MP3SYNC_THREADS is not set}"
: "${MP3SYNC_QUIET_SECONDS:=60}"

MP3SYNC_BIN="/usr/bin/mp3sync"

echo "Watching '${MP3SYNC_INPUT}' recursively (quiet period: ${MP3SYNC_QUIET_SECONDS}s)."

# Long-lived recursive monitor feeding fd 3. inotifywait -r re-adds watches
# for newly created subdirectories automatically as it sees them.
exec 3< <(inotifywait -m -r \
    -e close_write -e moved_to -e create -e delete -e modify \
    --format '%e %w%f' "${MP3SYNC_INPUT}")

while true; do
    # Block indefinitely until at least one change happens.
    if ! read -r -u 3 event; then
        echo "inotifywait monitor pipe closed unexpectedly; exiting so systemd restarts us." >&2
        exit 1
    fi

    # Debounce: keep draining events until nothing new arrives within the quiet window.
    while read -r -u 3 -t "${MP3SYNC_QUIET_SECONDS}" event; do
        :
    done

    echo "Library appears settled - starting mp3sync run."
    "${MP3SYNC_BIN}" "${MP3SYNC_INPUT}" "${MP3SYNC_OUTPUT}" "${MP3SYNC_BITRATE}" "${MP3SYNC_THREADS}"
    echo "mp3sync run finished with exit code $?."
done