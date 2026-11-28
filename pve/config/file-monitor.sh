#!/bin/bash
#
# Watches ramPath for .txt trigger files and dispatches apicmds.sh.
#
#  - events are read sequentially from a single inotifywait stream
#  - each command is spawned in parallel (background job)
#  - each command's output goes to its own file, picked by outputRoutes:
#    <command>.out.save.txt unless the command has a row there
#  - a file is emptied before each run (replace) unless its row has the
#    append flag; commands sharing a replace file run one at a time
#  - each trigger file is claimed atomically, so it fires exactly once
#  - data files matching ignorePatterns are never consumed or deleted
#

ramPath="/mnt/ramdisk"
spoolPath="$ramPath/spool"       # claimed trigger files live here
apiCmdsPath="/usr/bin/apicmds.sh"
maxJobs=4                        # safety valve against a flood of triggers

## ---------------------------------------------------------------
## ignore list
## ---------------------------------------------------------------
## Data files that live in the ramdisk but are NOT commands. These are
## matched against the whole filename; shell globs are allowed. Ignored
## files are left untouched -- they are never claimed, run, or deleted.
## A new data file is named <name>.save.txt so it needs no entry here.
ignorePatterns=(
    "*.save.txt"        # data files: saved state, every command's output
                        # (<command>.out.save.txt, regions.out.save.txt), the
                        # backup wizard's bkpstatus / bkplog, the bkppwd and
                        # fusearch passfiles
    "*.lock"            # bkpjob.lock / pvebackup.lock
    "*.tmp"
)

## Answers 0 when a file name matches ignorePatterns.
## Usage: IS_IGNORED <file name>   e.g. IS_IGNORED bookmarks.save.txt -> 0
IS_IGNORED() {
    local fileName="$1"
    local ignorePattern
    for ignorePattern in "${ignorePatterns[@]}"; do
        # unquoted on the right so globs are honoured
        if [[ "$fileName" == $ignorePattern ]]; then
            return 0
        fi
    done
    return 1
}

## ---------------------------------------------------------------
## output routing
## ---------------------------------------------------------------
## Every command's output (stdout and stderr) goes to a file of its own
## in the ramdisk, which the website reads through
## /api/read?file=<file name without .txt>. A command with no row below
## goes to <command>.out.save.txt (startxana -> startxana.out.save.txt);
## the .out.save.txt ending keeps it apart from every data file, and
## *.save.txt keeps it from being taken for a trigger.
##
## Rows: "<command glob>|<file name>|<mode>", the first match wins.
##   replace  the file is emptied before the command runs, so it holds
##            only the newest run; commands sharing the file wait for
##            each other and run one at a time (the default)
##   append   the output is added to the end, with no emptying and no
##            waiting
## In replace mode a run's output sits between two marker lines, by which
## the website's /api/svrcmd (shared/nodeapi/svrcmd.js) knows the run it
## asked for has begun and ended:
##   @START|<command>|<epoch ms>
##   @END|<command>|<exit code>|<epoch ms>
## The website's Server window (shared/html/server.js, SERVER_COMMANDS)
## names the same shared files for the commands below.
outputRoutes=(
    ## every region attach and detach shares one file, one at a time
    "unmnt_all|regions.out.save.txt|replace"
    "mnt_*_region|regions.out.save.txt|replace"
    "unmnt_*_region|regions.out.save.txt|replace"
    ## the backup wizard: each job begins its log afresh itself
    ## (tank-backup.sh), and a refusal or a cancel must reach the running
    ## job's log at once, so it is never emptied or waited on here
    "bkp_*|bkplog.save.txt|append"
)

## Prints the output file name and mode for a command: its row in
## outputRoutes, or <command>.out.save.txt with replace.
## Usage: ROUTE_OUTPUT <command>
##   e.g. ROUTE_OUTPUT startxana       -> startxana.out.save.txt|replace
##        ROUTE_OUTPUT mnt_ram_region  -> regions.out.save.txt|replace
ROUTE_OUTPUT() {
    local commandName="$1"
    local outputRoute
    local routePattern
    local routeFile
    local routeMode
    for outputRoute in "${outputRoutes[@]}"; do
        IFS='|' read -r routePattern routeFile routeMode <<< "$outputRoute"
        # unquoted on the right so globs are honoured
        if [[ "$commandName" == $routePattern ]]; then
            printf '%s|%s\n' "$routeFile" "$routeMode"
            return 0
        fi
    done
    printf '%s|%s\n' "$commandName.out.save.txt" "replace"
}

## ---------------------------------------------------------------
## ownership / permissions
## ---------------------------------------------------------------
## The LXC that shares this ramdisk now runs as tank:shared too, so
## group access is sufficient for every writer and no continuous
## enforcement is needed.
##
## Group ownership is handled by the mount itself: fstab mounts the
## tmpfs uid=tank,gid=shared,mode=2775, and the setgid bit means every
## file and directory created under it inherits gid=shared regardless
## of who creates it.
##
## The owner is not inherited -- a new file always belongs to the uid
## that created it, and everything here creates it as root. So both
## the owner and the mode still have to be asserted; only the group
## comes for free.
outputOwner="tank:shared"
outputMode="0644"

## Creates an output file when it is missing and sets its owner and mode.
## Mode lives on the inode, so emptying and appending keep it.
## Usage: ENSURE_OUTPUT <path>   e.g. ENSURE_OUTPUT /mnt/ramdisk/startxana.out.save.txt
ENSURE_OUTPUT() {
    local outputPath="$1"
    : >> "$outputPath"                          # create if missing, else no-op
    chown "$outputOwner" "$outputPath" 2>/dev/null
    chmod "$outputMode"  "$outputPath" 2>/dev/null
}

