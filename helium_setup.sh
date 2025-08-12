#!/bin/bash
# helium_setup.sh
# ZK Secure Helium Wi-Fi Hotspot + Data-Only Miner Setup Script
# Author: Dr. Raziek K.
# Tested for Debian-based systems (ZerOS, Raspberry Pi OS, Ubuntu Server ARM64)
# DISCLAIMER: This script does NOT enable Proof-of-Coverage mining; it sets up a Data-Only Helium miner.
# chmod +x helium_setup.sh
# sudo ./helium_setup.sh --ssid MySSID --pass 'MyStrongPassword'
set -e

### ===== CONFIGURATION =====
SSID_DEFAULT="HeliumMapper"
PASS_DEFAULT="YourPasswordHere"
WIFI_IFACE="wlan0"
STATIC_IP="192.168.4.1"
DHCP_RANGE="192.168.4.2,192.168.4.20,255.255.255.0,24h"
INSTALL_MAPPER=false

### ===== FUNCTIONS =====
usage() {
    echo "Usage: sudo bash helium_setup.sh [--ssid SSID] [--pass PASSWORD] [--install-mapper]"
    exit 1
}

### ===== ARGUMENT PARSING =====
while [[ "$#" -gt 0 ]]; do
    case "$1" in
        --ssid) SSID_DEFAULT="$2"; shift ;;
        --pass) PASS_DEFAULT="$2"; shift ;;
        --install-mapper) INSTALL_MAPPER=true ;;
        *) usage ;;
    esac
    shift
done

echo "[INFO] Starting ZK Secure Helium Hotspot setup..."

### ===== UPDATE SYSTEM =====
echo "[STEP] Updating system..."
apt update && apt upgrade -y

### ===== INSTALL HOSTAPD & DNSMASQ =====
echo "[STEP] Installing Wi-Fi hotspot packages..."
apt install -y hostapd dnsmasq

systemctl unmask hostapd
systemctl enable hostapd

# Backup configs
[ -f /etc/dnsmasq.conf ] && mv /etc/dnsmasq.conf /etc/dnsmasq.conf.bak
[ -f /etc/dhcpcd.conf ] && cp /etc/dhcpcd.conf /etc/dhcpcd.conf.bak

# dnsmasq config
cat <<EOF > /etc/dnsmasq.conf
interface=${WIFI_IFACE}
dhcp-range=${DHCP_RANGE}
EOF

# dhcpcd static IP
cat <<EOF >> /etc/dhcpcd.conf
interface ${WIFI_IFACE}
    static ip_address=${STATIC_IP}/24
    nohook wpa_supplicant
EOF

# hostapd config
cat <<EOF > /etc/hostapd/hostapd.conf
interface=${WIFI_IFACE}
driver=nl80211
ssid=${SSID_DEFAULT}
hw_mode=g
channel=7
wmm_enabled=0
macaddr_acl=0
auth_algs=1
ignore_broadcast_ssid=0
wpa=2
wpa_passphrase=${PASS_DEFAULT}
wpa_key_mgmt=WPA-PSK
rsn_pairwise=CCMP
EOF

sed -i "s|^#DAEMON_CONF=.*|DAEMON_CONF=\"/etc/hostapd/hostapd.conf\"|" /etc/default/hostapd

### ===== RESTART SERVICES =====
echo "[STEP] Restarting network services..."
systemctl restart dhcpcd
systemctl restart dnsmasq
systemctl restart hostapd

### ===== INSTALL DOCKER =====
echo "[STEP] Installing Docker..."
apt install -y docker.io
usermod -aG docker $SUDO_USER || true

### ===== HELIUM DATA-ONLY MINER =====
echo "[STEP] Deploying Helium Data-Only miner container..."
docker rm -f helium-miner 2>/dev/null || true
docker run -d --restart=always --name helium-miner \
    --net host \
    quay.io/team-helium/miner:latest

### ===== OPTIONAL MAPPER =====
if [ "$INSTALL_MAPPER" = true ]; then
    echo "[STEP] Installing Helium mapper..."
    apt install -y python3-pip
    pip3 install helium-mapper
fi

### ===== COMPLETE =====
echo "[DONE] ZK Secure Helium Hotspot setup complete!"
echo "  Wi-Fi SSID: ${SSID_DEFAULT}"
echo "  Wi-Fi Password: ${PASS_DEFAULT}"
echo "  Hotspot IP: ${STATIC_IP}"
echo "  Helium Miner: Data-Only mode running in Docker"
echo "Reboot your system for all changes to fully apply."
