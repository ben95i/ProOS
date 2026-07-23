#!/bin/bash
#
# Watches RAMPATH for .txt trigger files and dispatches apicmds.sh.
#
#  - events are read sequentially from a single inotifywait stream
#  - each command is spawned in parallel (background job)
#  - every job appends to the shared LOGPATH in O_APPEND mode
#  - each trigger file is claimed atomically, so it fires exactly once
#  - data files listed in IGNORE are never consumed or deleted
#

RAMPATH="/mnt/ramdisk"
LOGPATH="$RAMPATH/sysout.txt"
SPOOL="$RAMPATH/spool"           # claimed trigger files live here
APICMDS="/usr/bin/apicmds.sh"
MAXJOBS=4                        # safety valve against a flood of triggers

## ---------------------------------------------------------------
## ignore list
## ---------------------------------------------------------------
## Data files that live in the ramdisk but are NOT commands. These are
## matched against the whole filename; shell globs are allowed. Ignored
## files are left untouched -- they are never claimed, run, or deleted.
IGNORE=(
    "sysout.txt"        # the shared log itself
    "drives.txt"        # read by backupstd/backupchk
    "pwd.txt"           # ZFS load-key passphrase
    "fusearch.txt"      # gocryptfs passfile
    "bookmarks.txt"     # bookmarks bind link
    "*.lock"            # backupsvr.lock / pvebackup.lock
    "*.tmp"
)

is_ignored() {
    local name="$1" pattern
    for pattern in "${IGNORE[@]}"; do
        # unquoted $pattern on the right so globs are honoured
        [[ "$name" == $pattern ]] && return 0
    done
    return 1
}

## ---------------------------------------------------------------
## log permissions
## ---------------------------------------------------------------
LOGOWNER="tank:shared"
LOGMODE="0666"

## Mode lives on the inode, so truncate/append preserve it -- it only
## resets when something RECREATES the file. This re-asserts it, and is
## called on every dispatch so the window is milliseconds wide.
ensure_log() {
    [ -e "$LOGPATH" ] || : >> "$LOGPATH"
    chown "$LOGOWNER" "$LOGPATH" 2>/dev/null
    chmod "$LOGMODE"  "$LOGPATH" 2>/dev/null
}

## Continuous enforcement. The ramdisk is bind-mounted into the LXC by
## lxc-mp.sh, so processes inside the container can chmod/recreate the
## log at any time -- not just when a command is dispatched. This watches
## for attribute changes and repairs them immediately.
##
## The mode is compared BEFORE chmod: chmod always emits IN_ATTRIB even
## when the mode is unchanged, so an unconditional repair would loop.
enforce_log_perms() {
    local mode owner
    while true; do
        mode=$(stat -c '%a'     "$LOGPATH" 2>/dev/null)
        owner=$(stat -c '%U:%G' "$LOGPATH" 2>/dev/null)
        if [ "$mode" != "${LOGMODE#0}" ] || [ "$owner" != "$LOGOWNER" ]; then
            ensure_log
        fi
        ## blocks until someone touches the inode; returns if it vanishes
        inotifywait -q -e attrib -e delete_self -e move_self \
            "$LOGPATH" >/dev/null 2>&1 || sleep 2
        [ -e "$LOGPATH" ] || ensure_log
    done
}

## ---------------------------------------------------------------
## setup
## ---------------------------------------------------------------
## 0000, not 0002: 0002 masks the other-write bit, so any process in
## this tree that RECREATES the log gets 0664 instead of 0666.
umask 0000
mkdir -p "$SPOOL"
ensure_log

## clean up any triggers left over from an unclean shutdown.
## Locks too: KillMode=control-group means no job survives a service
## stop, so any lock present at startup is by definition stale.
rm -f "$SPOOL"/* "$RAMPATH"/*.lock 2>/dev/null

## on service stop, take the whole process group down with us
shutdown() {
    trap - TERM INT
    kill -- -$$ 2>/dev/null
    exit 0
}
trap shutdown TERM INT

## permission watchdog (killed with the process group on stop)
enforce_log_perms &

## ---------------------------------------------------------------
## dispatcher
## ---------------------------------------------------------------
dispatch() {
    local command="$1" claimed="$2"

    ensure_log

    # O_APPEND: every write is positioned at EOF by the kernel, so
    # parallel jobs cannot overwrite each other's output, and
    # 'clearlog' truncating the file does not strand the writers.
    # stdbuf -oL keeps whole lines together.
    stdbuf -oL -eL "$APICMDS" "$command" "$RAMPATH" >> "$LOGPATH" 2>&1

    rm -f "$claimed"
}

## ---------------------------------------------------------------
## monitor
## ---------------------------------------------------------------
# close_write  -> file is fully written and closed (never a partial read)
# moved_to     -> file dropped in via mv/rename
# NOTE: 'create' is deliberately NOT watched. create + close_write both
# fire for a normal write and would trigger the same file twice.
## The event stream is the watcher PLUS a one-time catch-up scan.
## inotify only reports events that happen while watching, so any
## trigger dropped while the service was down (boot, RestartSec
## window) would otherwise sit in the ramdisk forever. The scan runs
## AFTER the watch is established; if the scan and a live event both
## report the same file, the atomic mv -n claim below de-duplicates.
{
    inotifywait -m -q --format '%f' \
        -e close_write -e moved_to "$RAMPATH" &
    WATCHER=$!
    sleep 1     # give inotifywait time to establish its watch
    for _EXISTING in "$RAMPATH"/*.txt; do
        [ -e "$_EXISTING" ] && printf '%s\n' "${_EXISTING##*/}"
    done
    wait "$WATCHER"
} |
while IFS= read -r FILENAME; do

    ## only act on .txt triggers, skip dotfiles/editor temp files
    ## NOTE: the dotfile pattern MUST come before *.txt -- case takes
    ## the first match, so '.hidden.txt' would otherwise dispatch.
    case "$FILENAME" in
        .*|*~) continue ;;
        *.txt) ;;
            *) continue ;;
    esac

    ## ---- ignore list ------------------------------------------
    ## checked BEFORE the claim, so ignored files stay where they are
    if is_ignored "$FILENAME"; then
        continue
    fi

    COMMAND="${FILENAME%.*}"
    [ -n "$COMMAND" ] || continue

    ## ---- atomic claim -----------------------------------------
    ## mv within the same filesystem is atomic. If it succeeds we own
    ## this event; if it fails the file is already gone (duplicate
    ## event, or someone else removed it) and we skip it.
    ## This both de-duplicates and removes the trigger in one step.
    CLAIM="$SPOOL/$$.$(date +%s%N).$FILENAME"
    mv -n -- "$RAMPATH/$FILENAME" "$CLAIM" 2>/dev/null || continue
    [ -e "$CLAIM" ] || continue

    ## ---- throttle ---------------------------------------------
    while [ "$(jobs -rp | wc -l)" -ge "$MAXJOBS" ]; do
        wait -n 2>/dev/null || sleep 0.2
    done

    ## ---- run detached, in parallel ----------------------------
    dispatch "$COMMAND" "$CLAIM" &

done

## Reaching this point means the inotifywait stream ended, which is
## always abnormal for a monitor. Exit nonzero so systemd restarts us
## (belt-and-braces alongside Restart=always in the unit).
exit 1