## ---------------------------------------------------------------
## setup
## ---------------------------------------------------------------
## 0002 keeps group write on anything root creates in the tree; the
## other-write bit is no longer needed now that the container shares
## the tank:shared identity.
umask 0002
## Fallback only: the fstab entry already sets this
## at mount time. It matters if the tmpfs ever comes up with the old
## uid=0 options -- one syscall, and the setgid bit does the rest.
chown "$outputOwner" "$ramPath" 2>/dev/null
chmod 2775           "$ramPath" 2>/dev/null
mkdir -p "$spoolPath"            # inherits gid=shared + setgid from ramPath

## clean up any triggers left over from an unclean shutdown.
## Locks too: KillMode=control-group means no job survives a service
## stop, so any lock present at startup is by definition stale, and so
## are the backup job's worker PID and cancel flag. A backup passphrase
## no job read is shredded.
rm -f "$spoolPath"/* "$ramPath"/*.lock "$ramPath/bkpworker.pid" "$ramPath/bkpcancel.flag" 2>/dev/null
if [ -e "$ramPath/bkppwd.save.txt" ]; then
    shred -n 2 -z -u "$ramPath/bkppwd.save.txt" 2>/dev/null || rm -f "$ramPath/bkppwd.save.txt"
fi

## On service stop, takes the whole process group down with us.
## Usage: trap SHUTDOWN TERM INT
SHUTDOWN() {
    trap - TERM INT
    kill -- -$$ 2>/dev/null
    exit 0
}
trap SHUTDOWN TERM INT

## ---------------------------------------------------------------
## dispatcher
## ---------------------------------------------------------------
## Runs one claimed command into its output file (ROUTE_OUTPUT), then
## deletes the claim. Run in the background, one per trigger.
## Usage: DISPATCH <command> <claimed trigger path>
##   e.g. DISPATCH startxana /mnt/ramdisk/spool/812.1764340000.startxana.txt
DISPATCH() {
    local commandName="$1"
    local claimPath="$2"
    local outputRoute
    local outputFile
    local routeMode
    local outputPath
    local lockFd
    local startTime
    local commandCode
    local endTime

    outputRoute=$(ROUTE_OUTPUT "$commandName")
    IFS='|' read -r outputFile routeMode <<< "$outputRoute"
    outputPath="$ramPath/$outputFile"
    ENSURE_OUTPUT "$outputPath"

    # O_APPEND: every write is positioned at EOF by the kernel, so
    # parallel append commands cannot overwrite each other's output, and
    # emptying the file does not strand a writer still holding it.
    # stdbuf -oL keeps whole lines together.
    if [ "$routeMode" == "append" ]; then
        stdbuf -oL -eL "$apiCmdsPath" "$commandName" "$ramPath" >> "$outputPath" 2>&1
    else
        ## Holds a lock on the output file while the command runs, so a
        ## second command for the same file waits instead of emptying it
        ## mid-run. The command gets the lock's descriptor closed
        ## ({lockFd}>&-): a VM, container or FUSE mount it starts keeps
        ## running after it, and must not keep holding the lock.
        exec {lockFd}>>"$outputPath"
        flock "$lockFd"
        : > "$outputPath"
        startTime=$(date +%s%3N)
        printf '@START|%s|%s\n' "$commandName" "$startTime" >> "$outputPath"
        stdbuf -oL -eL "$apiCmdsPath" "$commandName" "$ramPath" >> "$outputPath" 2>&1 {lockFd}>&-
        commandCode=$?
        endTime=$(date +%s%3N)
        printf '@END|%s|%s|%s\n' "$commandName" "$commandCode" "$endTime" >> "$outputPath"
        exec {lockFd}>&-
    fi

    rm -f "$claimPath"
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
        -e close_write -e moved_to "$ramPath" &
    watcherPid=$!
    sleep 1     # give inotifywait time to establish its watch
    for existingPath in "$ramPath"/*.txt; do
        if [ -e "$existingPath" ]; then
            printf '%s\n' "${existingPath##*/}"
        fi
    done
    wait "$watcherPid"
} |
while IFS= read -r fileName; do

    ## only act on .txt triggers, skip dotfiles/editor temp files
    ## NOTE: the dotfile pattern MUST come before *.txt -- case takes
    ## the first match, so '.hidden.txt' would otherwise dispatch.
    case "$fileName" in
        .*|*~) continue ;;
        *.txt) ;;
            *) continue ;;
    esac

    ## ---- ignore list ------------------------------------------
    ## checked BEFORE the claim, so ignored files stay where they are
    if IS_IGNORED "$fileName"; then
        continue
    fi

    commandName="${fileName%.*}"
    if [ -z "$commandName" ]; then
        continue
    fi

    ## ---- atomic claim -----------------------------------------
    ## mv within the same filesystem is atomic. If it succeeds we own
    ## this event; if it fails the file is already gone (duplicate
    ## event, or someone else removed it) and we skip it.
    ## This both de-duplicates and removes the trigger in one step.
    claimPath="$spoolPath/$$.$(date +%s%N).$fileName"
    mv -n -- "$ramPath/$fileName" "$claimPath" 2>/dev/null || continue
    if [ ! -e "$claimPath" ]; then
        continue
    fi

    ## ---- throttle ---------------------------------------------
    while [ "$(jobs -rp | wc -l)" -ge "$maxJobs" ]; do
        wait -n 2>/dev/null || sleep 0.2
    done

    ## ---- run detached, in parallel ----------------------------
    DISPATCH "$commandName" "$claimPath" &

done

## Reaching this point means the inotifywait stream ended, which is
## always abnormal for a monitor. Exit nonzero so systemd restarts us
## (belt-and-braces alongside Restart=always in the unit).
exit 1
