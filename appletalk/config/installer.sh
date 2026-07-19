#!/bin/bash
### ONLY RUN ON THE SERVER!!
### by Ben Provenzano III
###

SHRUID="1000"
SHRGID="1000"
SHRUSER="ben"
SHRGROUP="shared"
SHRPASS="ben1995"

## Set working directory
cd /tmp/config

## Update sources
apt-get --yes update

## Install packages
apt-get install -y --no-upgrade --ignore-missing unzip wget \
  screen scrub ethtool aptitude sudo sshpass libdbus-1-dev \
  libdbus-glib-1-dev locales apt-transport-https binutils \
  fastfetch autofs htop cifs-utils nmap avahi-daemon

## User configuration
groupadd -g $SHRGID $SHRGROUP
adduser --shell=/bin/false --uid $SHRUID --gid $SHRGID $SHRUSER
usermod -a -G $SHRGROUP $SHRUSER
passwd ${SHRUSER} << EOD
${SHRPASS}
${SHRPASS}
EOD

## AppleTalk
apt-get install --assume-yes --no-install-recommends bison ca-certificates \
  cmark-gfm cracklib-runtime file flex gcc libacl1-dev libavahi-client-dev \
  libcrack2-dev libcups2-dev libdb-dev libdbus-1-dev libevent-dev libgcrypt20-dev \
  libglib2.0-dev libiniparser-dev libkrb5-dev libldap2-dev libmariadb-dev \
  libpam0g-dev libsqlite3-dev libtalloc-dev libtirpc-dev libtracker-sparql-3.0-dev \
  libwrap0-dev meson ninja-build quota systemtap-sdt-dev tcpd tracker \
  tracker-miner-fs valgrind
if [ ! -e "/usr/lib/systemd/system/netatalk.service" ]; then
  ## download source code
  tar -xvf ./netatalk-4.4.3.tar.xz
  cd netatalk-4.4.3
  ## compile from source
  meson setup build -Dbuildtype=release -Dwith-appletalk=true -Dwith-cups-pap-backend=true \
    -Dwith-dbus-sysconf-path=/usr/share/dbus-1/system.d -Dwith-init-hooks=false \
    -Dwith-init-style=debian-sysv,systemd -Dwith-pkgconfdir-path=/etc/netatalk \
    -Dwith-tests=true -Dwith-testsuite=true
  meson test -C build
  meson install -C build
  cd -
fi
cp -f /tmp/config/afp.conf /etc/netatalk/
chmod 644 /etc/netatalk/afp.conf 
chown root:root /etc/netatalk/afp.conf
mkdir -p /opt/afpdb

## AutoFS configuration
cp -f /tmp/config/auto.master /etc/
chmod 644 /etc/auto.master
chown root:root /etc/auto.master
cp -f /tmp/config/auto.map /etc/
chmod 644 /etc/auto.map
chown root:root /etc/auto.map
cp -f /tmp/config/auto.creds /etc/
chmod 400 /etc/auto.creds
chown root:root /etc/auto.creds
mkdir -p /mnt/smb

## Reload services
systemctl daemon-reload

## Enable auto-starting services
systemctl enable netatalk atalkd autofs avahi-daemon

## Set locale
if [ ! -e "/etc/locales.generated" ]; then
  dpkg-reconfigure locales
  touch /etc/locales.generated
fi

## Disable password login
passwd -d root

## SSH configuration
mkdir -p /root/.ssh
cp /tmp/config/authorized_keys /root/.ssh/
chown root:root /root/.ssh/authorized_keys
chmod 644 /root/.ssh/authorized_keys
cp /tmp/config/sshd_config /etc/ssh
chmod 644 /etc/ssh/sshd_config
chown root:root /etc/ssh/sshd_config

## Quiet login 
touch /root/.hushlogin
chmod 644 /root/.hushlogin
chown root:root /root/.hushlogin

## Login script
cp /tmp/config/profile /root/.profile
chown root:root /root/.profile
chmod +x /root/.profile

## System configuration
cp /tmp/config/sysctl.conf /etc
chmod 644 /etc/sysctl.conf
chown root:root /etc/sysctl.conf

## Startup configuration
cp /tmp/config/rc-local.service /etc/systemd/system/
chmod 644 /etc/systemd/system/rc-local.service
chown root:root /etc/systemd/system/rc-local.service
cp /tmp/config/rc.local /etc/
chmod 755 /etc/rc.local
chown root:root /etc/rc.local
systemctl enable rc-local

## Clean-up
rm -rf /tmp/config
exit
