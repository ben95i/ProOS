#!/bin/bash
### ONLY RUN ON THE SERVER!!
### AutoConfig - ProOS for Automate VM
### by Ben Provenzano III

USER1_NAME="tank"
USER1_HOME="/home/$USER1_NAME"
USER1_UID=1000
GROUP_NAME="shared"
GROUP_GID=1000
WWWPATH="/var/www/html"
WWW_NAME="www-data"
WWW_GROUP="www-data"
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

## Create the shared group 
getent group "$GROUP_NAME" >/dev/null 2>&1 || 
  groupadd -g "$GROUP_GID" "$GROUP_NAME"

## Create the user with no shell
id -u "$USER1_NAME" >/dev/null 2>&1 || 
  useradd -u "$USER1_UID" -g "$GROUP_GID" --home "$USER1_HOME" -s /sbin/nologin "$USER1_NAME"

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

## Allow Ping Raw Sockets
setcap cap_net_raw+p /usr/bin/ping

## Node.JS API Server
cp -f $TMPDIR/nodeapi.service /etc/systemd/system/
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
chown -R "$WWW_NAME":"$WWW_GROUP" $TMPDIR/html
rsync -a $TMPDIR/html/ $WWWPATH/
mkdir -p /var/www/sessions
chmod -R g+rx /var/www/sessions
chown -R "$WWW_NAME":"$WWW_GROUP" /var/www/sessions
mkdir -p /var/www/uploads
chmod -R g+rx /var/www/uploads
chown -R "$WWW_NAME":"$WWW_GROUP" /var/www/uploads

## Website permissions
rm -f /etc/sudoers.d/www-perms
sh -c "touch /etc/sudoers.d/www-perms"
sh -c "echo \"tank ALL=(root) NOPASSWD:/usr/bin/nmap\" >> /etc/sudoers.d/www-perms"
chown root:root /etc/sudoers.d/www-perms
chmod u=rwx,g=rx,o=rx /etc/sudoers.d/www-perms
chmod u=r,g=r,o= /etc/sudoers.d/www-perms

## Minify JS/CSS
MIN_BEFORE_TOTAL=0
MIN_AFTER_TOTAL=0
echo "Compressing website components."
printf '%-26s %10s %10s %10s\n' 'FILE' 'BEFORE KB' 'AFTER KB' 'SAVED KB'
printf -- '-----------------------------------------------------------\n'
for CSSFILE in "$WWWPATH"/styles/*.css; do
  [ -e "$CSSFILE" ] || continue
  MIN_SIZE_BEFORE=$(wc -c < "$CSSFILE")
  if yui-compressor "$CSSFILE" > "$CSSFILE.tmp" \
     && [ -s "$CSSFILE.tmp" ] \
     && [ "$(wc -c < "$CSSFILE.tmp")" -lt "$MIN_SIZE_BEFORE" ]; then
    mv -f "$CSSFILE.tmp" "$CSSFILE"
    chmod 644 "$CSSFILE"
    MIN_SIZE_AFTER=$(wc -c < "$CSSFILE")
    awk -v f="styles/${CSSFILE##*/}" -v b="$MIN_SIZE_BEFORE" -v a="$MIN_SIZE_AFTER" 'BEGIN {
      s = (b - a) / 1024
      if (s > -0.05 && s < 0.05) s = 0
      printf "%-26s %10.1f %10.1f %10.1f\n", f, b/1024, a/1024, s }'
  else
    if [ -s "$CSSFILE.tmp" ]; then
      MIN_NOTE='no change'
    else
      MIN_NOTE='failed'
    fi
    rm -f "$CSSFILE.tmp"
    MIN_SIZE_AFTER=$MIN_SIZE_BEFORE
    awk -v f="styles/${CSSFILE##*/}" -v b="$MIN_SIZE_BEFORE" -v n="$MIN_NOTE" 'BEGIN {
      printf "%-26s %10.1f %10.1f %10s\n", f, b/1024, b/1024, n }'
  fi
  MIN_BEFORE_TOTAL=$(( MIN_BEFORE_TOTAL + MIN_SIZE_BEFORE ))
  MIN_AFTER_TOTAL=$(( MIN_AFTER_TOTAL + MIN_SIZE_AFTER ))
done
for JSFILE in "$WWWPATH"/*.js; do
  [ -e "$JSFILE" ] || continue
  case "$JSFILE" in *.min.js) continue ;; esac
  MIN_SIZE_BEFORE=$(wc -c < "$JSFILE")
  if uglifyjs --warn --compress --output "$JSFILE.tmp" -- "$JSFILE" \
     && [ -s "$JSFILE.tmp" ] \
     && [ "$(wc -c < "$JSFILE.tmp")" -lt "$MIN_SIZE_BEFORE" ]; then
    mv -f "$JSFILE.tmp" "$JSFILE"
    chmod 644 "$JSFILE"
    MIN_SIZE_AFTER=$(wc -c < "$JSFILE")
    awk -v f="${JSFILE##*/}" -v b="$MIN_SIZE_BEFORE" -v a="$MIN_SIZE_AFTER" 'BEGIN {
      s = (b - a) / 1024
      if (s > -0.05 && s < 0.05) s = 0
      printf "%-26s %10.1f %10.1f %10.1f\n", f, b/1024, a/1024, s }'
  else
    if [ -s "$JSFILE.tmp" ]; then
      MIN_NOTE='no change'
    else
      MIN_NOTE='failed'
    fi
    rm -f "$JSFILE.tmp"
    MIN_SIZE_AFTER=$MIN_SIZE_BEFORE
    awk -v f="${JSFILE##*/}" -v b="$MIN_SIZE_BEFORE" -v n="$MIN_NOTE" 'BEGIN {
      printf "%-26s %10.1f %10.1f %10s\n", f, b/1024, b/1024, n }'
  fi
  MIN_BEFORE_TOTAL=$(( MIN_BEFORE_TOTAL + MIN_SIZE_BEFORE ))
  MIN_AFTER_TOTAL=$(( MIN_AFTER_TOTAL + MIN_SIZE_AFTER ))
done
printf -- '-----------------------------------------------------------\n'
awk -v b="$MIN_BEFORE_TOTAL" -v a="$MIN_AFTER_TOTAL" 'BEGIN {
  pct = (b > 0) ? (b - a) * 100 / b : 0
  s = (b - a) / 1024
  if (s > -0.05 && s < 0.05) s = 0
  printf "%-26s %10.1f %10.1f %10.1f   (%.1f%% smaller)\n", \
    "TOTAL", b/1024, a/1024, s, pct }'
chown -R "$WWW_NAME":"$WWW_GROUP" $WWWPATH/styles/*.css
chown -R "$WWW_NAME":"$WWW_GROUP" $WWWPATH/*.js

## Reload Services
systemctl daemon-reload
/etc/rc.local

## Clean-up
rm -rf $TMPDIR
apt-get autoremove -y
apt-get clean -y
apt-get autoclean -y

exit