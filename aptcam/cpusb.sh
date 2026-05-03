#!/bin/bash

## Copy Videos on USB Drive to Server
rsync -a --progress /media/usb0/cameras/Doorway/* /mnt/smb/Cameras/Doorway/
echo "videos can be found at '//files/Cameras/Doorway'"
read -p "delete videos from USB drive? (y/n) " -n 1 -r
echo
if [[ $REPLY =~ ^[yY]$ ]]
then
  rm -rfv /media/usb0/cameras/Doorway
  mkdir -p /media/usb0/cameras/Doorway
  chown -R motion:motion /media/usb0/cameras/Doorway
  chmod -R 777 /media/usb0/cameras/Doorway
fi
exit