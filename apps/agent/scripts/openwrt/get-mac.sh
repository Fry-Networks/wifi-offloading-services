#!/bin/sh

# Get model from environment variable
MODEL="${FRY_MODEL:-unknown}"

# Function to get MAC from a specific interface path
get_mac_from_path() {
    local mac_path="$1"
    local interface_name="$2"
    
    if [ -f "$mac_path" ]; then
        mac_address=$(cat "$mac_path" 2>/dev/null)
        if [ -n "$mac_address" ] && [ "$mac_address" != "" ]; then
            # Log to syslog which interface was used
            logger -t "get-mac.sh" "MAC retrieved from $interface_name: $mac_address"
            echo "$mac_address"
            return 0
        fi
    fi
    return 1
}

# Determine MAC address path based on device name
case "$MODEL" in
    "Kratos"|"Hemera"|"Artemisa"|"Atenea")
        # These devices use phy1-ap0 interface
        logger -t "get-mac.sh" "Device $MODEL detected, using phy1-ap0 interface"
        if get_mac_from_path "/sys/class/net/phy1-ap0/address" "phy1-ap0"; then
            exit 0
        fi
        # Fallback to eth0 if phy1-ap0 fails
        logger -t "get-mac.sh" "phy1-ap0 failed, trying eth0 fallback"
        if get_mac_from_path "/sys/class/net/eth0/address" "eth0"; then
            exit 0
        fi
        ;;
    "Loki"|"Gaia"|"Helios")
        # These devices use br-lan interface
        logger -t "get-mac.sh" "Device $MODEL detected, using br-lan interface"
        if get_mac_from_path "/sys/devices/virtual/net/br-lan/address" "br-lan"; then
            exit 0
        fi
        # Fallback to eth0 if br-lan fails
        logger -t "get-mac.sh" "br-lan failed, trying eth0 fallback"
        if get_mac_from_path "/sys/class/net/eth0/address" "eth0"; then
            exit 0
        fi
        ;;
    *)
        # Default behavior for unknown devices - try eth0 first (most reliable)
        logger -t "get-mac.sh" "Device $MODEL unknown or not specified, using eth0 (default)"
        if get_mac_from_path "/sys/class/net/eth0/address" "eth0"; then
            exit 0
        fi
        # Fallback to br-lan
        logger -t "get-mac.sh" "eth0 failed, trying br-lan fallback"
        if get_mac_from_path "/sys/devices/virtual/net/br-lan/address" "br-lan"; then
            exit 0
        fi
        ;;
esac

# If all attempts failed
logger -t "get-mac.sh" "ERROR: Failed to retrieve MAC address for device $MODEL"
echo "MAC_RETRIEVAL_FAILED"
exit 1
