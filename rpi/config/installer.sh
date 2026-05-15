#!/bin/bash
## Raspberry Pi ProOS Server Setup Script v15
## run this 1st, then module installer

# Core Path
BIN="/opt/rpi/config"
# Import Hostname
NEWHOST=`cat $BIN/hostname`
# Import Module Name
MODNAME=`cat /opt/rpi/modconf/brand.txt`
## Installer Command Line Arguments
APTARGS="install -y --no-upgrade --ignore-missing"

echo "Installing on $NEWHOST..." 

## Pre-installation checks
if [ ! -e "/opt/rpi/rpi.sh" ]; then
  echo "Core components missing."
  exit
else
  echo "Core components integrity verified."
fi
if [ ! -e "/boot/firmware/config.txt" ]; then
  echo "Not running on a Pi !!"
  exit
else
  echo "Raspberry Pi detected."
fi
OSVER="$(sed -n 's|^VERSION=".*(\(.*\))"|\1|p' /etc/os-release)"
if [ "${OSVER}" = "trixie" ]; then
  echo "Debian ${OSVER} detected."
else
  echo "Unsupported OS version ${OSVER} detected."
  exit
fi

cd $BIN

echo ""
echo "########### Welcome to ProOS ! ###########"
echo "############## Configurator ##############"
echo "######### by Ben Provenzano III ##########"
echo ""

###################################################
### Initial configuration #########################
if [ ! -e "/etc/rpi-conf.done" ]; then
echo "*"
echo "Resettings to defaults..."
echo "*"

## Set boot partition to read/write
mount -o remount,rw /boot/firmware

## Set locale
raspi-config nonint do_change_locale LANG=en_US.UTF-8

## Set time zone
raspi-config nonint do_change_timezone America/New_York

## Set keyboard layout
raspi-config nonint do_configure_keyboard us

## Create pi user
adduser pi
chown -R pi:pi /home/pi
chsh -s /bin/bash pi

## Update Sources
apt-get -y update --allow-releaseinfo-change

## Essential Packages
apt-get $APTARGS locales console-setup aptitude libnss-mdns libnss3-tools xxd autofs \
 wget bc uuid-runtime iptables jq rsync screen parallel sudo nano curl wireless-regdb \
 zsync libpq5 htop lsb-release avahi-daemon avahi-utils hostapd dnsmasq-base pkg-config \
 cryptsetup cryptsetup-bin texi2html nmap autoconf gnucobol4 avrdude libao-common sox npm \
 wireless-tools overlayroot iw wpasupplicant dirmngr triggerhappy default-jre-headless sed \
 automake cifs-utils fuse apt-utils sqlite3 shairport-sync socat libexpat1 fastfetch ffmpeg \
 gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-plugins-ugly \
 gstreamer1.0-plugins-bad gstreamer1.0-alsa gstreamer1.0-libav libgstreamer-plugins-base1.0-0 \
 rng-tools5 gpiod net-tools python3 python3-pip python3-venv v4l-utils x264 xmltoman samba \
 build-essential autoconf make libtool binutils i2c-tools cmake yasm minicom rclone unzip \
 bluetooth pi-bluetooth bluez bluez-tools bluez-alsa-utils libbluetooth3 mpg321 arduino \
 samba-common-bin samba-libs libjson-glib-1.0-0 libupnp6 mpv  alsa-base alsa-utils \
 nodejs raspi-utils lame usbutils

## Development Support
apt-get $APTARGS libgtk2.0-dev libbluetooth-dev libpng-dev libdaemon-dev libpopt-dev \
 libjpeg-dev libgstreamer1.0-dev libupnp-dev libx264-dev libswscale-dev libavformat-dev \
 libjasper-dev libavfilter-dev libavdevice-dev libavc1394-dev libusb-1.0-0-dev libplist-dev \
 libjack-jackd2-dev portaudio19-dev libffi-dev libass-dev libfreetype6-dev libsdl1.2-dev \
 libglib2.0-dev libavutil-dev uuid-dev libsndfile1-dev libavahi-client-dev libtiff-dev \
 libtheora-dev libssl-dev libx11-dev libxml2-dev libxslt1-dev zlib1g-dev libdbus-1-dev \
 libva-dev libvdpau-dev libvorbis-dev libxext-dev libxfixes-dev libdbus-glib-1-dev \
 libsoxr-dev libao-dev libreadline-dev libsoup2.4-dev libgcrypt-dev libconfig-dev \
 libjson-glib-dev libsodium-dev libopencv-dev libgpiod-dev

