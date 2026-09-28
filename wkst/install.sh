#!/bin/bash

# Check if the script is running as root
if [ "$EUID" -ne 0 ]; then
    echo "Error: This script must be run as root." >&2
    exit 1
fi
echo "Installing the configuration on wkst..."

mkdir -p /opt/scripts

tee /etc/systemd/system/nvidia-clocklock.service >/dev/null <<'EOF'
[Unit]
Description=Lock NVIDIA GPU clocks to reduce Wayland stutter
After=multi-user.target

[Service]
Type=oneshot
ExecStart=/usr/bin/nvidia-smi -pm 1
ExecStart=/usr/bin/nvidia-smi -lgc 1300,2130

[Install]
WantedBy=multi-user.target
EOF
chmod 664 /etc/systemd/system/nvidia-clocklock.service
systemctl enable --now nvidia-clocklock.service

mkdir -p /mnt/SFTP
mkdir -p /mnt/Data
mkdir -p /mnt/Media
mkdir -p /mnt/Scratch
mkdir -p /mnt/Cameras
mkdir -p /mnt/Regions
mkdir -p /mnt/Games
mkdir -p /mnt/MP3s
mkdir -p /mnt/External
tee /etc/fstab >/dev/null <<'EOF'
# /etc/fstab: static file system information
# Updated: 8/9/2026
# Use 'blkid' to print the universally unique identifier for a device; this may
# be used with UUID= as a more robust way to name devices that works even if
# disks are added and removed. See fstab(5).
#
# <file system>             <mount point>  <type>  <options>  <dump>  <pass>
#
UUID=AC46-2BA9                             /boot/efi      vfat   defaults   0  2
UUID=1b5d9265-2b47-440c-b698-586a7d2b6835  /              ext4   defaults   0  1
UUID=e3ce5d70-8f2d-4f98-a81e-e5c1b94900af  /mnt/Local     ext4   defaults   0  0
/swapfile                                  swap           swap   defaults   0  0
tmpfs                                      /tmp           tmpfs  defaults,noatime,mode=1777   0  0
ben@files.home:/       /mnt/SFTP      fuse.sshfs  noauto,x-systemd.automount,x-systemd.idle-timeout=120,x-systemd.mount-timeout=5,_netdev,nofail,ssh_command=/root/sshfs_wrapper.sh,default_permissions,allow_other,reconnect,ServerAliveInterval=15,ServerAliveCountMax=2,uid=1000,gid=1000   0  0
//files.home/Data      /mnt/Data      cifs        noauto,x-systemd.automount,x-systemd.idle-timeout=120,x-systemd.mount-timeout=5,_netdev,nofail,credentials=/root/smb.credentials,iocharset=utf8,vers=3.0,soft,echo_interval=5,handletimeout=5000,uid=1000,gid=1000   0  0
//files.home/Media     /mnt/Media     cifs        noauto,x-systemd.automount,x-systemd.idle-timeout=120,x-systemd.mount-timeout=5,_netdev,nofail,credentials=/root/smb.credentials,iocharset=utf8,vers=3.0,soft,echo_interval=5,handletimeout=5000,uid=1000,gid=1000   0  0
//files.home/Scratch   /mnt/Scratch   cifs        noauto,x-systemd.automount,x-systemd.idle-timeout=120,x-systemd.mount-timeout=5,_netdev,nofail,credentials=/root/smb.credentials,iocharset=utf8,vers=3.0,soft,echo_interval=5,handletimeout=5000,uid=1000,gid=1000   0  0
//files.home/Cameras   /mnt/Cameras   cifs        noauto,x-systemd.automount,x-systemd.idle-timeout=120,x-systemd.mount-timeout=5,_netdev,nofail,credentials=/root/smb.credentials,iocharset=utf8,vers=3.0,soft,echo_interval=5,handletimeout=5000,uid=1000,gid=1000   0  0
//files.home/Regions   /mnt/Regions   cifs        noauto,x-systemd.automount,x-systemd.idle-timeout=120,x-systemd.mount-timeout=5,_netdev,nofail,credentials=/root/smb.credentials,iocharset=utf8,vers=3.0,soft,echo_interval=5,handletimeout=5000,uid=1000,gid=1000   0  0
//files.home/Games     /mnt/Games     cifs        noauto,x-systemd.automount,x-systemd.idle-timeout=120,x-systemd.mount-timeout=5,_netdev,nofail,credentials=/root/smb.credentials,iocharset=utf8,vers=3.0,soft,echo_interval=5,handletimeout=5000,uid=1000,gid=1000   0  0
//files.home/MP3s      /mnt/MP3s      cifs        noauto,x-systemd.automount,x-systemd.idle-timeout=120,x-systemd.mount-timeout=5,_netdev,nofail,credentials=/root/smb.credentials,iocharset=utf8,vers=3.0,soft,echo_interval=5,handletimeout=5000,uid=1000,gid=1000   0  0
//files.home/External  /mnt/External  cifs        noauto,x-systemd.automount,x-systemd.idle-timeout=120,x-systemd.mount-timeout=5,_netdev,nofail,credentials=/root/smb.credentials,iocharset=utf8,vers=3.0,soft,echo_interval=5,handletimeout=5000,uid=1000,gid=1000   0  0
EOF
chmod 664 /etc/fstab

