#!/bin/bash
## Runs every 25 seconds on Proxmox
################################

WRITE_TRAILER() {
  TRAILER=$(date)
  TRAILER+=" ("
  TRAILER+=$(hostname)
  TRAILER+=")"
  echo "$TRAILER"
  echo " "
}

## VM Log File
TRIGGERS_DIR="/mnt/ramdisk"
LOCK_FILE="$TRIGGERS_DIR/actiontrig.lock"

## Check for lock file
if [ -e "$LOCK_FILE" ]; then
  echo "process locked! exiting. (pve.home)"
  exit
fi

## Create lock file
touch "$LOCK_FILE"

### IMPORT ZFS ########################################   
if [ -e "$TRIGGERS_DIR/attach_bkps.txt" ]; then
  rm -f "$TRIGGERS_DIR/attach_bkps.txt"
  if [ ! -e "$TRIGGERS_DIR/drives.txt" ]; then
	  echo "drives file not found, exiting."
	else
    readarray -t ZFSPOOLS < $TRIGGERS_DIR/drives.txt
    for _POOL in "${ZFSPOOLS[@]}"; do
      POOL=$(echo $_POOL | sed -e 's/\r//g')
      if [ ! "$POOL" == "" ]; then
        if [ "$POOL" == "tank" ] || \
          [ "$POOL" == "datastore" ] || \
          [ "$POOL" == "rpool" ] ; then
          echo "invalid pool specified."
        else
          echo "importing ZFS backup volume $POOL."
          zpool import $POOL
          zfs load-key -L file://$TRIGGERS_DIR/pwd.txt $POOL/extbkp
          zfs mount $POOL/extbkp
          zpool status $POOL
        fi
        echo " "
      fi
    done
  fi
  zfs list
  echo " "
  if [ -e "$TRIGGERS_DIR/pwd.txt" ]; then
    shred -n 2 -z -u "$TRIGGERS_DIR/pwd.txt"
  fi
  WRITE_TRAILER
fi

### EXPORT ZFS ########################################
if [ -e "$TRIGGERS_DIR/detach_bkps.txt" ]; then
  rm -f "$TRIGGERS_DIR/detach_bkps.txt"
  if [ ! -e "$TRIGGERS_DIR/drives.txt" ]; then
	  echo "drives file not found, exiting."
  else
    readarray -t ZFSPOOLS < "$TRIGGERS_DIR/drives.txt"
    for _POOL in "${ZFSPOOLS[@]}"; do
      POOL=$(echo $_POOL | sed -e 's/\r//g')
      if [ ! "$POOL" == "" ]; then
        if [ "$POOL" == "tank" ] || \
          [ "$POOL" == "datastore" ] || \
          [ "$POOL" == "rpool" ] ; then
          echo "invalid pool specified."
        else
          echo "unmounting ZFS backup volume $POOL."
          zfs unmount /mnt/extbkps/$POOL
          zpool export $POOL
        fi
        echo " "
      fi  
    done 
  fi
  zfs unload-key -a
  zpool status
  zfs list
  WRITE_TRAILER
fi

## List ZFS Snapshots
if [ -e "$TRIGGERS_DIR/pve_listsnaps.txt" ]; then
  rm -f "$TRIGGERS_DIR/pve_listsnaps.txt"
  echo "Snapshots on ZFS pool (tank/datastore):"
  zfs list -t snapshot tank/datastore | grep -o '^\S*'
  WRITE_TRAILER
fi
### START VMs #########################################
#######################################################
if [ -e "$TRIGGERS_DIR/startxana.txt" ]; then
  rm -f "$TRIGGERS_DIR/startxana.txt"
  echo "starting xana..."
  qm start 105
  WRITE_TRAILER
fi
if [ -e "$TRIGGERS_DIR/stopxana.txt" ]; then
  rm -f "$TRIGGERS_DIR/stopxana.txt"
  echo "shutting down xana..."
  qm stop 105
  WRITE_TRAILER
fi
if [ -e "$TRIGGERS_DIR/restorexana.txt" ]; then
  rm -f "$TRIGGERS_DIR/restorexana.txt"
  echo "restoring xana..."
  qmrestore /opt/xana-restore-image.vma.zst \
    105 -force -storage scratch
  WRITE_TRAILER
fi
#######################################################
if [ -e "$TRIGGERS_DIR/startunifi.txt" ]; then
  rm -f "$TRIGGERS_DIR/startunifi.txt"
  echo "starting unifi..."
  qm start 107
  WRITE_TRAILER
fi
if [ -e "$TRIGGERS_DIR/stopunifi.txt" ]; then
  rm -f "$TRIGGERS_DIR/stopunifi.txt"
  echo "shutting down unifi..."
  qm stop 107
  WRITE_TRAILER
fi
#######################################################
if [ -e "$TRIGGERS_DIR/startlegacy.txt" ]; then
  rm -f "$TRIGGERS_DIR/startlegacy.txt"
  echo "starting legacy..."
  qm start 103
  WRITE_TRAILER
