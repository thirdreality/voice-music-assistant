#! /bin/sh
#
# System-V init script for the openntp daemon
#

PATH=/usr/local/sbin:/usr/local/bin:/sbin:/bin:/usr/sbin:/usr/bin
DESC="network time protocol daemon"
NAME=ntpd
DAEMON=/usr/sbin/$NAME
NTPDATE_BIN=/usr/bin/ntpdate

# Gracefully exit if the package has been removed.
test -x $DAEMON || exit 0

# Read config file if it is present.
if [ -r /etc/default/$NAME ]; then
    . /etc/default/$NAME
fi

if [ -x $NTPDATE_BIN ] ; then
    while ! iw wlan0 info 2>/dev/null | awk '$1 == "ssid" { found=1 } END { exit !found }'; do
        sleep 1
    done

    MAX_RETRIES=5
    attempt=0
    ntp_synced=false
    while [ $attempt -lt $MAX_RETRIES ]; do
        # Re-read config each iteration so DHCP Option 42 updates take effect
        [ -r /etc/default/$NAME ] && . /etc/default/$NAME
        # DHCP-provided servers first (local, low latency), then hardcoded IPs.
        # NTPSERVERS_DHCP may be empty; that's fine, ntpdate just skips it.
        $NTPDATE_BIN -b $NTPDATE_OPTS $NTPSERVERS_DHCP $NTPSERVERS_IP > /dev/null 2>&1 && { ntp_synced=true; break; }
        # Fall back to domain name
        $NTPDATE_BIN -b $NTPDATE_OPTS $NTPSERVERS_DNS > /dev/null 2>&1 && { ntp_synced=true; break; }
        killall -9 ntpd > /dev/null 2>&1
        attempt=$((attempt + 1))
        sleep 1
    done

    if [ "$ntp_synced" = true ]; then
        echo "ntpdate OK"
    else
        echo "ntpdate FAILED after $MAX_RETRIES attempts, starting services anyway"
    fi

    if [ ! -f /data/first_wifi_connected ]; then
        touch /data/first_wifi_connected
        paplay /usr/share/thirdreality/audio/ready_to_connect_ha.wav &
    fi
    /etc/init.d/S44bluetooth stop
    /etc/init.d/S99ha-speaker voice-assistant start

    #If the platform have RTC, we will write back to RTC HW
    if [ -e /dev/rtc ] || [ -e /dev/rtc0 ] || [ -e /dev/misc/rtc ]; then
        hwclock -w -u
    fi
fi

echo -n "Starting $DESC: $NAME"
start-stop-daemon -S -q -x $DAEMON

exit 0
