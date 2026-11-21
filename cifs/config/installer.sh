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
  rsync curl screen scrub ethtool aptitude sudo samba sshpass \
  libdbus-1-dev libdbus-glib-1-dev bc git locales mailutils \
  apt-transport-https nmap bpytop binutils iperf3 cron fastfetch \
  cron-daemon-common cifs-utils avahi-daemon htop

## User configuration
groupadd -g $SHRGID $SHRGROUP
adduser --shell=/bin/false --uid $SHRUID --gid $SHRGID $SHRUSER
passwd ${SHRUSER} << EOD
${SHRPASS}
${SHRPASS}
EOD
smbpasswd -a -s ${SHRUSER} << EOD
DUMMY
DUMMY
EOD
smbpasswd -a -s ${SHRUSER} << EOD
${SHRPASS}
${SHRPASS}
EOD
usermod -a -G shared ${SHRUSER}

## Samba configuration 
cp -f /tmp/config/smb.conf /etc/samba/
chmod 644 /etc/samba/smb.conf
chown root:root /etc/samba/smb.conf

## Reload services
systemctl daemon-reload

## Auto-starting services
systemctl enable smbd nmbd avahi-daemon

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

## Remove sysctl settings (systemd-sysctl is skipped in an LXC, so they never applied)
rm -f /etc/sysctl.conf

## Remove legacy boot service
rm -f /etc/systemd/system/multi-user.target.wants/rc-local.service
rm -f /etc/systemd/system/rc-local.service
rm -f /etc/rc.local

## Clean-up
rm -rf /tmp/config
exit