fi
if [ -e "$TRIGGERS_DIR/stoplegacy.txt" ]; then
  rm -f "$TRIGGERS_DIR/stoplegacy.txt"
  echo "shutting down legacy..."
  qm stop 103
  WRITE_TRAILER
fi
#######################################################
if [ -e "$TRIGGERS_DIR/rebootrouter.txt" ]; then
  rm -f "$TRIGGERS_DIR/rebootrouter.txt"
  echo "rebooting router, may take up to 5-minutes."
  qm reboot 100
  WRITE_TRAILER
fi
### Write Server Log ##################################   
if [ -e "$TRIGGERS_DIR/syslog.txt" ]; then
  rm -f "$TRIGGERS_DIR/syslog.txt"
  /usr/bin/sys-check
  WRITE_TRAILER
fi
  ###### Server VM Backup Script ######################
if [ -e "$TRIGGERS_DIR/pve_vmsbkp.txt" ]; then
  VM_CONFS="/mnt/datastore/data/ProOS"
  rm -f "$TRIGGERS_DIR/pve_vmsbkp.txt"
  ###
  ### Container Backups
  ###
  echo " "
  echo "Backing-up Files LXC 101..."
  vzdump 101 --mode snapshot --compress zstd --node pve --storage local \
   --maxfiles 1 --remove 1 --exclude-path /mnt --exclude-path /home/ben
  cp -v /etc/pve/lxc/101.conf $VM_CONFS/files/lxc.conf
  chmod 777 $VM_CONFS/files/lxc.conf
  ###
  echo " "
  echo "Backing-up Mgmt LXC 102..."
  vzdump 102 --mode snapshot --compress zstd --node pve --storage local \
   --maxfiles 1 --remove 1 --exclude-path /mnt
  cp -v /etc/pve/lxc/102.conf $VM_CONFS/mgmt/lxc.conf
  chmod 777 $VM_CONFS/mgmt/lxc.conf
  ### 
  echo " "
  echo "Backing-up Automate LXC 106..."
  vzdump 106 --mode snapshot --compress zstd --node pve --storage local \
   --maxfiles 1 --remove 1 --exclude-path /mnt --exclude-path /var/www/html
  cp -v /etc/pve/lxc/106.conf $VM_CONFS/automate/lxc.conf
  chmod 777 $VM_CONFS/automate/lxc.conf
  ###
  echo " "
  echo "Backing-up Media LXC 108..."
  vzdump 108 --mode snapshot --compress zstd --node pve --storage local \
   --maxfiles 1 --remove 1 --exclude-path /mnt
  cp -v /etc/pve/lxc/108.conf $VM_CONFS/media/lxc.conf
  chmod 777 $VM_CONFS/media/lxc.conf
  ###
  ### Virtual Machine Backups
  ###
  echo " "
  echo "Backing-up Unifi KVM 107..."
  vzdump 107 --mode snapshot --compress zstd --node pve --storage local \
    --maxfiles 1 --remove 1
  cp -v /etc/pve/qemu-server/107.conf $VM_CONFS/pve/vmbkps/vzdump-qemu-107.conf
  chmod 777 $VM_CONFS/pve/vmbkps/vzdump-qemu-107.conf
  ###
  echo " "
  echo "Backing-up Router KVM 100..."
  vzdump 100 --mode snapshot --compress zstd --node pve --storage local \
    --maxfiles 1 --remove 1 --exclude-path /mnt
  cp -v /etc/pve/qemu-server/100.conf $VM_CONFS/pve/vmbkps/vzdump-qemu-100.conf
  chmod 777 $VM_CONFS/pve/vmbkps/vzdump-qemu-100.conf
  ###
  echo " "
  echo "Backing-up Legacy KVM 103..."
  vzdump 103 --mode snapshot --compress zstd --node pve --storage local \
    --maxfiles 1 --remove 1 --exclude-path /mnt
  cp -v /etc/pve/qemu-server/103.conf $VM_CONFS/legacy/qemu.conf
  chmod 777 $VM_CONFS/legacy/qemu.conf
  ###  
  echo " "
  echo "Backing-up Xana KVM 105..."
  vzdump 105 --mode snapshot --compress zstd --node pve --storage local \
    --maxfiles 1 --remove 1 --exclude-path /mnt
  cp -v /etc/pve/qemu-server/105.conf $VM_CONFS/xana/qemu.conf
  chmod 777 $VM_CONFS/xana/qemu.conf
  ###
  ### Copy to ZFS Pool
  ###
  echo " "
  echo "Copying VM's to Datastore..."
  rsync --progress -a /var/lib/vz/dump/vzdump-* /mnt/datastore/data/ProOS/pve/vmbkps/
  chmod -R 777 /mnt/datastore/data/ProOS/pve/vmbkps/*.vma.zst
  chmod -R 777 /mnt/datastore/data/ProOS/pve/vmbkps/*.tar.zst
  chmod -R 777 /mnt/datastore/data/ProOS/pve/vmbkps/*.conf
  chmod -R 777 /mnt/datastore/data/ProOS/pve/vmbkps/*.log
  echo "Backup Complete."
  WRITE_TRAILER
fi

rm -f "$LOCK_FILE"
exit