## Install X11
apt-get $APTARGS xserver-xorg xorg x11-common x11-common xserver-xorg-input-evdev \
 xserver-xorg-legacy xvfb libxext6 libxtst6 xprintidle xdotool wmctrl openbox gpicview \
 lxde-common lxsession pcmanfm lxterminal xfce4-panel xfce4-whiskermenu-plugin

if [ ! -e "/usr/bin/arduino-cli" ]; then
  echo "Installing Arduino command line..."
  rm -f /opt/rpi/arduino-cli
  curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=/usr/bin sh
  arduino-cli version
  arduino-cli core update-index
  arduino-cli core install arduino:avr
  arduino-cli lib install Encoder@1.4.4
fi

## Light Web Server
apt-get $APTARGS lighttpd
chown www-data:www-data /var/www
usermod -a -G www-data pi
mkdir -p /var/www/html
chmod -R 777 /var/www/html
chown -R www-data:www-data /var/www/html

## Camera Motion Server
apt-get $APTARGS motion libmicrohttpd12t64
groupadd motion
useradd motion -g motion --shell /bin/false
groupmod -g 1005 motion
usermod -u 1005 motion

## Disable Swap Space
dphys-swapfile swapoff > /dev/null 2>&1
update-rc.d dphys-swapfile remove
if [ -e "/var/swap" ]; then
  echo "Removing swap file..."
  rm -fv /var/swap
fi

## Remove Unused Packages 
apt-get remove --purge -y cron anacron logrotate fake-hwclock ntp udhcpd usbmuxd pmount usbmount \
 cups cups-client cups-common cups-core-drivers cups-daemon cups-filters cups-filters-core-drivers \
 cups-ipp-utils cups-ppdc cups-server-common upower exim4 exim4-base exim4-config exim4-daemon-light \
 libudisks2-0 dnsmasq wolfram-engine libssl-doc libatasmart4 libavahi-glib1 rng-tools rng-tools-debian \
 plymouth plymouth-label plymouth-themes pulseaudio pulseaudio-utils pavucontrol pipewire pipewire-bin \
 iptables-persistent bridge-utils ntfs-3g lxlock xscreensaver xscreensaver-data gvfs gvfs-backends piwiz \
 tracker-extract tracker-miner-fs cloud-guest-utils cloud-init rpi-cloud-init-mods rpi-connect-lite \
 rpi-systemd-config rpi-swap systemd-zram-generator apparmor busybox-syslogd php-common php-cgi php \
 php-mysql mesa-vulkan-drivers dphys-swapfile neofetch python3-rpi.gpio python3-gpiozero
dpkg -l | grep unattended-upgrades
dpkg -r unattended-upgrades
rm -rf /etc/cron.*

## Clean-up Packages
apt-get autoremove -y
apt-get autoclean -y
apt-get clean -y

## Delete custom services
rm -fr /etc/systemd/system/rpi-*
rm -f /etc/systemd/system/usbplug.service

## Reset LED configuration
rm -f /opt/rpi/remotes/leds
rm -f /tmp/global-fc.lock

echo "Phase I setup complete."
fi ###### END OF SUB-SCRIPT #######################
###################################################

###################################################
########## Install System Configuration ###########
echo "Installing system configuration."

## Boot partition read-write
mount -o remount,rw /boot/firmware

## Delete flags, return to default
rm -f /boot/firmware/disable.wifi
rm -f /boot/firmware/apd.enable
rm -f /boot/firmware/apd.conf

## Default Boot Config
if [ ! -e "/etc/rpi-bootro.done" ]; then
  echo "Enabling read-only boot partition.."  
  raspi-config nonint enable_bootro
  touch /etc/rpi-bootro.done
fi
cp -f /boot/firmware/config.txt /boot/firmware/config.bak
cp -f $BIN/config.txt /boot/firmware/

## Disable Serial Console Redirection
sed -i 's/console=serial0,115200 //g' /boot/firmware/cmdline.txt

## Boot to Console
systemctl set-default multi-user.target

## WiFi Configuration
raspi-config nonint do_wifi_country US

## Copy SSH Configuration
cp $BIN/sshd_config /etc/ssh
cp $BIN/ssh_config /etc/ssh
chmod 644 /etc/ssh/sshd_config
chown root:root /etc/ssh/sshd_config
chmod 644 /etc/ssh/ssh_config
chown root:root /etc/ssh/ssh_config

## Copy SSH Key for Root
rm -r /root/.ssh
mkdir -p /root/.ssh
cp -f $BIN/authorized_keys /root/.ssh
chmod 644 /root/.ssh/authorized_keys
chown root:root /root/.ssh/authorized_keys

