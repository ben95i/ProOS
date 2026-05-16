#!/bin/bash
### ONLY RUN ON THE SERVER!!
### AutoConfig - ProOS for Automate VM
### by Ben Provenzano III

WWWPATH="/var/www/html"
TMPDIR="/tmp/config"

## Update Sources
apt-get --yes update

## Install Packages
apt-get install -y --no-upgrade --ignore-missing dirmngr ca-certificates bpytop \
 apt-transport-https wget unzip gnupg rsync curl screen parallel libdbus-1-dev \
 ethtool libdbus-glib-1-dev locales aptitude sudo gnupg scrub binutils nmap fastfetch \
 avahi-daemon ffmpeg pip python3-ament-xmllint etherwake wakeonlan npm nodejs

## Remove Packages
apt-get remove -y --purge cron anacron postfix apache2 apache2-data htop

## CSS Minifier
apt-get install -y --no-upgrade --ignore-missing yui-compressor default-jre-headless
mkdir -p /usr/lib/jvm/java-8-openjdk-amd64
ln -sf /usr/lib/jvm/default-java /usr/lib/jvm/java-8-openjdk-amd64/jre
if [ ! -e "/usr/local/bin/uglifyjs" ]; then
  ## Minify JS
  npm install uglify-js -g
fi  

## Process Monitor
if [ ! -e "/usr/local/bin/htop" ]; then
  apt-get remove -y htop
  apt-get install -y --no-upgrade --ignore-missing libncursesw5-dev \
   autotools-dev autoconf automake build-essential
  cd $TMPDIR
  wget https://github.com/htop-dev/htop/releases/download/3.2.1/htop-3.2.1.tar.xz
  tar -xf htop-3.2.1.tar.xz
  cd htop-3.2.1
  ./autogen.sh
  ./configure
  make
  make install
  cd $TMPDIR
  ln -sf /usr/local/bin/htop /usr/bin/htop	
fi
ln -sf /usr/bin/bpytop /usr/bin/pytop

## Install Replacement Logger
apt-get remove --purge -y rsyslog
apt-get install -y --no-upgrade busybox-syslogd
echo "Run command 'logread' to check system logs"
dpkg --purge rsyslog
rm -f /var/log/messages
rm -f /var/log/syslog

## System Configuration
cp $TMPDIR/sysctl.conf /etc
chmod 644 /etc/sysctl.conf
chown root:root /etc/sysctl.conf

