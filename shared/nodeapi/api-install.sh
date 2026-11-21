#!/bin/bash

## Install the node API from a staged folder and report what changed
##   bash api-install.sh /tmp/config/nodeapi /opt/nodeapi
##
## Every new or changed file in the staged folder is copied into the installed one, and an
## installed file the staged folder no longer has is deleted, so the installed API always
## matches the deploy. Left alone in the installed folder: node_modules, dotfiles
## (.deps.hash; macOS ._* metadata is not deployed either) and package-lock.json, which
## npm install makes there. Prints a table of the new, changed and deleted files, in the
## style of www-minify.sh.

stagedDir="$1"
installDir="$2"
reportRule='--------------------------------------------------------------------'
reportRows=''
totalBefore=0
totalAfter=0
newCount=0
changedCount=0
deletedCount=0
unchangedCount=0

## Lists the API's files under a folder, one relative path per line, sorted:
##   API_FILE_LIST /opt/nodeapi  ->  controller.js, server.js, ...
API_FILE_LIST() {
  local listDir="$1"
  find "$listDir" -path "$listDir/node_modules" -prune -o -type f ! -name '.*' -printf '%P\n' | sort
}

## Prints one row of the report, sizes given in bytes or "-" for none:
##   API_REPORT_ROW controller.js 42188 42905 changed
##   ->  controller.js                    41.2       41.9       +0.7  changed
API_REPORT_ROW() {
  awk -v fileName="$1" -v beforeBytes="$2" -v afterBytes="$3" -v fileStatus="$4" 'BEGIN {
    beforeText = "-"
    afterText = "-"
    changeText = "-"
    if (beforeBytes != "-") beforeText = sprintf("%.1f", beforeBytes / 1024)
    if (afterBytes != "-") afterText = sprintf("%.1f", afterBytes / 1024)
    if (beforeBytes != "-" && afterBytes != "-") {
      changeBytes = afterBytes - beforeBytes
      changeKb = changeBytes / 1024
      if (changeKb > -0.05 && changeKb < 0.05) changeKb = 0
      changeText = sprintf("%+.1f", changeKb)
    }
    printf "%-26s %10s %10s %10s %8s\n", fileName, beforeText, afterText, changeText, fileStatus }'
}

## Copies one staged file into the installed folder, making its folder if needed:
##   API_COPY_FILE controller.js  ->  0 when copied
API_COPY_FILE() {
  local relPath="$1"
  local targetFile="$installDir/$relPath"
  mkdir -p "${targetFile%/*}"
  cp -f "$stagedDir/$relPath" "$targetFile"
}

## Check the folders
## A staged folder without server.js is a failed or empty deploy: installing it would delete
## the whole API, so nothing is touched.
if [ -z "$stagedDir" ] || [ -z "$installDir" ] || [ "$installDir" = "/" ]; then
  echo "Usage: api-install.sh <staged folder> <install folder>"
  exit 1
fi
if [ ! -f "$stagedDir/server.js" ]; then
  echo "No node API in $stagedDir, $installDir left as it is."
  exit 1
fi
mkdir -p "$installDir"

echo "Installing node API files."

## New and changed files
while IFS= read -r relPath; do
  stagedFile="$stagedDir/$relPath"
  installedFile="$installDir/$relPath"
  sizeAfter=$(wc -c < "$stagedFile")
  if [ ! -f "$installedFile" ]; then
    sizeBefore=0
    fileStatus="new"
    rowBefore="-"
  elif cmp -s "$stagedFile" "$installedFile"; then
    sizeBefore=$sizeAfter
    unchangedCount=$(( unchangedCount + 1 ))
    totalBefore=$(( totalBefore + sizeBefore ))
    totalAfter=$(( totalAfter + sizeAfter ))
    continue
  else
    sizeBefore=$(wc -c < "$installedFile")
    fileStatus="changed"
    rowBefore=$sizeBefore
  fi
  if API_COPY_FILE "$relPath"; then
    if [ "$fileStatus" = "new" ]; then
      newCount=$(( newCount + 1 ))
    else
      changedCount=$(( changedCount + 1 ))
    fi
  else
    # the installed file (if any) stays as it was
    fileStatus="failed"
    if [ -f "$installedFile" ]; then
      sizeAfter=$(wc -c < "$installedFile")
    else
      sizeAfter=0
    fi
  fi
  reportRows+=$(API_REPORT_ROW "$relPath" "$rowBefore" "$sizeAfter" "$fileStatus")$'\n'
  totalBefore=$(( totalBefore + sizeBefore ))
  totalAfter=$(( totalAfter + sizeAfter ))
done < <(API_FILE_LIST "$stagedDir")

## Files no longer deployed
while IFS= read -r relPath; do
  [ "$relPath" = "package-lock.json" ] && continue
  [ -e "$stagedDir/$relPath" ] && continue
  installedFile="$installDir/$relPath"
  sizeBefore=$(wc -c < "$installedFile")
  totalBefore=$(( totalBefore + sizeBefore ))
  if rm -f "$installedFile"; then
    deletedCount=$(( deletedCount + 1 ))
    reportRows+=$(API_REPORT_ROW "$relPath" "$sizeBefore" "-" "deleted")$'\n'
  else
    totalAfter=$(( totalAfter + sizeBefore ))
    reportRows+=$(API_REPORT_ROW "$relPath" "$sizeBefore" "$sizeBefore" "failed")$'\n'
  fi
done < <(API_FILE_LIST "$installDir")

## Report
if [ -z "$reportRows" ]; then
  echo "No changes in node API files."
  exit 0
fi
printf '%-26s %10s %10s %10s %8s\n' 'FILE' 'BEFORE KB' 'AFTER KB' 'CHANGE KB' 'STATUS'
printf -- '%s\n' "$reportRule"
printf '%s' "$reportRows"
printf -- '%s\n' "$reportRule"
API_REPORT_ROW "TOTAL" "$totalBefore" "$totalAfter" ""
printf '%d changed, %d new, %d deleted, %d unchanged\n' \
  "$changedCount" "$newCount" "$deletedCount" "$unchangedCount"
exit 0
