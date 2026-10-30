#!/bin/sh
# Shared boot-time policy reader. /data is mounted before DHCP/S49 start.
# POLICY_ROOT is used by isolated host tests; production leaves it empty.
NETWORK_CONF="${POLICY_ROOT}/data/conf/network.json"
NETWORK_INIT="${POLICY_ROOT}/data/conf/network.initialized"
NETWORK_LOCK="${POLICY_ROOT}/tmp/ha_network_policy.lock"

network_policy_init() (
    mkdir -p "${POLICY_ROOT}/data/conf" "${POLICY_ROOT}/tmp" || exit 1
    flock -x 9 || exit 1
    if [ ! -e "$NETWORK_CONF" ] && [ ! -e "$NETWORK_INIT" ]; then
        # Explicit default for both upgrades and factory images.
        printf '%s\n' '{"allow_background_internet":true}' > "$NETWORK_CONF.tmp" &&
            mv "$NETWORK_CONF.tmp" "$NETWORK_CONF" || exit 1
    fi
    # A missing file after initialization must fail closed.
    if [ ! -e "$NETWORK_INIT" ]; then
        touch "$NETWORK_INIT" || exit 1
        sync
    fi
) 9>"$NETWORK_LOCK"

network_policy_allow() {
    network_policy_init || { echo 0; return; }
    jq -er 'if .allow_background_internet == true then 1 else 0 end' \
        "$NETWORK_CONF" 2>/dev/null || echo 0
}

wifi_local_ready() {
    iw dev wlan0 link 2>/dev/null | grep -q 'Connected to ' &&
        ip -o -4 addr show dev wlan0 scope global 2>/dev/null | grep -q 'inet '
}

# Some deployed ntpd processes keep SIGTERM blocked. Confirm termination
# before claiming public time queries have stopped, then bound the fallback.
stop_ntpd() {
    pidof ntpd >/dev/null 2>&1 || return 0
    start-stop-daemon -K -q -n ntpd 2>/dev/null || :
    for wait_step in 1 2 3; do
        pidof ntpd >/dev/null 2>&1 || return 0
        sleep 1
    done
    start-stop-daemon -K -q -s KILL -n ntpd 2>/dev/null || :
    for wait_step in 1 2; do
        pidof ntpd >/dev/null 2>&1 || return 0
        sleep 1
    done
    echo 'Could not stop ntpd' >&2
    return 1
}

# Never invent a DNS server from a gateway address or a public fallback.
# The DHCP script owns resolv.conf and snapshots its current lease DNS.
ensure_dns_config() (
    flock -x 8 || exit 1
    resolv="${POLICY_ROOT}/etc/resolv.conf"
    grep -q '^[[:space:]]*nameserver[[:space:]]' "$resolv" 2>/dev/null && exit 0
    lease="${POLICY_ROOT}/tmp/ha_dhcp_dns"
    if [ -s "$lease" ]; then
        # Write through the file: resolv.conf can be a symlink into /tmp.
        cat "$lease" > "$resolv"
    else
        echo 'No DHCP DNS server available; leaving DNS unconfigured' >&2
    fi
) 8>"${POLICY_ROOT}/tmp/ha_dns.lock"