tee /root/sshfs_wrapper.sh >/dev/null <<'EOF'
#!/bin/bash
## install in /root/sshfs_wrapper.sh
# Define where your credentials file is located
CRED_FILE="/root/smb.credentials"
# Extract the password using awk (splits the line at '=' and prints the 2nd part)
PASSWORD=$(awk -F= '/^password=/{print $2}' "$CRED_FILE" | tr -d '\r\n')
# Export the password as an environment variable (securely read by sshpass)
export SSHPASS="$PASSWORD"
# Execute sshpass, telling it to use the environment variable (-e) and pass all other arguments to ssh
exec sshpass -e ssh -o StrictHostKeyChecking=no "$@"
EOF
chmod +x /root/sshfs_wrapper.sh

tee /home/ben/.bashrc >/dev/null <<'EOF'
#!/bin/bash
alias ll='ls -la'
alias mgmt='ssh ben@mgmt -i /home/ben/.ssh/mgmt_ben.rsa'
alias esp='source "/home/ben/.espressif/tools/activate_idf_v6.0.1.sh"; echo "switch to the projects directory, and then run:"; echo "idf.py -p /dev/ttyUSB0 flash"'
SPINNER=("|" "/" "-" "\\")
echo "Press any key to connect to the server."
RUN_MGMT=false
LOOPS=50
for (( I=LOOPS; I>=1; I-- )); do
    SEC=$(( (I + 9) / 10 ))
    SPIN_IDX=$(( (LOOPS - I) % 4 ))
    SPIN_CHAR=${SPINNER[$SPIN_IDX]}
    printf "\rOpening local shell in%2d seconds... %s " "$SEC" "$SPIN_CHAR"
    if read -r -t 0.1 -n 1 -s KEY; then
        RUN_MGMT=true
        break
    fi
done
if [ "$RUN_MGMT" = true ]; then
   printf "\rConnecting to server...\e[K\n"
    mgmt
else
    printf "\rOpening local shell...\e[K\n"
    timeout 30 bash -c fastfetch
fi
EOF
chmod +x /home/ben/.bashrc
chown ben:ben /home/ben/.bashrc

tee /opt/scripts/toggle_split_screen.sh >/dev/null <<'EOF'
#!/bin/bash
# Note: For many Dell monitors, PBP mode changes the 'mh' or 'sl' values.
# On the U3223QE, 'sl' is often the mode indicator (0x00=Single, 0x24=PBP).
SL_VAL=$(ddcutil --sleep-multiplier .5 getvcp E9 | sed -n 's/.*sl=\([^,]*\).*/\1/p')
if [ "$SL_VAL" == "0x00" ]; then
    echo "Status: Single (Full Screen)"
    ## Set to split screen
    ddcutil --sleep-multiplier .5 setvcp E9 0x24
elif [ "$SL_VAL" == "0x24" ] || [ "$SL_VAL" == "0x23" ]; then
    echo "Status: PBP (Split Screen)"
    ## Set to single input
    ddcutil --sleep-multiplier .5 setvcp E9 0x00
else
    echo "Status: Unknown Mode (sl=$SL_VAL)"
fi
exit
EOF
chmod +x /opt/scripts/toggle_split_screen.sh

echo "Complete, reboot system for changes to take effect."
exit