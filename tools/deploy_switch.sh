#!/usr/bin/env bash
# One-command deploy to THIS Switch. Thin wrapper over build_deploy_switch_lan.sh that
# pins the LAN IP your router hands the console (DHCP reservation → always 192.168.2.54),
# so you never have to type it. Override on the rare occasion the IP changes:
#   ./tools/deploy_switch.sh                 # uses the default below
#   ./tools/deploy_switch.sh 192.168.2.77    # one-off override
#   SWITCH_IP=192.168.2.77 ./tools/deploy_switch.sh
#
# Switch must be sitting on the homebrew menu (netloader listening) before you run this.

set -e

# Default Switch IP (DHCP-reserved on the WLAN). Arg 1 wins, then $SWITCH_IP, then this.
DEFAULT_SWITCH_IP="192.168.2.54"
SWITCH_IP="${1:-${SWITCH_IP:-$DEFAULT_SWITCH_IP}}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$SCRIPT_DIR/build_deploy_switch_lan.sh" "$SWITCH_IP"
