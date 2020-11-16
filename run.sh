#!/usr/bin/env bash

sudo modprobe snd_virmidi

connect_midi() {
    virtual=`aconnect -l | grep "Virtual Raw" -m 1 | sed -e 's/^client \([0-9]*\).*/\1/'`
    synth=`aconnect -l | grep "Waldorf Kyra" -m 1 | sed -e 's/^client \([0-9]*\).*/\1/'`
    if [ -n "$virtual" ] && [ -n "$synth" ]; then
	aconnect ${virtual} ${synth}
    fi
}

while true; do
    make
    connect_midi
    ./midipush 0 3
    sleep 2
done
