#!/bin/bash

WWWPATH="/var/www/html"
MINCACHE="/opt/www-mincache" 
WWW_NAME="www-data"
WWW_GROUP="www-data"

## Minify JS/CSS (only files whose source changed since the last deploy)
##
## The cache mirrors the site layout twice:
##   $MINCACHE/src/<path>  the file exactly as it arrived last deploy
##   $MINCACHE/min/<path>  the minified output produced from it
## Changed  = new file differs from its src/ copy
## Removed  = a src/ entry with nothing at the matching path in $WWWPATH
MIN_SRCDIR="$MINCACHE/src"
MIN_OUTDIR="$MINCACHE/min"
mkdir -p "$MINCACHE"
mkdir -p "$MIN_SRCDIR" "$MIN_OUTDIR"
MIN_BEFORE_TOTAL=0
MIN_AFTER_TOTAL=0
MIN_RUN=0
MIN_REUSED=0
MIN_REMOVED=0
MIN_RULE='--------------------------------------------------------------------'

echo "Compressing website components."
printf '%-26s %10s %10s %10s %8s\n' 'FILE' 'BEFORE KB' 'AFTER KB' 'SAVED KB' 'STATUS'
printf -- '%s\n' "$MIN_RULE"

for MINFILE in "$WWWPATH"/styles/*.css "$WWWPATH"/*.js; do
  [ -e "$MINFILE" ] || continue
  case "$MINFILE" in *.min.js|*.min.css) continue ;; esac

  MIN_NAME="${MINFILE#"$WWWPATH"/}"
  MIN_SRC="$MIN_SRCDIR/$MIN_NAME"
  MIN_OUT="$MIN_OUTDIR/$MIN_NAME"
  MIN_SIZE_BEFORE=$(wc -c < "$MINFILE")

  if cmp -s "$MINFILE" "$MIN_SRC" && [ -f "$MIN_OUT" ]; then
    # Byte-identical to the last deploy - reuse the stored output.
    cp -f "$MIN_OUT" "$MINFILE"
    chmod 644 "$MINFILE"
    MIN_SIZE_AFTER=$(wc -c < "$MINFILE")
    MIN_REUSED=$(( MIN_REUSED + 1 ))
    awk -v f="$MIN_NAME" -v b="$MIN_SIZE_BEFORE" -v a="$MIN_SIZE_AFTER" 'BEGIN {
      s = (b - a) / 1024
      if (s > -0.05 && s < 0.05) s = 0
      printf "%-26s %10.1f %10.1f %10.1f %8s\n", f, b/1024, a/1024, s, "cached" }'
    MIN_BEFORE_TOTAL=$(( MIN_BEFORE_TOTAL + MIN_SIZE_BEFORE ))
    MIN_AFTER_TOTAL=$(( MIN_AFTER_TOTAL + MIN_SIZE_AFTER ))
    continue
  fi

  # Back up the pristine source before the compressor overwrites it.
  MIN_RUN=$(( MIN_RUN + 1 ))
  mkdir -p "${MIN_SRC%/*}" "${MIN_OUT%/*}"
  cp -f "$MINFILE" "$MIN_SRC"

  case "$MINFILE" in
    *.css) yui-compressor "$MINFILE" > "$MINFILE.tmp" ;;
    *)     uglifyjs --warn --compress --output "$MINFILE.tmp" -- "$MINFILE" ;;
  esac
  MIN_RC=$?

  if [ "$MIN_RC" -eq 0 ] \
     && [ -s "$MINFILE.tmp" ] \
     && [ "$(wc -c < "$MINFILE.tmp")" -lt "$MIN_SIZE_BEFORE" ]; then
    mv -f "$MINFILE.tmp" "$MINFILE"
    chmod 644 "$MINFILE"
    MIN_SIZE_AFTER=$(wc -c < "$MINFILE")
    cp -f "$MINFILE" "$MIN_OUT"
    awk -v f="$MIN_NAME" -v b="$MIN_SIZE_BEFORE" -v a="$MIN_SIZE_AFTER" 'BEGIN {
      s = (b - a) / 1024
      if (s > -0.05 && s < 0.05) s = 0
      printf "%-26s %10.1f %10.1f %10.1f %8s\n", f, b/1024, a/1024, s, "minified" }'
  else
    if [ "$MIN_RC" -eq 0 ] && [ -s "$MINFILE.tmp" ]; then
      MIN_NOTE='no change'
      # Compressor ran fine, output just wasn't smaller - store the original
      # as its own output so the next deploy skips it too.
      cp -f "$MINFILE" "$MIN_OUT"
    else
      MIN_NOTE='failed'
      # No output stored, so the next deploy retries and reports it again.
      # The src/ copy stays, so it is not mistaken for a removed file.
      rm -f "$MIN_OUT"
    fi
    rm -f "$MINFILE.tmp"
    MIN_SIZE_AFTER=$MIN_SIZE_BEFORE
    awk -v f="$MIN_NAME" -v b="$MIN_SIZE_BEFORE" -v n="$MIN_NOTE" 'BEGIN {
      printf "%-26s %10.1f %10.1f %10s %8s\n", f, b/1024, b/1024, "-", n }'
  fi

  MIN_BEFORE_TOTAL=$(( MIN_BEFORE_TOTAL + MIN_SIZE_BEFORE ))
  MIN_AFTER_TOTAL=$(( MIN_AFTER_TOTAL + MIN_SIZE_AFTER ))
done

# Anything still in the backup but no longer in the deploy has been removed:
# drop its stored source and minified copy, and report the size that went away.
for MIN_OLD in "$MIN_SRCDIR"/styles/*.css "$MIN_SRCDIR"/*.js; do
  [ -e "$MIN_OLD" ] || continue
  MIN_NAME="${MIN_OLD#"$MIN_SRCDIR"/}"
  [ -e "$WWWPATH/$MIN_NAME" ] && continue
  MIN_SIZE_BEFORE=$(wc -c < "$MIN_OLD")
  rm -f "$MIN_OLD" "$MIN_OUTDIR/$MIN_NAME"
  MIN_REMOVED=$(( MIN_REMOVED + 1 ))
  awk -v f="$MIN_NAME" -v b="$MIN_SIZE_BEFORE" 'BEGIN {
    printf "%-26s %10.1f %10s %10s %8s\n", f, b/1024, "-", "-", "removed" }'
done

printf -- '%s\n' "$MIN_RULE"
awk -v b="$MIN_BEFORE_TOTAL" -v a="$MIN_AFTER_TOTAL" -v r="$MIN_RUN" -v c="$MIN_REUSED" -v d="$MIN_REMOVED" 'BEGIN {
  pct = (b > 0) ? (b - a) * 100 / b : 0
  s = (b - a) / 1024
  if (s > -0.05 && s < 0.05) s = 0
  printf "%-26s %10.1f %10.1f %10.1f   (%.1f%% smaller)\n", \
    "TOTAL", b/1024, a/1024, s, pct
  printf "%d changed file(s) compressed, %d reused from cache, %d removed\n", r, c, d }'

chown -R "$WWW_NAME":"$WWW_GROUP" $WWWPATH/styles/*.css
chown -R "$WWW_NAME":"$WWW_GROUP" $WWWPATH/*.js