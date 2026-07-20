by Ben Provenzano III

###############################################
* SAMBA Initial Setup *

## Set SMB Passwords (3)
smbpasswd -a ben
smbpasswd -a media

## Reset All Permissions 'Optional' (4)
## !!WILL FORCE ENTIRE NEW DISK BACKUP SNAPSHOT!!
chmod -Rv a-rwX /mnt/datastore
chmod -Rv u=rwX,g=rX,o=rX /mnt/datastore
chmod -Rv a-rwX /mnt/scratch/cameras
chmod -Rv u=rwX,g=rX,o=rX /mnt/scratch/cameras
chmod -Rv a-rwX /mnt/scratch/downloads
chmod -Rv u=rwX,g=rX,o=rX /mnt/scratch/downloads
chmod 777 /mnt/scratch/downloads

## Reset All Owners 'Optional' (5)
## !!WILL FORCE ENTIRE NEW DISK BACKUP SNAPSHOT!!
chown -Rv ben:shared /mnt/datastore/Data
chown -Rv ben:shared /mnt/datastore/Media
chown -Rv ben:shared /mnt/datastore/.Archive
chown -Rv ben:shared /mnt/scratch/downloads
chown -Rv cameras:shared /mnt/scratch/cameras
