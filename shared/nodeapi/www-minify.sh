#!/bin/bash

WWWPATH="/var/www/html"
MINCACHE="/opt/www-mincache" 
WWW_NAME="www-data"
WWW_GROUP="www-data"

## Minify JS/CSS (only files whose source changed since the last deploy)
mkdir -p "$MINCACHE"
MIN_KEEP=$(mktemp)
MIN_BEFORE_TOTAL=0
MIN_AFTER_TOTAL=0
MIN_RUN=0
MIN_REUSED=0
MIN_RULE='--------------------------------------------------------------------'

echo "Compressing website components."
printf '%-26s %10s %10s %10s %8s\n' 'FILE' 'BEFORE KB' 'AFTER KB' 'SAVED KB' 'STATUS'
printf -- '%s\n' "$MIN_RULE"

for MINFILE in "$WWWPATH"/styles/*.css "$WWWPATH"/*.js; do
  [ -e "$MINFILE" ] || continue
  case "$MINFILE" in *.min.js|*.min.css) continue ;; esac

  MIN_NAME="${MINFILE#"$WWWPATH"/}"
  MIN_SIZE_BEFORE=$(wc -c < "$MINFILE")
  MIN_SUM=$(sha1sum < "$MINFILE" | cut -d' ' -f1)
  case "$MINFILE" in
    *.css) MIN_EXT=css ;;
    *)     MIN_EXT=js  ;;
  esac
  MIN_CACHED="$MINCACHE/$MIN_SUM.$MIN_EXT"
  echo "$MIN_SUM.$MIN_EXT" >> "$MIN_KEEP"

  if [ -f "$MIN_CACHED" ]; then
    # Source is byte-identical to the last deploy - reuse the stored output.
    cp -f "$MIN_CACHED" "$MINFILE"
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

  MIN_RUN=$(( MIN_RUN + 1 ))
  if [ "$MIN_EXT" = css ]; then
    yui-compressor "$MINFILE" > "$MINFILE.tmp"
  else
    uglifyjs --warn --compress --output "$MINFILE.tmp" -- "$MINFILE"
  fi
  MIN_RC=$?

  if [ "$MIN_RC" -eq 0 ] \
     && [ -s "$MINFILE.tmp" ] \
     && [ "$(wc -c < "$MINFILE.tmp")" -lt "$MIN_SIZE_BEFORE" ]; then
    mv -f "$MINFILE.tmp" "$MINFILE"
    chmod 644 "$MINFILE"
    MIN_SIZE_AFTER=$(wc -c < "$MINFILE")
    cp -f "$MINFILE" "$MIN_CACHED"
    awk -v f="$MIN_NAME" -v b="$MIN_SIZE_BEFORE" -v a="$MIN_SIZE_AFTER" 'BEGIN {
      s = (b - a) / 1024
      if (s > -0.05 && s < 0.05) s = 0
      printf "%-26s %10.1f %10.1f %10.1f %8s\n", f, b/1024, a/1024, s, "minified" }'
  else
    if [ "$MIN_RC" -eq 0 ] && [ -s "$MINFILE.tmp" ]; then
      MIN_NOTE='no change'
      # Compressor ran fine, output just wasn't smaller - cache the original
      # so the next deploy skips it too.
      cp -f "$MINFILE" "$MIN_CACHED"
    else
      MIN_NOTE='failed'
      # Not cached, so a failure is retried (and reported) on every deploy.
      sed -i "\|^$MIN_SUM\.$MIN_EXT\$|d" "$MIN_KEEP"
    fi
    rm -f "$MINFILE.tmp"
    MIN_SIZE_AFTER=$MIN_SIZE_BEFORE
    awk -v f="$MIN_NAME" -v b="$MIN_SIZE_BEFORE" -v n="$MIN_NOTE" 'BEGIN {
      printf "%-26s %10.1f %10.1f %10s %8s\n", f, b/1024, b/1024, "-", n }'
  fi

  MIN_BEFORE_TOTAL=$(( MIN_BEFORE_TOTAL + MIN_SIZE_BEFORE ))
  MIN_AFTER_TOTAL=$(( MIN_AFTER_TOTAL + MIN_SIZE_AFTER ))
done

# Drop cache entries for files that no longer exist or have changed.
for MIN_OLD in "$MINCACHE"/*; do
  [ -e "$MIN_OLD" ] || continue
  grep -qxF "${MIN_OLD##*/}" "$MIN_KEEP" || rm -f "$MIN_OLD"
done
rm -f "$MIN_KEEP"

printf -- '%s\n' "$MIN_RULE"
awk -v b="$MIN_BEFORE_TOTAL" -v a="$MIN_AFTER_TOTAL" -v r="$MIN_RUN" -v c="$MIN_REUSED" 'BEGIN {
  pct = (b > 0) ? (b - a) * 100 / b : 0
  s = (b - a) / 1024
  if (s > -0.05 && s < 0.05) s = 0
  printf "%-26s %10.1f %10.1f %10.1f   (%.1f%% smaller)\n", \
    "TOTAL", b/1024, a/1024, s, pct
  printf "%d changed file(s) compressed, %d reused from cache\n", r, c }'

chown -R "$WWW_NAME":"$WWW_GROUP" $WWWPATH/styles/*.css
chown -R "$WWW_NAME":"$WWW_GROUP" $WWWPATH/*.js

