#!/bin/bash

PORT=/dev/ttyACM0
BAUD=115200

while true; do
    if [ -e "$PORT" ]; then
        echo "Connecting to $PORT..."
        picocom -b $BAUD $PORT
        echo "Disconnected. Waiting..."
    else
        sleep 1
    fi
done

