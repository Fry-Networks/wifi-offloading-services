#!/bin/sh

package_path="$1"

if [ -z "$package_path" ]; then
    echo "-1"
    exit 1
fi

# Write a temporary upgrade script
cat > /tmp/do_upgrade.sh << EOF
#!/bin/sh
sleep 5  # Give the parent process time to exit

# Remove old package
opkg remove fry-os-services

# Clean up old configuration files
rm -f /etc/config/fry-os-services
rm -f /etc/config/fry-os-services-opkg
rm -f /etc/config/fry-agent
rm -f /etc/config/fry-config
rm -f /etc/config/fry-collector

# Install new package
opkg install "$package_path"
EOF

chmod +x /tmp/do_upgrade.sh
/tmp/do_upgrade.sh > /tmp/upgrade.log 2>&1 &

echo "Upgrade initiated"
exit 0
