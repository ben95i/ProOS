#!/bin/bash
### ONLY RUN ON THE SERVER!!
### AutoConfig - ProOS for Automate VM
### by Ben Provenzano III

WWWPATH="/var/www/html"
TMPDIR="/tmp/config"
cd $TMPDIR

## Update Sources
apt-get --yes update

## Install Packages
apt-get install -y --no-upgrade --ignore-missing dirmngr ca-certificates bpytop \
 ethtool libdbus-glib-1-dev locales aptitude sudo gnupg scrub binutils nmap pip \
 apt-transport-https wget unzip gnupg rsync curl screen parallel libdbus-1-dev htop \
 autotools-dev autoconf automake build-essential libncursesw5-dev fastfetch \
 avahi-daemon ffmpeg python3-ament-xmllint etherwake wakeonlan npm nodejs 

## Remove Packages
apt-get remove -y --purge cron anacron postfix apache2 apache2-data

## CSS Minifier
mkdir -p /usr/lib/jvm/java-8-openjdk-amd64
apt-get install -y --no-upgrade --ignore-missing yui-compressor default-jre-headless
ln -sf /usr/lib/jvm/default-java /usr/lib/jvm/java-8-openjdk-amd64/jre
if [ ! -e "/usr/local/bin/uglifyjs" ]; then
  ## Minify JS
  npm install uglify-js -g
fi

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

## Node.JS API Server
mv -f /opt/nodeapi/nodeapi.service /etc/systemd/system/
chmod 644 /etc/systemd/system/nodeapi.service
chown root:root /etc/systemd/system/nodeapi.service
mkdir -p /opt/nodeapi
cp -r $TMPDIR/nodeapi/* /opt/nodeapi/
chown -R root:root /opt/nodeapi
cd /opt/nodeapi
npm install
npm audit fix
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

## Website permissions
rm -f /etc/sudoers.d/www-perms
sh -c "touch /etc/sudoers.d/www-perms"
sh -c "echo \"www-data ALL=(root) NOPASSWD:/usr/bin/nmap\" >> /etc/sudoers.d/www-perms"
chown root:root /etc/sudoers.d/www-perms
chmod u=rwx,g=rx,o=rx /etc/sudoers.d/www-perms
chmod u=r,g=r,o= /etc/sudoers.d/www-perms

## Minify JS/CSS
yui-compressor $WWWPATH/styles.css > $WWWPATH/styles.min.css
uglifyjs --warn --compress --output $WWWPATH/utils.min.js -- $WWWPATH/utils.js
uglifyjs --warn --compress --output $WWWPATH/main.min.js -- $WWWPATH/main.js
mv -f $WWWPATH/styles.min.css $WWWPATH/styles.css
mv -f $WWWPATH/utils.min.js $WWWPATH/utils.js
mv -f $WWWPATH/main.min.js $WWWPATH/main.js
chmod 644 $WWWPATH/styles.css
chown 644 $WWWPATH/utils.js
chmod 644 $WWWPATH/main.js
chown www-data:www-data $WWWPATH/styles.css
chown www-data:www-data $WWWPATH/utils.js
chown www-data:www-data $WWWPATH/main.js

## Boot Service
cp -f $TMPDIR/rc.local /etc/
chmod +x /etc/rc.local
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
chmod +x /root/.profile
chown root:root /root/.profile
touch /root/.hushlogin
chmod 644 /root/.hushlogin
chown root:root /root/.hushlogin

## Reload Services
systemctl daemon-reload
/etc/rc.local

## Clean-up
rm -rf $TMPDIR
apt-get autoremove -y
apt-get clean -y
apt-get autoclean -y

exit