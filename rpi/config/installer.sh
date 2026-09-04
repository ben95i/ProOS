#!/bin/bash
## Raspberry Pi ProOS Server Setup Script v17
## run this 1st, then module installer

# Installer Files Path
TMPDIR="/opt/rpi/config"
# Import Hostname
NEWHOST=`cat $TMPDIR/hostname`
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

cd $TMPDIR

echo ""
echo "########### Welcome to ProOS ! ###########"
echo "############## Configurator ##############"
echo "######### by Ben Provenzano III ##########"
echo ""

###################################################
### Initial #########################
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
 samba-common-bin samba-libs libjson-glib-1.0-0 libupnp6 alsa-base alsa-utils uglifyjs \
 nodejs mpv raspi-utils lame usbutils lighttpd yui-compressor 

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

## Recompile All Programs
rm -rf /opt/source-backup

## Reset LED
rm -f /opt/rpi/remotes/leds
rm -f /tmp/global-fc.lock

echo "Phase I setup complete."
fi ###### END OF SUB-SCRIPT #######################
###################################################

###################################################
########## Install System ###########
echo "Installing system."

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
cp -f $TMPDIR/config.txt /boot/firmware/

## Disable Serial Console Redirection
sed -i 's/console=serial0,115200 //g' /boot/firmware/cmdline.txt

## Boot to Console
systemctl set-default multi-user.target

## WiFi
raspi-config nonint do_wifi_country US

## Copy SSH
cp $TMPDIR/sshd_config /etc/ssh
cp $TMPDIR/ssh_config /etc/ssh
chmod 644 /etc/ssh/sshd_config
chown root:root /etc/ssh/sshd_config
chmod 644 /etc/ssh/ssh_config
chown root:root /etc/ssh/ssh_config

## Copy SSH Key for Root
rm -r /root/.ssh
mkdir -p /root/.ssh
cp -f $TMPDIR/authorized_keys /root/.ssh
chmod 644 /root/.ssh/authorized_keys
chown root:root /root/.ssh/authorized_keys

## Copy Bash Login Script
cp -f $TMPDIR/bashrc /root/.bashrc
chmod +x /root/.bashrc
chown root:root /root/.bashrc
cp -f $TMPDIR/bashrc /home/pi/.bashrc
chmod +x /home/pi/.bashrc
chown pi:pi /home/pi/.bashrc

## Remove Root Password
passwd -d root

## System
cp -f $TMPDIR/sysctl.conf /etc
chmod 644 /etc/sysctl.conf
chown root:root /etc/sysctl.conf

## Network
cp -f $TMPDIR/NetworkManager.conf /etc/NetworkManager/
chmod 644 /etc/NetworkManager/NetworkManager.conf
chown root:root /etc/NetworkManager/NetworkManager.conf
cp -f $TMPDIR/captive.conf /etc/NetworkManager/dnsmasq-shared.d/
chmod 644 /etc/NetworkManager/dnsmasq-shared.d/captive.conf
chown root:root /etc/NetworkManager/dnsmasq-shared.d/captive.conf

## Journal
cp -f $TMPDIR/journald.conf /etc/systemd/
chmod 644 /etc/systemd/journald.conf
chown root:root /etc/systemd/journald.conf

## Kernel Drivers
cp -f $TMPDIR/modules /etc
chmod 644 /etc/modules
chown root:root /etc/modules

## Detect if proinit service is installed
if [ ! -e "/etc/systemd/system/proinit.service" ]; then
  echo "proinit.service not found, resetting services."
  rm -f /etc/rpi-conf.done
fi

