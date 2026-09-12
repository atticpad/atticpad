#!/bin/bash
# scripts/ds-ap-atticpad.sh -- let a DS on the fenced WEP access point reach
# this server.
#
#   scripts/ds-ap-atticpad.sh up      # allow UDP 21100 in from the AP
#   scripts/ds-ap-atticpad.sh down    # remove that allowance
#   scripts/ds-ap-atticpad.sh status  # is it in place?
#   scripts/ds-ap-atticpad.sh rules   # print the commands, run nothing
#
# The access point itself comes from a sibling project, gtacw-socialclub's
# scripts/ds-ap.sh: it brings a Wi-Fi card up as a WEP-40 network for a DS and
# fences it with an iptables chain (GTACW_AP) that accepts DHCP, ICMP and that
# stack's own ports, then DROPS everything else. Correctly so -- but it means
# a DS on that network can associate, take a lease, and then have every
# datagram it sends to this server silently dropped on the host. This script
# inserts one ACCEPT for UDP 21100 at the top of that chain and removes it
# again. It never edits the other project.
#
# docs/SETUP-DS.md describes the same firewall shape for a user building the
# access point from scratch; this is the developer's shortcut for the one that
# already exists on this machine.
set -eu

CHAIN="${DSAP_CHAIN:-GTACW_AP}"
PORT="${ATTICPAD_PORT:-21100}"
TAG="atticpad"

rule_add() { echo "iptables -I $CHAIN 1 -p udp --dport $PORT -m comment --comment $TAG -j ACCEPT"; }
rule_del() { echo "while iptables -D $CHAIN -p udp --dport $PORT -m comment --comment $TAG -j ACCEPT 2>/dev/null; do :; done"; }

# Everything root has to do goes through here as text, so it can be printed
# as well as run -- the same shape as ds-ap.sh, so the two read alike.
priv() {
    local script rc
    script="$(mktemp)"
    { echo "#!/bin/bash"; echo "set -u"; cat; } > "$script"
    chmod 0755 "$script"
    if [ "$(id -u)" = 0 ]; then
        /bin/bash "$script"; rc=$?
    elif sudo -n true 2>/dev/null; then
        sudo /bin/bash "$script"; rc=$?
    elif command -v pkexec >/dev/null 2>&1; then
        echo "asking for authorisation (a dialog may appear on your desktop)…" >&2
        pkexec /bin/bash "$script"; rc=$?
    else
        echo "this needs root; run these yourself:" >&2
        sed 1,2d "$script" >&2
        rc=1
    fi
    rm -f "$script"
    return $rc
}

# Reading the chain needs root too; without a cached sudo, fall back to the
# kernel's own view of the interface, which any user can read.
chain_exists() {
    if [ "$(id -u)" = 0 ]; then iptables -S "$CHAIN" >/dev/null 2>&1
    elif sudo -n true 2>/dev/null; then sudo -n iptables -S "$CHAIN" >/dev/null 2>&1
    else nmcli -t -f NAME,DEVICE connection show --active 2>/dev/null | grep -q "^gtacw-ds-ap:"; fi
}

case "${1:-status}" in
rules)
    rule_add
    ;;
up)
    if ! chain_exists; then
        echo "chain $CHAIN is not present: bring the access point up first" >&2
        echo "  (cd ~/code/gtacw-socialclub && scripts/ds-ap.sh up)" >&2
        exit 1
    fi
    { rule_del; echo "set -e"; rule_add; } | priv
    echo "UDP $PORT allowed in from the access point (chain $CHAIN)."
    echo "The server binds every LAN interface; on the console enter the AP's host address (10.42.0.1 by default)."
    ;;
down)
    rule_del | priv
    echo "UDP $PORT allowance removed from $CHAIN (if it was there)."
    ;;
status)
    if ! chain_exists; then
        echo "chain $CHAIN absent: the access point is down"
        exit 0
    fi
    if sudo -n true 2>/dev/null; then
        if sudo -n iptables -S "$CHAIN" 2>/dev/null | grep -q -- "--comment $TAG"; then
            echo "allowed: UDP $PORT in from the access point"
            sudo -n iptables -S "$CHAIN" | grep -- "--comment $TAG"
        else
            echo "not allowed: run '$0 up' before testing a DS"
        fi
    else
        echo "access point is up; cannot read the chain without root (sudo -n unavailable)"
    fi
    ;;
*)
    echo "usage: $0 {up|down|status|rules}" >&2
    exit 2
    ;;
esac
