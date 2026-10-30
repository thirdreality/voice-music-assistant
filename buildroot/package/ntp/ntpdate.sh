#! /bin/sh
# Product policy/lifecycle path; other products retain their NTP behaviour.
POLICY_SCRIPT=/usr/share/thirdreality/script/network-policy.sh
NTP_APPLY=/usr/share/thirdreality/script/ntp-apply
if [ -r "$POLICY_SCRIPT" ] && [ -x "$NTP_APPLY" ]; then
    . "$POLICY_SCRIPT"
    exec 7>/tmp/ha_initial_ntp_phase.lock
    # A second invocation must not queue behind initial Wi-Fi/NTP waits.
    flock -n 7 || { echo 'Initial NTP phase already running'; exit 0; }
    finish_initial_ntp() {
        trap - EXIT HUP INT TERM
        touch /tmp/ha_initial_ntp_done
        "$NTP_APPLY"
    }
    # Only the first invocation performs initial sync. Runtime NTP restarts
    # cannot reset the completion gate or undo a user's voice stop.
    if [ -f /tmp/ha_initial_ntp_done ]; then
        exec "$NTP_APPLY"
    fi
    network_policy_init || exit 1
    wifi_wait=0
    while ! wifi_local_ready; do
        if [ "$wifi_wait" -ge 30 ]; then
            echo 'Wi-Fi not ready; initial NTP phase deferred until association and IP are available'
            exit 0
        fi
        wifi_wait=$((wifi_wait + 1))
        sleep 1
    done
    # Only a network-ready attempt can publish completion. A timeout while
    # provisioning leaves the phase pending; S49 retries in the background.
    trap finish_initial_ntp EXIT
    trap 'exit 1' HUP INT TERM
    if [ ! -x /usr/bin/ntpdate ] || [ ! -x /usr/sbin/ntpd ]; then
        echo 'NTP binaries unavailable; continuing without synchronized time'
        exit 0
    fi
    synced=0
    for attempt in 1 2 3 4 5; do
        (
            flock -x 9 || exit 1
            NTPSERVERS_DHCP='' NTPSERVERS_IP='' NTPSERVERS_DNS=''
            [ ! -r /etc/default/ntpd ] || . /etc/default/ntpd
            # Only this process owns UDP 123 during a burst.
            if pidof ntpd >/dev/null 2>&1; then
                stop_ntpd || exit 1
            fi
            if [ -n "$NTPSERVERS_DHCP" ]; then
                /usr/bin/ntpdate -b -4 -t 2 $NTPSERVERS_DHCP >/dev/null 2>&1 && exit 0
            fi
            if [ "$(network_policy_allow)" = 1 ]; then
                if [ -n "$NTPSERVERS_IP" ]; then
                    /usr/bin/ntpdate -b -4 -t 2 $NTPSERVERS_IP >/dev/null 2>&1 && exit 0
                fi
                # Re-read before each possible public request.
                if [ "$(network_policy_allow)" = 1 ] && [ -n "$NTPSERVERS_DNS" ]; then
                    /usr/bin/ntpdate -b -4 -t 2 $NTPSERVERS_DNS >/dev/null 2>&1 && exit 0
                fi
            fi
            exit 1
        ) 9>/tmp/ha_ntp.lock && { synced=1; break; }
        [ "$(network_policy_allow)" = 1 ] || [ -n "$(sed -n 's/^NTPSERVERS_DHCP="\(.*\)"/\1/p' /etc/default/ntpd)" ] || break
        sleep 1
    done
    if [ "$synced" = 1 ]; then
        echo 'ntpdate OK'
    else
        echo 'Initial NTP phase finished without synchronized time; continuing'
    fi
    exit 0
fi

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
    wifi_wait=0
    while ! iw wlan0 info 2>/dev/null | awk '$1 == "ssid" { found=1 } END { exit !found }'; do
        if [ "$wifi_wait" -ge 30 ]; then
            echo 'Wi-Fi association timed out; proceeding with bounded NTP retries'
            break
        fi
        wifi_wait=$((wifi_wait + 1))
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