## Copy Bash Login Script
cp -f $BIN/bashrc /root/.bashrc
chmod +x /root/.bashrc
chown root:root /root/.bashrc
cp -f $BIN/bashrc /home/pi/.bashrc
chmod +x /home/pi/.bashrc
chown pi:pi /home/pi/.bashrc

## Remove Root Password
passwd -d root

## System Configuration
cp -f $BIN/sysctl.conf /etc
chmod 644 /etc/sysctl.conf
chown root:root /etc/sysctl.conf

## Journal Configuration
cp -f $BIN/journald.conf /etc/systemd/
chmod 644 /etc/systemd/journald.conf
chown root:root /etc/systemd/journald.conf

## Kernel Drivers Configuration
cp -f $BIN/modules /etc
chmod 644 /etc/modules
chown root:root /etc/modules

## Detect if proinit service is installed
if [ ! -e "/etc/systemd/system/proinit.service" ]; then
  echo "proinit.service not found, resetting services."
  rm -f /etc/rpi-conf.done
fi

## Replacement / New Services
cp -fr $BIN/systemd/* /etc/systemd/system/
chmod -fR 644 /etc/systemd/system/*.timer
chown -fR root:root /etc/systemd/system/*.timer
chmod -fR 644 /etc/systemd/system/*.service
chown -fR root:root /etc/systemd/system/*.service

## Startup Scripts
cp -f $BIN/netmode.sh /etc/
chmod +x /etc/netmode.sh
chown root:root /etc/netmode.sh
cp -f $BIN/preinit.sh /etc/
chmod +x /etc/preinit.sh
chown root:root /etc/preinit.sh

## NeoFetch Alias
rm -f /usr/bin/neofetch
ln -sf /usr/bin/fastfetch /usr/bin/neofetch

## ShairPort AirPlay Support
rm -f /etc/init.d/shairport-sync
if [ ! -e "/opt/rpi/modconf/brand.txt" ]; then
  echo "Skipping module modification."
else
  ## ShairPort configuration
  sed -i "s/RaspberryPi/$MODNAME/g" /etc/systemd/system/rpi-airplay.service
fi

## Light Web Server Configuration
rm -rf /etc/lighttpd/conf-enabled/*
cp -f $BIN/lighttpd.conf /etc/lighttpd/
chmod 644 /etc/lighttpd/lighttpd.conf
chown root:root /etc/lighttpd/lighttpd.conf

## Node.JS API Pi Specific Overrides
mv -f /opt/rpi/custom.js /opt/nodeapi/
mv -f /opt/rpi/ledparser.js /opt/nodeapi/
mv -f /opt/rpi/controller.js /opt/nodeapi/

## Node.JS API Server
mv -f /opt/nodeapi/nodeapi.service /etc/systemd/system/
chmod 644 /etc/systemd/system/nodeapi.service
chown root:root /etc/systemd/system/nodeapi.service
cd /opt/nodeapi
npm install
cd $BIN

## Lighttpd base website files
rm -rf /var/www/html
mkdir -p /var/www/html
mv -f $BIN/html-base/* /var/www/html
mv -f $BIN/html/* /var/www/html
mkdir -p /var/www/html/ram
touch /var/www/html/ram/sysout.txt
chmod -R +x /var/www/html
chown -R www-data:www-data /var/www/html
mkdir -p /var/www/sessions
chmod -R g+rx /var/www/sessions
chown -R www-data:www-data /var/www/sessions
mkdir -p /var/www/uploads
chmod -R g+rx /var/www/uploads
chown -R www-data:www-data /var/www/uploads

if [ ! -e "/opt/rpi/modconf/brand.txt" ]; then
  ## No module selected
  sed -i "s/>Automate</>RaspberryPi</g" /var/www/html/index.html
else
  ## Set module name
  sed -i "s/>Automate</>$MODNAME</g" /var/www/html/index.html
fi

## WWW Permissions
usermod -aG audio www-data
rm -f /etc/sudoers.d/www-perms
rm -f /etc/sudoers.d/www-nopasswd
rm -f /etc/sudoers.d/www-mod-nopasswd
rm -f /etc/sudoers.d/nobody-perms
sh -c "touch /etc/sudoers.d/www-perms"
sh -c "echo \"www-data ALL=(ALL) NOPASSWD:/opt/rpi/servercmds.sh\" >> /etc/sudoers.d/www-perms"
sh -c "echo \"www-data ALL=(ALL) NOPASSWD:/opt/rpi/relaxrun.sh\" >> /etc/sudoers.d/www-perms"
chown root:root /etc/sudoers.d/www-perms
chmod u=rwx,g=rx,o=rx /etc/sudoers.d/www-perms
chmod u=r,g=r,o= /etc/sudoers.d/www-perms

## Unrestricted Pi Permissions
rm -f /etc/sudoers.d/010_pi-perms
rm -f /etc/sudoers.d/010_pi-nopasswd
sh -c "touch /etc/sudoers.d/010_pi-nopasswd"
sh -c "echo \"pi ALL=(ALL) NOPASSWD: ALL\" >> /etc/sudoers.d/010_pi-nopasswd"
chown root:root /etc/sudoers.d/010_pi-nopasswd
chmod u=rwx,g=rx,o=rx /etc/sudoers.d/010_pi-nopasswd
chmod u=r,g=r,o= /etc/sudoers.d/010_pi-nopasswd
usermod -a -G sudo pi

## X Server Configuration
usermod -a -G tty pi
usermod -a -G video pi
cp -f $BIN/Xwrapper.config /etc/X11
chmod 644 /etc/X11/Xwrapper.config
chown root:root /etc/X11/Xwrapper.config
mkdir -p /home/pi/.config/lxsession
chown -R pi:pi /home/pi/.config/lxsession
mkdir -p /home/pi/.config/lxsession/LXDE
chown -R pi:pi /home/pi/.config/lxsession/LXDE
cp -f $BIN/xdesktop.conf /home/pi/.config/lxsession/LXDE/desktop.conf
chmod 644 /home/pi/.config/lxsession/LXDE/desktop.conf
chown pi:pi /home/pi/.config/lxsession/LXDE/desktop.conf
cp -f $BIN/xautostart /home/pi/.config/lxsession/LXDE/autostart
chmod +x /home/pi/.config/lxsession/LXDE/autostart
chown pi:pi /home/pi/.config/lxsession/LXDE/autostart

## Bluetooth Configuration
cp -f $BIN/btmain.conf /etc/bluetooth/main.conf
if [ ! -e "/opt/rpi/modconf/brand.txt" ]; then
echo "Skip module naming."
else
## Set module name to bluetooth config
sed -i "s/RaspberryPi/$MODNAME/g" /etc/bluetooth/main.conf
fi
chown root:root /etc/bluetooth/main.conf
chmod 644 /etc/bluetooth/main.conf
mkdir -p /etc/systemd/system/bluetooth.service.d
cp -f $BIN/bt-override.conf /etc/systemd/system/bluetooth.service.d/override.conf
chown root:root /etc/systemd/system/bluetooth.service.d/override.conf
chmod 644 /etc/systemd/system/bluetooth.service.d/override.conf
# Make Bluetooth Discoverable
mkdir -p /etc/systemd/system/bthelper@.service.d
cp -f $BIN/bthelper-override.conf /etc/systemd/system/bthelper@.service.d/override.conf
chown root:root /etc/systemd/system/bthelper@.service.d/override.conf
chmod 644 /etc/systemd/system/bthelper@.service.d/override.conf
# BlueALSA Configuration
rm -f /etc/systemd/system/bluealsa.service
cp -f $BIN/bluealsa.service /lib/systemd/system/bluealsa.service  
chown root:root /lib/systemd/system/bluealsa.service
chmod 644 /lib/systemd/system/bluealsa.service
mkdir -p /etc/systemd/system/bluealsa.service.d
cp -f $BIN/bluealsa-override.conf /etc/systemd/system/bluealsa.service.d/override.conf
chown root:root /etc/systemd/system/bluealsa.service.d/override.conf
chmod 644 /etc/systemd/system/bluealsa.service.d/override.conf
# Bluetooth UDEV Script
cp -f $BIN/bluetooth-udev /usr/local/bin/bluetooth-udev
chown root:root /usr/local/bin/bluetooth-udev
chmod +x /usr/local/bin/bluetooth-udev
cp -f $BIN/bluetooth-udev.rules /etc/udev/rules.d/99-bluetooth-udev.rules
chown root:root /etc/udev/rules.d/99-bluetooth-udev.rules
chmod 644 /etc/udev/rules.d/99-bluetooth-udev.rules
## Bluetooth Input Configuration
cp -f $BIN/btinput.conf /etc/bluetooth/input.conf
chmod 644 /etc/bluetooth/input.conf
chown root:root /etc/bluetooth/input.conf
## Disable Bluetooth Soft-Block
rfkill unblock bluetooth

## Remove unused udev network rules
rm -f /etc/udev/rules.d/70-persistent-net.rules
rm -f /lib/udev/rules.d/75-persistent-net-generator.rules

## Enable Network Time Sync
timedatectl set-ntp true
timedatectl status

## Set hostname to configurations
if [ ! -e "$BIN/hostname" ]; then
echo "Skipping hostname modification."
else
## Write hostname file
cp -r $BIN/hostname /etc/hostname
chmod 644 /etc/hostname
chown root:root /etc/hostname
## Write hosts file
cp -f $BIN/hosts /etc
chmod 644 /etc/hosts
chown root:root /etc/hosts
sed -i "s/raspberrypi/$NEWHOST/g" /etc/hosts
fi

## Create ALSA state file if not found
if [ ! -e "/var/lib/alsa/asound.state" ]; then
echo "Creating Alsa state file"
touch /var/lib/alsa/asound.state
echo '#' > /var/lib/alsa/asound.state
chmod 777 /var/lib/alsa/asound.state
fi

## ALSA Configuration
cp -f $BIN/asound.conf /etc/
chmod 644 /etc/asound.conf
chown root:root /etc/asound.conf
cp -f $BIN/aliases.conf /lib/modprobe.d/
chmod 644 /lib/modprobe.d/aliases.conf
chown root:root /lib/modprobe.d/aliases.conf

## Samba USB Share Configuration
cp -f $BIN/smb.conf /etc/samba
chmod 644 /etc/samba/smb.conf
chown root:root /etc/samba/smb.conf
if [ ! -e "/opt/rpi/modconf/brand.txt" ]; then
echo "Skipping module modification."
else
## Set module name to file share config
sed -i "s/RaspberryPi/$MODNAME/g" /etc/samba/smb.conf
fi

## Automount Fix System Override
mkdir -p /etc/systemd/system/systemd-udevd.service.d
cp -f $BIN/udevd-override.conf /etc/systemd/system/systemd-udevd.service.d/override.conf
chmod -f 644 /etc/systemd/system/systemd-udevd.service.d/override.conf
chown -f root:root /etc/systemd/system/systemd-udevd.service.d/override.conf
## Run script when USB drive plugged in 
cp -f $BIN/10-udev-disks.rules /etc/udev/rules.d/
chmod 644 /etc/udev/rules.d/10-udev-disks.rules
chown root:root /etc/udev/rules.d/10-udev-disks.rules
udevadm control --reload-rules

## Motion Default Configuration
cp -f $BIN/motion /etc/default/motion
chmod 644 /etc/default/motion
chown root:root /etc/default/motion
mkdir -p /etc/motion
cp -f $BIN/motion.conf /etc/motion/motion.conf
chmod 644 /etc/motion/motion.conf
chown root:root /etc/motion/motion.conf

##### Compiled from Source #####
NC_MSG="(Source Not Changed)"
LED_EF="/opt/rpi/effects"
SRC_BIN="/opt/rpi"
SRC_BKP="/opt/source-backup"
mkdir -p $SRC_BKP

## WS2812B DMA Library ################################
if [ ! -e "/usr/local/include/ws2811/ws2811.h" ]; then
  echo "Compiling WS28xx DMA Library..."
  cd $LED_EF/ws281x
  mkdir build
  cd build
  cmake ..
  make
  make install
  ldconfig
  cd $BIN
fi
#######################################################
if [ ! -f "$SRC_BKP/opcserver.cpp" ]; then
  touch $SRC_BKP/opcserver.cpp
fi
if ! cmp -s $LED_EF/opcserver.cpp $SRC_BKP/opcserver.cpp; then
    echo "Compiling OPC to WS2812B Program..."
    rm -f $LED_EF/opcserver
    g++ -O3 -std=c++17 $LED_EF/opcserver.cpp $LED_EF/httpapiserver.cpp -o $LED_EF/opcserver -lws2811 -lpthread
fi
mv -f $LED_EF/opcserver.cpp $SRC_BKP/
#######################################################
if [ ! -f "$SRC_BKP/fcproxy.cpp" ]; then
  touch $SRC_BKP/fcproxy.cpp
fi
if ! cmp -s $LED_EF/fcproxy.cpp $SRC_BKP/fcproxy.cpp; then
    echo "Compiling Brightness Proxy Program..."
    rm -f $LED_EF/fcproxy
    g++ -O3 $LED_EF/fcproxy.cpp $LED_EF/httpapiserver.cpp -o $LED_EF/fcproxy -lpthread
fi
mv -f $LED_EF/fcproxy.cpp $SRC_BKP/
#######################################################
if [ ! -f "$SRC_BKP/textscroll.cpp" ]; then
  touch $SRC_BKP/textscroll.cpp
fi
if ! cmp -s $LED_EF/textscroll.cpp $SRC_BKP/textscroll.cpp; then
    echo "Compiling LED Text Scrolling Program..."
    rm -f $LED_EF/textscroll
    g++ -O3 -Wall $LED_EF/textscroll.cpp $LED_EF/ledlayout.cpp $LED_EF/opcclient.cpp -o $LED_EF/textscroll
fi
mv -f $LED_EF/textscroll.cpp $SRC_BKP/
#######################################################
if [ ! -f "$SRC_BKP/mp4toleds.cpp" ]; then
  touch $SRC_BKP/mp4toleds.cpp
fi
if ! cmp -s $LED_EF/mp4toleds.cpp $SRC_BKP/mp4toleds.cpp; then
    echo "Compiling Video File to OPC Program..."
    rm -f $LED_EF/mp4toleds
    g++ -O3 $LED_EF/mp4toleds.cpp $LED_EF/ledlayout.cpp $LED_EF/opcclient.cpp -o $LED_EF/mp4toleds $(pkg-config --cflags --libs opencv4) -pthread
fi
mv -f $LED_EF/mp4toleds.cpp $SRC_BKP/
#######################################################
if [ ! -f "$SRC_BKP/spectro.cpp" ]; then
  touch $SRC_BKP/spectro.cpp
fi
if ! cmp -s $LED_EF/spectro.cpp $SRC_BKP/spectro.cpp; then
    echo "Compiling Spectrogram LED Program..."
    rm -f $LED_EF/spectro
    g++ -O3 $LED_EF/spectro.cpp $LED_EF/ledlayout.cpp $LED_EF/opcclient.cpp -o $LED_EF/spectro -lasound -pthread
fi
mv -f $LED_EF/spectro.cpp $SRC_BKP/
#######################################################
if [ ! -f "$SRC_BKP/noise.cpp" ]; then
  touch $SRC_BKP/noise.cpp
fi
if ! cmp -s $LED_EF/noise.cpp $SRC_BKP/noise.cpp; then
    echo "Compiling Noise LED Program..."
    rm -f $LED_EF/noise
    g++ -O3 $LED_EF/noise.cpp $LED_EF/ledlayout.cpp $LED_EF/opcclient.cpp -o $LED_EF/noise -lm
fi
mv -f $LED_EF/noise.cpp $SRC_BKP/
#######################################################
if [ ! -f "$SRC_BKP/orbits.cpp" ]; then
  touch $SRC_BKP/orbits.cpp
fi
if ! cmp -s $LED_EF/orbits.cpp $SRC_BKP/orbits.cpp; then
    echo "Compiling Orbits LED Program..."
    rm -f $LED_EF/orbits
    g++ -O3 $LED_EF/orbits.cpp $LED_EF/ledlayout.cpp $LED_EF/opcclient.cpp -o $LED_EF/orbits -lm
fi
mv -f $LED_EF/orbits.cpp $SRC_BKP/
#######################################################
if [ ! -f "$SRC_BKP/miami.cpp" ]; then
  touch $SRC_BKP/miami.cpp
fi
if ! cmp -s $LED_EF/miami.cpp $SRC_BKP/miami.cpp; then
    echo "Compiling Miami LED Program..."
    rm -f $LED_EF/miami
    g++ -O3 $LED_EF/miami.cpp $LED_EF/ledlayout.cpp $LED_EF/opcclient.cpp -o $LED_EF/miami -lm
fi
mv -f $LED_EF/miami.cpp $SRC_BKP/
#######################################################
if [ ! -f "$SRC_BKP/spark.cpp" ]; then
  touch $SRC_BKP/spark.cpp
fi
if ! cmp -s $LED_EF/spark.cpp $SRC_BKP/spark.cpp; then
    echo "Compiling Spark LED Program..."
    rm -f $LED_EF/spark
    g++ -O3 $LED_EF/spark.cpp $LED_EF/ledlayout.cpp $LED_EF/opcclient.cpp -o $LED_EF/spark -lm
fi
mv -f $LED_EF/spark.cpp $SRC_BKP/
#######################################################
if [ ! -f "$SRC_BKP/lava.cpp" ]; then
  touch $SRC_BKP/lava.cpp
fi
if ! cmp -s $LED_EF/lava.cpp $SRC_BKP/lava.cpp; then
    echo "Compiling Lava LED Program..."
    rm -f $LED_EF/lava
    g++ -O3 $LED_EF/lava.cpp $LED_EF/ledlayout.cpp $LED_EF/opcclient.cpp -o $LED_EF/lava -lm
fi
mv -f $LED_EF/lava.cpp $SRC_BKP/
#######################################################
if [ ! -f "$SRC_BKP/stripes.cpp" ]; then
  touch $SRC_BKP/stripes.cpp
fi
if ! cmp -s $LED_EF/stripes.cpp $SRC_BKP/stripes.cpp; then
    echo "Compiling Stripes LED Program..."
    rm -f $LED_EF/stripes
    g++ -O3 $LED_EF/stripes.cpp $LED_EF/ledlayout.cpp $LED_EF/opcclient.cpp -o $LED_EF/stripes -lm
fi
mv -f $LED_EF/stripes.cpp $SRC_BKP/
#######################################################
if [ ! -f "$SRC_BKP/chase.cpp" ]; then
  touch $SRC_BKP/chase.cpp
fi
if ! cmp -s $LED_EF/chase.cpp $SRC_BKP/chase.cpp; then
    echo "Compiling Chase LED Program..."
    rm -f $LED_EF/chase
    g++ -O3 $LED_EF/chase.cpp $LED_EF/ledlayout.cpp $LED_EF/opcclient.cpp -o $LED_EF/chase -lm
fi
mv -f $LED_EF/chase.cpp $SRC_BKP/
#######################################################
if [ ! -f "$SRC_BKP/writecolor.cpp" ]; then
  touch $SRC_BKP/writecolor.cpp
fi
if ! cmp -s $LED_EF/writecolor.cpp $SRC_BKP/writecolor.cpp; then
    echo "Compiling Color to OPC LED Program..."
    rm -f $LED_EF/writecolor
    g++ -O3 $LED_EF/writecolor.cpp $LED_EF/ledlayout.cpp $LED_EF/opcclient.cpp -o $LED_EF/writecolor -lm
fi
mv -f $LED_EF/writecolor.cpp $SRC_BKP/
#######################################################
if [ ! -f "$SRC_BKP/colorscan.cbl" ]; then
  touch $SRC_BKP/colorscan.cbl
fi
if ! cmp -s $LED_EF/colorscan.cbl $SRC_BKP/colorscan.cbl; then
    echo "Compiling Color Lookup COBOL Program..."
    rm -f $LED_EF/colorscan
    cobc -x --free $LED_EF/colorscan.cbl -o $LED_EF/colorscan
fi
mv -f $LED_EF/colorscan.cbl $SRC_BKP/
#######################################################
if [ ! -f "$SRC_BKP/gposerver.cpp" ]; then
  touch $SRC_BKP/gposerver.cpp
fi
if ! cmp -s $SRC_BIN/gposerver.cpp $SRC_BKP/gposerver.cpp; then
    echo "Compiling GPO Pin Control Program..."
    rm -f $SRC_BIN/gposerver
    g++ $SRC_BIN/gposerver.cpp -o $SRC_BIN/gposerver -lgpiodcxx
fi
mv -f $SRC_BIN/gposerver.cpp $SRC_BKP/
#######################################################
if [ ! -f "$SRC_BKP/pinmon.cpp" ]; then
  touch $SRC_BKP/pinmon.cpp
fi
if ! cmp -s $SRC_BIN/pinmon.cpp $SRC_BKP/pinmon.cpp; then
    echo "Compiling GPI Pin Monitor Program..."
    rm -f $SRC_BIN/pinmon
    g++ -Wall -O3 $SRC_BIN/pinmon.cpp -o $SRC_BIN/pinmon -lgpiodcxx
fi
mv -f $SRC_BIN/pinmon.cpp $SRC_BKP/
#######################################################
if [ ! -f "$SRC_BKP/ldrdetect.cpp" ]; then
  touch $SRC_BKP/ldrdetect.cpp
fi
if ! cmp -s $SRC_BIN/ldrdetect.cpp $SRC_BKP/ldrdetect.cpp; then
    echo "Compiling LDR Detector Program..."
    rm -f $SRC_BIN/ldrdetect
    g++ $SRC_BIN/ldrdetect.cpp -o $SRC_BIN/ldrdetect -lgpiodcxx
fi
mv -f $SRC_BIN/ldrdetect.cpp $SRC_BKP/
#######################################################
if [ ! -f "$SRC_BKP/ztermcom.c" ]; then
  touch $SRC_BKP/ztermcom.c
fi
if ! cmp -s $SRC_BIN/ztermcom.c $SRC_BKP/ztermcom.c; then
    echo "Compiling Z-Term RS232 Communication Program..."
    rm -f /usr/bin/ztermcom
    gcc $SRC_BIN/ztermcom.c -o /usr/bin/ztermcom
fi
mv -f $SRC_BIN/ztermcom.c $SRC_BKP/
usermod -a -G dialout www-data
usermod -a -G tty www-data
#######################################################

## Services Configuration
cp -f $BIN/timesyncd.conf /etc/systemd/
chmod 644 /etc/systemd/timesyncd.conf
chown root:root /etc/systemd/timesyncd.conf
cp -f $BIN/keyboard-setup.service /lib/systemd/system/
chmod 644 /lib/systemd/system/keyboard-setup.service
chown root:root /lib/systemd/system/keyboard-setup.service
rm -f /lib/systemd/system/shairport-sync.service
systemctl daemon-reload
if [ ! -e "/etc/rpi-conf.done" ]; then
  ## Active on startup
  systemctl unmask NetworkManager-wait-online NetworkManager-dispatcher \
    NetworkManager ModemManager systemd-journald hostapd motion
  systemctl enable ssh avahi-daemon proinit rpi-cleanup.timer \
    systemd-timesyncd systemd-time-wait-sync NetworkManager ModemManager \
    NetworkManager-wait-online NetworkManager-dispatcher
  ## Disabled on startup
  systemctl stop triggerhappy.socket
  systemctl disable apt-daily-upgrade.timer apt-daily.timer e2scrub_all.timer \
    sysstat-summary.timer triggerhappy.socket man-db.timer
  systemctl disable apt-daily-upgrade.service apt-daily.service autofs \
    e2scrub_all.service e2scrub_reap.service hostapd keyboard-setup sysstat \
    lighttpd motion serial-getty@ttyS0.service man-db.service nfs-blkmap \
    serial-getty@ttyAMA0.service winbind hciuart bluetooth bthelper@hci0 \
    bluealsa-aplay nmbd smbd samba-ad-dc triggerhappy sshswitch
  echo "Initial setup (phase II) complete."
  touch /etc/rpi-conf.done
else
  echo "Skipping services configuration."
fi

## Run module script if found
if [ ! -e "/opt/rpi/modconf/modinit" ]; then
  echo "Module script not found."
else
  echo "Running Module Script..."
  chmod +x /opt/rpi/modconf/modinit
  sh /opt/rpi/modconf/modinit
  rm -rf /opt/rpi/modconf
fi

## Regenerate Update Database
systemctl start man-db.service

## Restart Web Server
systemctl restart lighttpd
systemctl restart nodeapi

## Reset Null Device
rm -f /dev/null
mknod /dev/null c 1 3
chmod 666 /dev/null

## Reset Log Files
rm -f /var/log/lastlog
touch /var/log/lastlog
rm -f /var/log/faillog
touch /var/log/faillog
rm -f /var/log/btmp
touch /var/log/btmp
rm -f /var/log/wtmp
touch /var/log/wtmp
chmod -R 644 /var/log/wtmp /var/log/btmp /var/log/lastlog /var/log/faillog
chown -R root:utmp /var/log/wtmp /var/log/btmp /var/log/lastlog /var/log/faillog
rm -f /home/pi/.xsession-errors
touch /home/pi/.xsession-errors
rm -f /var/log/Xorg.0.log.old
touch /var/log/Xorg.0.log.old
rm -f /var/log/Xorg.0.log
touch /var/log/Xorg.0.log
chmod -R 777 /var/log/Xorg.0.log /var/log/Xorg.0.log.old
chmod -R 644 /home/pi/.xsession-errors
chown -R pi:pi /var/log/Xorg.0.log /var/log/Xorg.0.log.old /home/pi/.xsession-errors

## Execute Permissions
chmod -R +x /opt/rpi
chown -R root:root /opt/rpi

## Remove Installer Files
rm -rf /opt/rpi/RetroPie-Setup
rm -rf /opt/rpi/config
rm -rf /opt/rpi/pkgs
rm -f /usr/bin/leds
apt autoclean
apt clean

## Clean systemd logs
journalctl --flush --rotate
journalctl -m --vacuum-time=300s

## Aliases
ln -sf /opt/rpi/rpi.sh /usr/bin/rpi

echo ""
echo "Configuration Complete."
exit

