#!/usr/bin/env bash
# mkv2mp4.sh - batch convert H.265 MKV files to H.264 720p MP4
#   - all audio tracks converted to stereo AAC
#   - 5.1 tracks downmixed with boosted dialogue (centre channel)
#   - every audio track measured and amplified as far as possible without clipping
#   - English audio placed 1st (default), Japanese 2nd, any others after
#   - subtitles are not included
#
# Usage:
#   ./mkv2mp4.sh                      # pick input/output folders with a dialog (needs zenity)
#   ./mkv2mp4.sh /path/in /path/out   # or pass them directly
#
# Requires: ffmpeg (with libx264) and ffprobe. Optional: zenity for folder pickers.

set -euo pipefail
shopt -s nullglob nocaseglob

# ---- Settings you may want to tweak ----
CRF=21              # quality: lower = better/bigger (18-23 is the usual range)
PRESET=medium       # slower = smaller file at same quality (fast, medium, slow, slower)
TUNE=animation      # animation for anime/cartoons, film for live action, empty for none
AUDIO_BITRATE=192k  # per stereo track
DIALOG_BOOST=1      # 1 = dialogue-boost downmix for 5.1 tracks, 0 = ffmpeg's standard downmix
SIDE_MIX=0.30       # how much of the front L/R and surround channels to mix in with the
                    # centre (dialogue) channel; lower = louder dialogue, less music/effects

NORMALIZE=loudness  # peak     = raise each track until its loudest peak reaches PEAK_DB
                    #            (as loud as possible without clipping, dynamics untouched)
                    # loudness = make every track the same perceived loudness (EBU R128),
                    #            with peaks kept below LOUD_TP so nothing clips
                    # none     = leave levels as they are
PEAK_DB=-1.0        # peak mode: target for the loudest sample, in dBFS. -1 leaves a little
                    # headroom because AAC encoding can push peaks slightly higher.
LOUD_I=-16          # loudness mode: target loudness in LUFS (-16 is typical for streaming)
LOUD_TP=-1.5        # loudness mode: maximum true peak in dBTP
LOUD_LRA=20         # loudness mode: allowed loudness range; high value keeps the dynamics
# ----------------------------------------

# 5.1 channel order in ffmpeg: c0=FL c1=FR c2=FC (dialogue) c3=LFE c4=SL/BL c5=SR/BR
# '<' normalises the gains so the mix can't clip.
BOOST_PAN="pan=stereo|c0<c2+${SIDE_MIX}*c0+${SIDE_MIX}*c4|c1<c2+${SIDE_MIX}*c1+${SIDE_MIX}*c5"

pick_dir() {
    if command -v zenity >/dev/null 2>&1; then
        zenity --file-selection --directory --title="$1" 2>/dev/null
    else
        echo "No folder given and zenity isn't installed." >&2
        echo "Usage: $0 <input_folder> <output_folder>" >&2
        return 1
    fi
}

# Pull one value out of loudnorm's JSON report
json_val() { sed -n "s/.*\"$1\" : \"\([^\"]*\)\".*/\1/p" <<<"$2" | tail -n1; }

# Prints the complete audio filter chain for one track: downmix + volume.
# Measures the track (after downmixing) first, so the gain fits that exact mix.
audio_filter() {
    local f=$1 idx=$2 ch=$3 base report max gain mi mtp mlra mth off

    if (( DIALOG_BOOST == 1 )) && [[ "$ch" == 6 ]]; then
        base=$BOOST_PAN
    else
        base="aformat=channel_layouts=stereo"
    fi

    case "$NORMALIZE" in
        peak)
            report=$(ffmpeg -nostdin -hide_banner -i "$f" -map "0:$idx" \
                        -af "$base,volumedetect" -f null - 2>&1 || true)
            max=$(sed -n 's/.*max_volume: \(-\?[0-9.]*\) dB.*/\1/p' <<<"$report" | tail -n1)
            if [[ -n "$max" ]]; then
                gain=$(awk -v t="$PEAK_DB" -v m="$max" 'BEGIN{printf "%.2f", t-m}')
                echo "    audio stream $idx: peak ${max} dB, gain ${gain} dB" >&2
                base+=",volume=${gain}dB"
            else
                echo "    audio stream $idx: could not measure, level unchanged" >&2
            fi
            ;;
        loudness)
            report=$(ffmpeg -nostdin -hide_banner -i "$f" -map "0:$idx" \
                        -af "$base,loudnorm=I=$LOUD_I:TP=$LOUD_TP:LRA=$LOUD_LRA:print_format=json" \
                        -f null - 2>&1 || true)
            mi=$(json_val input_i "$report");      mtp=$(json_val input_tp "$report")
            mlra=$(json_val input_lra "$report");  mth=$(json_val input_thresh "$report")
            off=$(json_val target_offset "$report")
            if [[ -n "$mi" && "$mi" != "-inf" ]]; then
                echo "    audio stream $idx: ${mi} LUFS -> ${LOUD_I} LUFS" >&2
                # loudnorm works at 192 kHz internally, so resample back to 48 kHz
                base+=",loudnorm=I=$LOUD_I:TP=$LOUD_TP:LRA=$LOUD_LRA"
                base+=":measured_I=$mi:measured_TP=$mtp:measured_LRA=$mlra"
                base+=":measured_thresh=$mth:offset=$off:linear=true,aresample=48000"
            else
                echo "    audio stream $idx: could not measure, level unchanged" >&2
            fi
            ;;
    esac
    printf '%s' "$base"
}

