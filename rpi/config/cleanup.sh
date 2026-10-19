#!/bin/bash
## Raspberry Pi ProOS Cleanup by Ben Provenzano III
## Frees space on the SD card (an 8 GB card was 80% full): slims OpenCV,
## removes the X11 desktop, the Arduino apt toolchain, USB Wi-Fi dongle
## firmware, unused kernels and headers, docs, apt lists and the npm cache.
## Run once per Pi by installer.sh, which skips it while /etc/rpi-cleanup.done
## exists; delete that file and deploy again to run it a second time.

# Installer Files Path
TMPDIR="/opt/rpi/config"
## Installer Command Line Arguments
APTARGS="install -y --no-upgrade --ignore-missing"

## IS_INSTALLED <package> -> true while the package or its config is on the Pi
## Example: IS_INSTALLED libopencv-dev && echo "full OpenCV present"
IS_INSTALLED() {
  local packageStatus
  packageStatus=$(dpkg-query -W -f '${Status}' "$1" 2>/dev/null)
  case "$packageStatus" in
    *"ok installed"*|*"config-files"*) return 0 ;;
  esac
  return 1
}

## PURGE_INSTALLED <package...> -> purges the listed packages that are on the
## Pi and skips the rest, since apt-get stops on a name it does not know
## (a v6 kernel on a 64-bit OS, say)
## Example: PURGE_INSTALLED arduino avrdude
PURGE_INSTALLED() {
  local packageName
  local purgePackages=""
  for packageName in "$@"; do
    if IS_INSTALLED "$packageName"; then
      purgePackages="$purgePackages $packageName"
    fi
  done
  if [ -n "$purgePackages" ]; then
    echo "Removing$purgePackages"
    apt-get purge -y $purgePackages
  fi
}

## OpenCV: mp4toleds links only the core and videoio modules
if IS_INSTALLED libopencv-dev; then
  echo "Replacing libopencv-dev with the core and videoio modules..."
  aptListFiles=$(ls /var/lib/apt/lists/*_Packages 2>/dev/null)
  if [ -z "$aptListFiles" ]; then
    apt-get -y update --allow-releaseinfo-change
  fi
  apt-get $APTARGS libopencv-core-dev libopencv-videoio-dev
  apt-get purge -y libopencv-dev
fi

## X11 Desktop: no module starts an X server
PURGE_INSTALLED xserver-xorg xorg xserver-xorg-input-evdev xserver-xorg-legacy xvfb \
 xprintidle xdotool wmctrl openbox gpicview lxde-common lxsession pcmanfm lxterminal \
 xfce4-panel xfce4-whiskermenu-plugin
rm -rf /home/pi/.config/lxsession

## Arduino: arduino-cli brings its own avr-gcc and avrdude, and only a module
## whose modinit installs arduino-cli (brpi) keeps it
PURGE_INSTALLED arduino avrdude gcc-avr avr-libc binutils-avr
if ! grep -q "arduino-cli" /opt/rpi/modconf/modinit 2>/dev/null; then
  rm -f /usr/bin/arduino-cli
  rm -rf /root/.arduino15
  rm -rf /root/Arduino
fi

## USB Wi-Fi Dongle Firmware: onboard Wi-Fi uses firmware-brcm80211
PURGE_INSTALLED firmware-atheros firmware-mediatek firmware-libertas

## Kernels: keep the v7 and v8 flavours of the running and newest kernels, so
## the card still boots a Zero 2 W, 3B, 3A+ or 4. The v6 flavour is for the
## Pi 1 and Zero 1 only, and nothing here builds kernel modules, so no headers.
runningKernel=$(uname -r | sed 's/-rpi-.*//')
newestKernel=$(dpkg-query -W -f '${Package}\n' 'linux-image-[0-9]*' 2>/dev/null \
  | sed -n 's/^linux-image-\(.*\)-rpi-.*/\1/p' | sort -uV | tail -n 1)
kernelPackages=""
for packageName in $(dpkg-query -W -f '${binary:Package}\n' 'linux-image-*' \
                     'linux-headers-*' 'linux-kbuild-*' 2>/dev/null); do
  case "$packageName" in
    linux-headers-*) kernelPackages="$kernelPackages $packageName" ;;
    *-rpi-v6|*-rpi-v6:*) kernelPackages="$kernelPackages $packageName" ;;
    linux-image-rpi-*) ;;
    *"$runningKernel"*|*"$newestKernel"*) ;;
    *) kernelPackages="$kernelPackages $packageName" ;;
  esac
done
PURGE_INSTALLED $kernelPackages
## Module folders no package owns any more (the running kernel's always stays)
runningModuleDir="/usr/lib/modules/$(uname -r)"
for moduleDir in /usr/lib/modules/*; do
  if [ "$moduleDir" = "$runningModuleDir" ]; then
    continue
  fi
  if ! dpkg -S "$moduleDir" > /dev/null 2>&1; then
    echo "Removing $moduleDir"
    rm -rf "$moduleDir"
  fi
done

## Leftover Packages
apt-get autoremove --purge -y
rcPackages=$(dpkg -l | awk '/^rc/ {print $2}')
PURGE_INSTALLED $rcPackages

## Documentation: dpkg skips docs, man pages and non-English translations
cp -f $TMPDIR/dpkg-nodoc.conf /etc/dpkg/dpkg.cfg.d/01-nodoc
chmod 644 /etc/dpkg/dpkg.cfg.d/01-nodoc
chown root:root /etc/dpkg/dpkg.cfg.d/01-nodoc
find /usr/share/doc -type f ! -name copyright -delete
find /usr/share/doc -type d -empty -delete
rm -rf /usr/share/man/*
rm -rf /usr/share/info/*
find /usr/share/locale -mindepth 1 -maxdepth 1 ! -name 'en*' ! -name locale.alias \
  -exec rm -rf {} +

## Package Caches: lists come back with apt-get update (rpi update, a first run)
apt-get clean -y
rm -rf /var/lib/apt/lists/*
npm cache clean --force

touch /etc/rpi-cleanup.done
echo "Cleanup complete."
exit