## Menu Layouts
rm -rf /opt/system
mkdir -p /opt/system
cp -r $TMPDIR/menus/*.txt /opt/system/
if [ ! -e "$WWWPATH/ram/mainmenu.txt" ]; then
  cp -r /opt/system/*menu.txt $WWWPATH/ram/
  chmod -R 777 $WWWPATH/ram/*menu.txt
  chown -R www-data:www-data $WWWPATH/ram/*menu.txt
fi

## Install Arduino Support
if [ ! -e "/usr/bin/arduino-cli" ]; then
  curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=/usr/bin sh
  arduino-cli version
  arduino-cli core update-index
  arduino-cli core install arduino:avr
  arduino-cli lib install Encoder@1.4.4
fi

## Arduino Firmware
rm -rf /opt/AutomateHub_1
mkdir -p /opt/AutomateHub_1
cp -r $TMPDIR/firmware/AutomateHub_1/* /opt/AutomateHub_1/
chmod -R 644 /opt/AutomateHub_1/*
chown -R root:root /opt/AutomateHub_1/*
cp -f $TMPDIR/update-fw.sh /usr/bin/update-fw
chmod 755 /usr/bin/update-fw
chown root:root /usr/bin/update-fw

## Xmit Serial COM
rm -f /usr/bin/ztermcom
gcc $TMPDIR/ztermcom.c -o /usr/bin/ztermcom
chmod 755 /usr/bin/ztermcom 
chown root:root /usr/bin/ztermcom
usermod -a -G dialout www-data
usermod -a -G tty www-data

## Node.JS API Server
mkdir -p /opt/nodeapi
cp -r $TMPDIR/nodeapi/* /opt/nodeapi/
mv -f /opt/nodeapi/nodeapi.service /etc/systemd/system/
chmod 644 /etc/systemd/system/nodeapi.service
chown root:root /etc/systemd/system/nodeapi.service
cd /opt/nodeapi
npm install
cd $TMPDIR

## Light Web Server
apt-get install -y --no-upgrade lighttpd
cp -f $TMPDIR/lighttpd.conf /etc/lighttpd/
chmod 644 /etc/lighttpd/lighttpd.conf
chown root:root /etc/lighttpd/lighttpd.conf
cp -f $TMPDIR/lighttpd.service /lib/systemd/system/
chmod 644 /lib/systemd/system/lighttpd.service 
chown root:root /lib/systemd/system/lighttpd.service

## SSL Certificates
cp -f $TMPDIR/ssl_cert.pem /etc/lighttpd/
chmod 644 /etc/lighttpd/ssl_cert.pem
chown root:root /etc/lighttpd/ssl_cert.pem
cp -f $TMPDIR/root_ca.crt /etc/lighttpd/
chmod 644 /etc/lighttpd/root_ca.crt
chown root:root /etc/lighttpd/root_ca.crt

## Base website files
mkdir -p $WWWPATH
chown -R www-data:www-data $TMPDIR/html
rsync -a $TMPDIR/html/ $WWWPATH/
chmod g+rx $WWWPATH
chown www-data:www-data $WWWPATH
mkdir -p /var/www/sessions
chmod -R g+rx /var/www/sessions
chown -R www-data:www-data /var/www/sessions
mkdir -p /var/www/uploads
chmod -R g+rx /var/www/uploads
chown -R www-data:www-data /var/www/uploads

## Minify JS/CSS
uglifyjs --warn --compress --output $WWWPATH/main.min.js -- $WWWPATH/main.js
yui-compressor $WWWPATH/main.css > $WWWPATH/main.min.css
cp -f $WWWPATH/main.min.js $WWWPATH/main.js
cp -f $WWWPATH/main.min.css $WWWPATH/main.css
chown www-data:www-data $WWWPATH/main.css
chmod 644 $WWWPATH/main.css
chown www-data:www-data $WWWPATH/main.js
chmod 644 $WWWPATH/main.js
rm -f $WWWPATH/main.min.css
rm -f $WWWPATH/main.min.js

## Boot Service
cp -f $TMPDIR/rc.local /etc/
chmod 755 /etc/rc.local
chown root:root /etc/rc.local
cp -f $TMPDIR/rc-local.service /etc/systemd/system/
chmod 644 /etc/systemd/system/rc-local.service
chown root:root /etc/systemd/system/rc-local.service
systemctl enable rc-local

## Disable Root Password
passwd -d root

## Auto SSH Login Key for root
mkdir -p /root/.ssh
cp -f $TMPDIR/authorized_keys /root/.ssh/

## SSH Configuration
cp -f $TMPDIR/sshd_config /etc/ssh/
chmod 644 /etc/ssh/sshd_config
chown root:root /etc/ssh/sshd_config

## Profile Configuration
cp -f $TMPDIR/profile /root/.profile
chmod 755 /root/.profile
chown root:root /root/.profile
touch /root/.hushlogin
chmod 644 /root/.hushlogin
chown root:root /root/.hushlogin

## Set Permissions
chmod -R 755 /opt/system
chown -R root:root /opt/system

## Clean-up
rm -f /etc/sudoers.d/www-perms
rm -f /etc/sudoers.d/www-nopasswd
rm -f /etc/sudoers.d/www-mod-nopasswd
rm -f /etc/sudoers.d/nobody-perms
systemctl daemon-reload
rm -rf $TMPDIR
apt-get autoremove -y
apt-get clean -y
apt-get autoclean -y

exit
