#!/usr/bin/env bash

set -euo pipefail

VIRTUAL_NAME="Virtual Raw"
SYNTH_NAME="Waldorf Kyra"

sudo modprobe snd_virmidi

get_midi_port() {
    aconnect -l | grep "$1" -m 1 | sed -e 's/^client \([0-9]*\).*/\1/'
}

connect_midi() {
    virtual=$(get_midi_port "${VIRTUAL_NAME}")
    synth=$(get_midi_port "${SYNTH_NAME}")
    if [ -n "$virtual" ] && [ -n "$synth" ]; then
        aconnect -x
	aconnect ${virtual} ${synth}
        return 0
    else
        return -1
    fi
}

mkdir -p record

while true; do
    if connect_midi; then
        make
        ./midipush 1 5 && true
        if [ $? == 40 ]; then
            sleep 15
            sudo poweroff
        fi
    fi
    sleep 5
done