## System-D Services
chmod -fR 644 $TMPDIR/systemd/*.timer
chown -fR root:root $TMPDIR/systemd/*.timer
chmod -fR 644 $TMPDIR/systemd/*.service
chown -fR root:root $TMPDIR/systemd/*.service
cp -fr $TMPDIR/systemd/* /etc/systemd/system/

## Startup Scripts
cp -f $TMPDIR/netmode.sh /etc/
chmod +x /etc/netmode.sh
chown root:root /etc/netmode.sh
cp -f $TMPDIR/preinit.sh /etc/
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
  ## ShairPort
  sed -i "s/RaspberryPi/$MODNAME/g" /etc/systemd/system/rpi-airplay.service
fi

## API server
API_PATH="/opt/nodeapi"
mkdir -p $API_PATH
mv -f /opt/rpi/custom.js $API_PATH/
mv -f /opt/rpi/ledparser.js $API_PATH/
mv -f /opt/rpi/controller.js $API_PATH/
cd $API_PATH
API_PKG_CHKSUM=$(cat package.json package-lock.json 2>/dev/null | sha256sum | cut -d' ' -f1)
if [ ! -d node_modules ] || [ "$API_PKG_CHKSUM" != "$(cat "$API_PATH/.deps.hash" 2>/dev/null)" ]; then
  echo "API dependencies changed, installing..."
  npm install
  echo "$API_PKG_CHKSUM" > "$API_PATH/.deps.hash"
else
  echo "API dependencies unchanged, skipping npm install"
fi
cd $TMPDIR

## Light web server configuration
rm -rf /etc/lighttpd/conf-enabled/*
cp -f $TMPDIR/lighttpd.conf /etc/lighttpd/
chmod 644 /etc/lighttpd/lighttpd.conf
chown root:root /etc/lighttpd/lighttpd.conf

## WWW paths
WWWPATH="/var/www/html"
WWW_NAME="www-data"
WWW_GROUP="www-data"

## install WWW files
rm -rf "$WWWPATH"
mkdir -p "$WWWPATH"
mv -f $TMPDIR/html-base/* "$WWWPATH"
mv -f $TMPDIR/html/* "$WWWPATH"
mkdir -p "$WWWPATH/ram"
touch "$WWWPATH/ram/sysout.txt"
chmod -R +x "$WWWPATH"
chown -R "$WWW_NAME":"$WWW_GROUP" "$WWWPATH"
mkdir -p /var/www/sessions
chmod -R g+rx /var/www/sessions
chown -R "$WWW_NAME":"$WWW_GROUP" /var/www/sessions
mkdir -p /var/www/uploads
chmod -R g+rx /var/www/uploads
chown -R "$WWW_NAME":"$WWW_GROUP" /var/www/uploads
if [ ! -e "/opt/rpi/modconf/brand.txt" ]; then
  ## No module selected
  sed -i "s/>Automate</>RaspberryPi</g" "$WWWPATH/index.html"
else
  ## Set module name
  sed -i "s/>Automate</>$MODNAME</g" "$WWWPATH/index.html"
fi

## Minify JS/CSS
$API_PATH/www-minify.sh

## WWW Permissions
usermod -aG audio "$WWW_NAME"
usermod -aG $WWW_NAME pi
rm -f /etc/sudoers.d/www-perms
rm -f /etc/sudoers.d/www-nopasswd
rm -f /etc/sudoers.d/www-mod-nopasswd
rm -f /etc/sudoers.d/nobody-perms
sh -c "touch /etc/sudoers.d/www-perms"
sh -c "echo \"$WWW_NAME ALL=(root) NOPASSWD:/opt/rpi/servercmds.sh\" >> /etc/sudoers.d/www-perms"
sh -c "echo \"$WWW_NAME ALL=(root) NOPASSWD:/opt/rpi/relaxrun.sh\" >> /etc/sudoers.d/www-perms"
chown root:root /etc/sudoers.d/www-perms
chmod u=rwx,g=rx,o=rx /etc/sudoers.d/www-perms
chmod u=r,g=r,o= /etc/sudoers.d/www-perms

## Pi Permissions
rm -f /etc/sudoers.d/010_pi-perms
rm -f /etc/sudoers.d/010_pi-nopasswd
sh -c "touch /etc/sudoers.d/010_pi-nopasswd"
sh -c "echo \"pi ALL=(root) NOPASSWD: ALL\" >> /etc/sudoers.d/010_pi-nopasswd"
chown root:root /etc/sudoers.d/010_pi-nopasswd
chmod u=rwx,g=rx,o=rx /etc/sudoers.d/010_pi-nopasswd
chmod u=r,g=r,o= /etc/sudoers.d/010_pi-nopasswd
usermod -a -G sudo pi

## X Server
usermod -a -G tty pi
usermod -a -G video pi
cp -f $TMPDIR/Xwrapper.config /etc/X11
chmod 644 /etc/X11/Xwrapper.config
chown root:root /etc/X11/Xwrapper.config
mkdir -p /home/pi/.config/lxsession
chown -R pi:pi /home/pi/.config/lxsession
mkdir -p /home/pi/.config/lxsession/LXDE
chown -R pi:pi /home/pi/.config/lxsession/LXDE
cp -f $TMPDIR/xdesktop.conf /home/pi/.config/lxsession/LXDE/desktop.conf
chmod 644 /home/pi/.config/lxsession/LXDE/desktop.conf
chown pi:pi /home/pi/.config/lxsession/LXDE/desktop.conf
cp -f $TMPDIR/xautostart /home/pi/.config/lxsession/LXDE/autostart
chmod +x /home/pi/.config/lxsession/LXDE/autostart
chown pi:pi /home/pi/.config/lxsession/LXDE/autostart

## Bluetooth
cp -f $TMPDIR/btmain.conf /etc/bluetooth/main.conf
if [ ! -e "/opt/rpi/modconf/brand.txt" ]; then
echo "Skip module naming."
else
## Set module name to bluetooth config
sed -i "s/RaspberryPi/$MODNAME/g" /etc/bluetooth/main.conf
fi
chown root:root /etc/bluetooth/main.conf
chmod 644 /etc/bluetooth/main.conf
mkdir -p /etc/systemd/system/bluetooth.service.d
cp -f $TMPDIR/bt-override.conf /etc/systemd/system/bluetooth.service.d/override.conf
chown root:root /etc/systemd/system/bluetooth.service.d/override.conf
chmod 644 /etc/systemd/system/bluetooth.service.d/override.conf
# Make Bluetooth Discoverable
mkdir -p /etc/systemd/system/bthelper@.service.d
cp -f $TMPDIR/bthelper-override.conf /etc/systemd/system/bthelper@.service.d/override.conf
chown root:root /etc/systemd/system/bthelper@.service.d/override.conf
chmod 644 /etc/systemd/system/bthelper@.service.d/override.conf
# BlueALSA
rm -f /etc/systemd/system/bluealsa.service
cp -f $TMPDIR/bluealsa.service /lib/systemd/system/bluealsa.service  
chown root:root /lib/systemd/system/bluealsa.service
chmod 644 /lib/systemd/system/bluealsa.service
mkdir -p /etc/systemd/system/bluealsa.service.d
cp -f $TMPDIR/bluealsa-override.conf /etc/systemd/system/bluealsa.service.d/override.conf
chown root:root /etc/systemd/system/bluealsa.service.d/override.conf
chmod 644 /etc/systemd/system/bluealsa.service.d/override.conf
# Bluetooth UDEV Script
cp -f $TMPDIR/bluetooth-udev /usr/local/bin/bluetooth-udev
chown root:root /usr/local/bin/bluetooth-udev
chmod +x /usr/local/bin/bluetooth-udev
cp -f $TMPDIR/bluetooth-udev.rules /etc/udev/rules.d/99-bluetooth-udev.rules
chown root:root /etc/udev/rules.d/99-bluetooth-udev.rules
chmod 644 /etc/udev/rules.d/99-bluetooth-udev.rules
## Bluetooth Input
cp -f $TMPDIR/btinput.conf /etc/bluetooth/input.conf
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

## Hostname
if [ ! -e "$TMPDIR/hostname" ]; then
echo "Skipping hostname modification."
else
## Write hostname file
cp -r $TMPDIR/hostname /etc/hostname
chmod 644 /etc/hostname
chown root:root /etc/hostname
## Write hosts file
cp -f $TMPDIR/hosts /etc
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

## ALSA
cp -f $TMPDIR/asound.conf /etc/
chmod 644 /etc/asound.conf
chown root:root /etc/asound.conf
cp -f $TMPDIR/aliases.conf /lib/modprobe.d/
chmod 644 /lib/modprobe.d/aliases.conf
chown root:root /lib/modprobe.d/aliases.conf

## Samba USB Share
cp -f $TMPDIR/smb.conf /etc/samba
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
cp -f $TMPDIR/udevd-override.conf /etc/systemd/system/systemd-udevd.service.d/override.conf
chmod -f 644 /etc/systemd/system/systemd-udevd.service.d/override.conf
chown -f root:root /etc/systemd/system/systemd-udevd.service.d/override.conf
## Run script when USB drive plugged in 
cp -f $TMPDIR/10-udev-disks.rules /etc/udev/rules.d/
chmod 644 /etc/udev/rules.d/10-udev-disks.rules
chown root:root /etc/udev/rules.d/10-udev-disks.rules
udevadm control --reload-rules

## Motion Default
cp -f $TMPDIR/motion /etc/default/motion
chmod 644 /etc/default/motion
chown root:root /etc/default/motion
mkdir -p /etc/motion
cp -f $TMPDIR/motion.conf /etc/motion/motion.conf
chmod 644 /etc/motion/motion.conf
chown root:root /etc/motion/motion.conf

## Compile Programs
$TMPDIR/compile.sh

## Services
cp -f $TMPDIR/timesyncd.conf /etc/systemd/
chmod 644 /etc/systemd/timesyncd.conf
chown root:root /etc/systemd/timesyncd.conf
cp -f $TMPDIR/keyboard-setup.service /lib/systemd/system/
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
  echo "Initial setup (phase II) complete"
  touch /etc/rpi-conf.done
else
  echo "Skipping services."
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

## Set Permissions
chmod -R +x /opt/rpi
chown -R root:root /opt/rpi

## Clean systemd logs
journalctl --flush --rotate
journalctl -m --vacuum-time=300s

## Aliases
ln -sf /opt/rpi/rpi.sh /usr/bin/rpi

## Cleanup
echo "Removing temporary files..."
rm -rf $TMPDIR
apt autoclean
apt clean

echo ""
echo "*** complete ***"
exit

