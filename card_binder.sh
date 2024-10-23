#!/bin/bash

if [ "$#" -ne 2 ]; then
    echo "Usage: $0 {intel|fchip} {bind|unbind}"
    exit 1
fi

DRIVER=$1
ACTION=$2

DEVICE_ID="0000:00:1f.3"

case $DRIVER in
    intel)
        DRIVER_PATH="snd_hda_intel"
        ;;
    fchip)
        DRIVER_PATH="filterchip"
        ;;
    *)
        echo "Invalid driver type. Use 'intel' or 'fchip'."
        exit 1
        ;;
esac

if [ "$ACTION" != "bind" ] && [ "$ACTION" != "unbind" ]; then
    echo "Invalid action. Use 'bind' or 'unbind'."
    exit 1
fi


echo "$DEVICE_ID" | sudo tee /sys/bus/pci/drivers/$DRIVER_PATH/$ACTION