IN="${1:-}"
OUT="${2:-}"
[[ -z "$IN"  ]] && { IN=$(pick_dir "Select folder with MKV files") || exit 1; }
[[ -z "$OUT" ]] && { OUT=$(pick_dir "Select output folder") || exit 1; }

[[ -d "$IN" ]] || { echo "Input folder not found: $IN" >&2; exit 1; }
mkdir -p "$OUT"

files=("$IN"/*.mkv)
(( ${#files[@]} )) || { echo "No .mkv files in $IN"; exit 0; }

count=0
for f in "${files[@]}"; do
    name=$(basename "${f%.*}")
    out="$OUT/$name.mp4"
    count=$((count + 1))

    if [[ -e "$out" ]]; then
        echo "[$count/${#files[@]}] Skipping (already exists): $name.mp4"
        continue
    fi
    echo "[$count/${#files[@]}] $name"

    # ---- Video: first video stream only (cover art and font attachments ignored),
    #      scaled to 720p, 10-bit -> 8-bit so any device can play it.
    vargs=(-map 0:v:0 -vf "scale=-2:720"
           -c:v libx264 -preset "$PRESET" -crf "$CRF" -pix_fmt yuv420p)
    [[ -n "$TUNE" ]] && vargs+=(-tune "$TUNE")

    # ---- Audio: read each track's stream index, channel count and language
    declare -A chans=()
    eng=(); jpn=(); other=()
    while IFS=, read -r idx ch lang; do
        [[ -z "$idx" ]] && continue
        chans[$idx]=$ch
        case "${lang,,}" in
            eng|en) eng+=("$idx") ;;
            jpn|ja) jpn+=("$idx") ;;
            *)      other+=("$idx") ;;
        esac
    done < <(ffprobe -v error -select_streams a \
                -show_entries stream=index,channels:stream_tags=language \
                -of csv=p=0 "$f")

    order=("${eng[@]}" "${jpn[@]}" "${other[@]}")
    if (( ${#order[@]} == 0 )); then
        echo "  No audio tracks found, skipping: $f" >&2
        unset chans
        continue
    fi
    (( ${#eng[@]} == 0 )) && echo "  Note: no English-tagged audio; original order kept for the rest"

    [[ "$NORMALIZE" != none ]] && echo "  Measuring audio levels..."
    aargs=()
    j=0
    for idx in "${order[@]}"; do
        aargs+=(-map "0:$idx")

        if   [[ " ${eng[*]} " == *" $idx "* ]]; then title="English"
        elif [[ " ${jpn[*]} " == *" $idx "* ]]; then title="Japanese"
        else title=""
        fi
        [[ -n "$title" ]] && aargs+=("-metadata:s:a:$j" "title=$title" "-metadata:s:a:$j" "handler_name=$title")

        # first output track is the default, the rest are not
        if (( j == 0 )); then aargs+=("-disposition:a:$j" default)
        else                  aargs+=("-disposition:a:$j" 0)
        fi

        aargs+=("-filter:a:$j" "$(audio_filter "$f" "$idx" "${chans[$idx]}")")
        j=$((j + 1))
    done
    aargs+=(-c:a aac -ac 2 -ar 48000 -b:a "$AUDIO_BITRATE")
    unset chans

    # Chapters are carried over automatically; language tags stay with each track.
    # Subtitle streams are never mapped, so none end up in the MP4.
    echo "  Encoding..."
    if ffmpeg -nostdin -hide_banner -loglevel warning -stats \
        -i "$f" \
        "${vargs[@]}" \
        "${aargs[@]}" \
        -map_metadata 0 \
        -movflags +faststart \
        "$out.part.mp4"; then
        mv "$out.part.mp4" "$out"
    else
        echo "  FAILED: $f" >&2
        rm -f "$out.part.mp4"
    fi
done

echo "Done. Output in: $OUT"
