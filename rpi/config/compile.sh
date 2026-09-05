##### Compiled from Source #####
LED_EF="/opt/rpi/effects"
SRC_BIN="/opt/rpi"
SRC_BKP="/opt/source-backup"
mkdir -p "$SRC_BKP"

## Helpers ############################################

# checksum <file> -> sha256 hash, or empty if the file is missing
checksum() {
  [ -f "$1" ] && sha256sum "$1" | cut -d' ' -f1
}

# build <description> <source> <binary> <compile command...>
# Compares the sha256 of the dropped-in source against the archived copy.
# Rebuilds on a mismatch, whenever the output binary is missing, or whenever a
# file in $SHARED_DEPS has changed, then archives the source either way.
build() {
  local desc="$1" src="$2" out="$3"
  shift 3
  local bkp="$SRC_BKP/$(basename "$src")"

  # A dropped-in source is not required to trigger a rebuild: a shared
  # dependency may have changed, or the binary may be missing. Either way the
  # source was archived by an earlier run, so restore it to compile against.
  if [ ! -f "$src" ] && { [ ! -f "$out" ] || deps_changed $SHARED_DEPS; }; then
    cp -f "$bkp" "$src" 2>/dev/null
  fi

  [ -f "$src" ] || return 0

  if [ "$(checksum "$src")" != "$(checksum "$bkp")" ] || [ ! -f "$out" ] \
     || deps_changed $SHARED_DEPS; then
    echo "Compiling $desc..."
    rm -f "$out"
    "$@"
  fi
  mv -f "$src" "$SRC_BKP/"
}

# Sources linked into more than one binary. build() consults this list on top of
# its own source, so editing a shared file rebuilds every dependent. Set it
# before a section and clear it after; archive_deps runs once the whole section
# has been built, since archiving earlier would make the second and later
# binaries in the section see the shared file as unchanged.
SHARED_DEPS=""

# deps_changed <file...> -> true when any listed file differs from its archive
deps_changed() {
  local dep
  for dep in "$@"; do
    [ -f "$dep" ] || continue
    if [ "$(checksum "$dep")" != "$(checksum "$SRC_BKP/$(basename "$dep")")" ]; then
      return 0
    fi
  done
  return 1
}

# archive_deps <file...> -> copy shared sources to the archive. Copies rather
# than moves: the compile commands reference these files on every run, so they
# have to stay resident in place.
archive_deps() {
  local dep
  for dep in "$@"; do
    [ -f "$dep" ] && cp -f "$dep" "$SRC_BKP/"
  done
  return 0
}

# effect <description> <name> <flags> <libs>
# Effect programs in $LED_EF that link ledlayout.cpp + opcclient.cpp
effect() {
  local desc="$1" name="$2" flags="$3" libs="$4"
  build "$desc" "$LED_EF/$name.cpp" "$LED_EF/$name" \
    g++ $flags "$LED_EF/$name.cpp" "$LED_EF/ledlayout.cpp" "$LED_EF/opcclient.cpp" \
        -o "$LED_EF/$name" $libs
}

# tool <description> <name> <flags> <libs>
# Standalone single-file programs in $SRC_BIN
tool() {
  local desc="$1" name="$2" flags="$3" libs="$4"
  build "$desc" "$SRC_BIN/$name.cpp" "$SRC_BIN/$name" \
    g++ $flags "$SRC_BIN/$name.cpp" -o "$SRC_BIN/$name" $libs
}

## WS2812B DMA Library ################################
if [ ! -e "/usr/local/include/ws2811/ws2811.h" ]; then
  echo "Compiling WS28xx DMA Library..."
  (
    cd "$LED_EF/ws281x" || exit
    mkdir -p build
    cd build || exit
    cmake ..
    make
    make install
    ldconfig
  )
fi

## Servers ############################################
# opcutils holds the OPC framing, socket, brightness and dither code shared by
# opcserver and fcproxy.
SHARED_DEPS="$LED_EF/opcutils.cpp $LED_EF/opcutils.h \
             $LED_EF/httpapiserver.cpp $LED_EF/httpapiserver.h"

build "OPC to WS2812B Program" "$LED_EF/opcserver.cpp" "$LED_EF/opcserver" \
  g++ -std=c++17 -O3 -march=native -mtune=native \
      "$LED_EF/opcserver.cpp" "$LED_EF/opcutils.cpp" "$LED_EF/httpapiserver.cpp" \
      -o "$LED_EF/opcserver" -lws2811 -lpthread

build "Brightness Proxy Program" "$LED_EF/fcproxy.cpp" "$LED_EF/fcproxy" \
  g++ -std=c++17 -O3 -march=native -mtune=native \
      "$LED_EF/fcproxy.cpp" "$LED_EF/opcutils.cpp" "$LED_EF/httpapiserver.cpp" \
      -o "$LED_EF/fcproxy" -lpthread

archive_deps $SHARED_DEPS
SHARED_DEPS=""

## LED Effects ########################################
# Every effect links ledlayout.cpp and opcclient.cpp, so a change to either
# rebuilds the whole list below.
SHARED_DEPS="$LED_EF/ledlayout.cpp $LED_EF/ledlayout.h \
             $LED_EF/opcclient.cpp $LED_EF/opcclient.h"

#      description                      name        flags                  libs
effect "LED Text Scrolling Program"     textscroll  "-O3 -Wall"            ""
effect "LED Maze Program"               maze        "-std=c++17 -O2"       "-pthread"
effect "Video File to OPC Program"      mp4toleds   "-O3"                  "$(pkg-config --cflags --libs opencv4) -pthread"
effect "O-scope LED Program"            oscope      "-O3"                  "-lasound -pthread"
effect "Spectrogram LED Program"        spectro     "-O3"                  "-lasound -pthread"
effect "Noise LED Program"              noise       "-O3"                  "-lm"
effect "Orbits LED Program"             orbits      "-std=c++17 -O2"       "-lm"
effect "Miami LED Program"              miami       "-O3"                  "-lm"
effect "Stripes LED Program"            stripes     "-O3"                  "-lm"
effect "Chase LED Program"              chase       "-O3"                  "-lm"
effect "Color to OPC LED Program"       writecolor  "-O3"                  "-lm"

archive_deps $SHARED_DEPS
SHARED_DEPS=""

build "Color Lookup COBOL Program" "$LED_EF/colorscan.cbl" "$LED_EF/colorscan" \
  cobc -x --free "$LED_EF/colorscan.cbl" -o "$LED_EF/colorscan"

## GPIO / Serial Tools ################################
#    description                    name        flags        libs
tool "GPO Pin Control Program"      gposerver   ""           "-lgpiodcxx"
tool "GPI Pin Monitor Program"      pinmon      "-Wall -O3"  "-lgpiodcxx"
tool "LDR Detector Program"         ldrdetect   ""           "-lgpiodcxx"

build "Z-Term RS232 Communication Program" "$SRC_BIN/ztermcom.c" "/usr/bin/ztermcom" \
  gcc "$SRC_BIN/ztermcom.c" -o /usr/bin/ztermcom

usermod -a -G dialout www-data
usermod -a -G tty www-data
#######################################################
