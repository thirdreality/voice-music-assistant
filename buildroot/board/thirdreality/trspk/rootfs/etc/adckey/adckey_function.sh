#!/bin/sh

# set -x
SOUND_CONF="/data/conf/sound.json"
LOCK_FILE="/tmp/sound_config.lock"

do_volume_change() {
    action="$1"

    # LVA is unavailable: apply every press synchronously under the lock.
    # Shared debounce/count files can lose presses or reverse their direction.
    exec 200>"$LOCK_FILE"
    flock -x 200

    vol=$(jq '.volume' "$SOUND_CONF")

    step=10

    if [ "$action" = "up" ]; then
        vol=$((vol + step))
    elif [ "$action" = "down" ]; then
        vol=$((vol - step))
    fi

    if [ "$vol" -ge 100 ]; then
        vol=100
    elif [ "$vol" -le 0 ]; then
        vol=0
    fi

    echo "Volume $action: $vol%"

    tmpfile=$(mktemp)
    jq --argjson v "$vol" '.volume = $v' "$SOUND_CONF" > "$tmpfile" && mv "$tmpfile" "$SOUND_CONF"
    sync

    /usr/bin/tr-volume set "$vol" > /dev/null 2>&1

    flock -u 200
    exec 200>&-

    dbus-send --system --type=signal /com/3r/EventBus com._3reality.EventBus.LedShow boolean:false array:string:'/usr/share/thirdreality/animation/volume-changed.animation'
}

factory_reset() {
    if [ -f "/tmp/factory_reset" ];then
        rm -rf /tmp/factory_reset
        exit 0
    else
        touch /tmp/factory_reset
    fi
    paplay /usr/share/thirdreality/audio/factory_reset.wav &
    echo "factory resetting..."
    dbus-send --system --type=signal /com/3r/EventBus com._3reality.EventBus.LedShow boolean:false array:string:'/usr/share/thirdreality/animation/ntf_incoming.animation'

    sleep 3

    for i in $(seq 3); do
        killall5 -9
        sleep 1
    done

    rm -rf /data/*
    umount /data
    sync
    reboot
}

mic_mute() {
    exec 200>"$LOCK_FILE"
    flock -x 200

    status=$(cat /sys/class/gpio/gpio438/value)

    if [ $status = "1" ]; then
        status=0
        echo 0 > /sys/class/gpio/gpio438/value
    else
        status=1
        echo 1 > /sys/class/gpio/gpio438/value
    fi

    tmpfile=$(mktemp)
    jq --argjson v "$status" '.mic_mute = $v' "$SOUND_CONF" > "$tmpfile" && mv "$tmpfile" "$SOUND_CONF"
    sync

    flock -u 200
    exec 200>&-
}

set_mic_mute() {
    status="$1"
    [ "$status" = "0" ] || [ "$status" = "1" ] || exit 1

    exec 200>"$LOCK_FILE"
    flock -x 200

    echo "$status" > /sys/class/gpio/gpio438/value
    tmpfile=$(mktemp)
    jq --argjson v "$status" '.mic_mute = $v' "$SOUND_CONF" > "$tmpfile" && mv "$tmpfile" "$SOUND_CONF"
    sync

    flock -u 200
    exec 200>&-
}

set_volume() {
    vol="$1"
    [ -z "$vol" ] && exit 1

    if [ "$vol" -ge 100 ]; then
        vol=100
    elif [ "$vol" -le 0 ]; then
        vol=0
    fi

    exec 200>"$LOCK_FILE"
    flock -x 200

    tmpfile=$(mktemp)
    jq --argjson v "$vol" '.volume = $v' "$SOUND_CONF" > "$tmpfile" && mv "$tmpfile" "$SOUND_CONF"
    sync

    flock -u 200
    exec 200>&-

    /usr/bin/tr-volume set "$vol" > /dev/null 2>&1

    echo "SetVolume: $vol%"
    dbus-send --system --type=signal /com/3r/EventBus com._3reality.EventBus.LedShow boolean:false array:string:'/usr/share/thirdreality/animation/volume-changed.animation'
}

change_wifi() {
    if [ -f "/tmp/change_wifi" ];then
        rm -rf /tmp/change_wifi
        exit 0
    else
        touch /tmp/change_wifi
    fi

    dbus-send --system --type=signal /com/3r/EventBus com._3reality.EventBus.LedShow boolean:false array:string:'/usr/share/thirdreality/animation/ntf_incoming.animation'
    /etc/init.d/S44bluetooth restart
}

case $1 in
    "Volup") do_volume_change "up" ;;
    "Voldown") do_volume_change "down" ;;
    "SetVolume") set_volume "$2" ;;
    "SetMicMute") set_mic_mute "$2" ;;
    "Home") echo "home was pressed";;
    "longpressTap") change_wifi ;;
    "Mute") mic_mute ;;
    "longpressHome") factory_reset ;;
    *) echo "no function to add this case: $1" ;;
esac

exit
