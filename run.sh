#!/usr/bin/env bash

sudo modprobe snd_virmidi

while true; do
    make
    ./midipush
    sleep 2
done
