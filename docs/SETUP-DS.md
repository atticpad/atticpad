# Nintendo DS and DSi — the Wi-Fi situation, honestly

The DS client is the one AtticPad platform where the network itself needs
setting up, because of the console's radio rather than anything in this
software. This page says what the constraint is, what the protocol does about
it, and how to run an access point the console can join.

## The constraint

An original DS or DS Lite has an 802.11b radio that speaks **open or WEP
only**. It cannot join a WPA or WPA2 network — which is to say it cannot join
your house. No software changes that. A 3DS or DSi running a DS-mode program
(from a flashcart, say) is in DS mode and has exactly the same limitation.

A **DSi**, or a 3DS running the client in DSi (TWL) mode, can use WPA2. The
same `.nds` detects which mode it booted in and tries WPA2 when it can, so a
DSi owner may simply join the normal network. Whether a given launcher puts
the console in DSi mode is the launcher's business; the client only reports
what it finds.

## What that means for security

WEP is broken and an open network has nothing at all, so on a DS the link
layer protects nothing. AtticPad's protocol carries its own authentication for
exactly this reason: a 6-digit PIN during a two-minute pairing window, a
session key derived from it, and a tag on every packet after the handshake.
`docs/PROTOCOL.md` §10 is the specification.

Stated plainly, as the design document does: this stops a housemate, not an
adversary. Someone in radio range who captures the pairing exchange can
brute-force a 6-digit PIN offline and recover the session key. On a WEP or
open network — which the DS requires — that someone is anyone in range. If
that matters to you, run the access point below as an **isolated** network
with no route to anything else, and turn it off when you are done.

## Running an access point for the DS

The simplest arrangement is a spare Wi-Fi card on the same PC that runs the
AtticPad server, brought up as a small 2.4 GHz WEP-40 network fenced off from
everything else. With NetworkManager the shape is:

```sh
# a hotspot on channel 1 (802.11b/g), sharing no route anywhere
nmcli connection add type wifi ifname wlan0 con-name ds-ap autoconnect no \
    ssid atticpad 802-11-wireless.mode ap 802-11-wireless.band bg \
    802-11-wireless.channel 1 ipv4.method shared ipv6.method ignore
# WEP-40: a ten-hex-digit key, open authentication
nmcli connection modify ds-ap 802-11-wireless-security.key-mgmt none \
    802-11-wireless-security.auth-alg open \
    802-11-wireless-security.wep-key-type key \
    802-11-wireless-security.wep-key0 0123456789
nmcli connection up ds-ap
```

`ipv4.method shared` gives the console a DHCP lease (10.42.0.x by default,
the PC at 10.42.0.1) and, unless you stop it, NAT to the rest of your network.
Stop it: drop forwarding on that interface, and accept only what the console
needs on input — DHCP, ICMP and **UDP 21100** for AtticPad:

```sh
iptables -I FORWARD 1 -i wlan0 -j DROP
iptables -I FORWARD 1 -o wlan0 -j DROP
iptables -N DSAP
iptables -A DSAP -p udp --dport 21100 -j ACCEPT
iptables -A DSAP -p udp --dport 67 -j ACCEPT
iptables -A DSAP -p udp --dport 68 -j ACCEPT
iptables -A DSAP -p icmp -j ACCEPT
iptables -A DSAP -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT
iptables -A DSAP -j DROP
iptables -I INPUT 1 -i wlan0 -j DSAP
```

Forgetting the UDP 21100 line is the classic mistake: the console associates,
gets an address, and every packet it sends to the server vanishes without a
trace on either side.

Two hardware notes. Modern Wi-Fi chipsets sometimes refuse to serve an
802.11b-only client at all, or drop WEP support in access-point mode; if the
DS sees the network but never associates, try another card before suspecting
anything else. And the key must be entered on the console in the Nintendo
Wi-Fi Connection settings (or in the client's own network picker) exactly as
ten hex digits.

Then, on the PC, run the AtticPad server as usual. It binds every LAN
interface, so 10.42.0.1 is already covered; type that address on the console
if automatic discovery does not find it.

## On the console

- **Saved settings first.** The client tries the connections stored in the
  console's Nintendo Wi-Fi Connection settings, the same ones DS games use.
  Save the access point there once and the client joins it on every launch.
- **Or pick a network.** If nothing is saved, or the saved one fails, the
  client scans and lists what it sees. An open network joins directly; a WEP
  (or, in DSi mode, WPA2) network asks for the key on the touch keyboard.
- **It remembers the network.** A network joined through the picker is saved
  on the SD card, key included, and is tried first on the next launch, so
  a console that uses WPA2 in DSi mode reconnects on its own. The key is
  stored in plain text in `atticpad/atticpad.cfg`, exactly as the console's
  own Wi-Fi settings store theirs: anyone holding the card can read it.
  Choosing another network replaces it; delete the file to forget it.
- **Enter the server address** on the keyboard if it is not discovered. The
  last address that worked is remembered on the SD card when there is one.
- **Self-test:** hold **L + R + START** as it launches, or press **SELECT** at
  any time. It runs the protocol conformance vectors on the console itself and
  is the first thing to check when something does not work.

## In an emulator

melonDS runs the client with working networking and needs no access point
at all. In its default configuration (indirect mode) it presents an open
network named `melonAP`; pick it from the client's network list. The emulated
console can reach the PC's real LAN address by unicast, but not by broadcast,
so type the server address rather than waiting for discovery.

## Status

Written before the first hardware run. `docs/SUPPORT-TIERS.md` records what
has actually been seen on a console and in melonDS; this page will be
corrected from those observations rather than the other way round.
