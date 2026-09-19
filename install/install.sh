#!/bin/bash
# PiFire (C) installer for Raspberry Pi OS Lite (Bookworm/Trixie). Run as root from the repo root
# after building:   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && sudo install/install.sh
set -euo pipefail
cd "$(dirname "$0")/.."

[ "$(id -u)" -eq 0 ] || { echo "run as root (sudo)"; exit 1; }
[ -x build/pifired ] || { echo "build/pifired not found - build first"; exit 1; }

echo "+ packages"
apt-get install -y --no-install-recommends libsqlite3-0 libmosquitto1 libcurl4 libsystemd0 \
	network-manager dnsmasq-base avahi-daemon bluez rfkill

echo "+ service user"
id pifire >/dev/null 2>&1 || useradd --system --home /var/lib/pifire --shell /usr/sbin/nologin pifire
for g in gpio i2c spi dialout netdev bluetooth; do getent group "$g" >/dev/null && usermod -aG "$g" pifire || true; done

echo "+ files"
install -m 755 build/pifired /usr/local/bin/pifired
install -m 755 install/pifire-boardcfg /usr/local/bin/pifire-boardcfg
install -d -o pifire -g pifire -m 750 /etc/pifire /var/lib/pifire /var/lib/pifire/cookfiles
install -d -m 755 /usr/share/pifire /usr/lib/pifire/controllers /usr/lib/pifire/probes
cp -r share/. /usr/share/pifire/
install -m 644 install/pifired.service /etc/systemd/system/pifired.service
install -m 644 install/99-pifire.rules /etc/udev/rules.d/99-pifire.rules
install -d /etc/NetworkManager/dnsmasq-shared.d
install -m 644 install/pifire-captive.conf /etc/NetworkManager/dnsmasq-shared.d/pifire-captive.conf
cat > /etc/sudoers.d/pifire <<'EOF'
pifire ALL=(root) NOPASSWD: /usr/local/bin/pifire-boardcfg, /usr/bin/systemctl reboot, /usr/bin/systemctl poweroff, /usr/bin/rfkill unblock bluetooth
EOF
chmod 440 /etc/sudoers.d/pifire

echo "+ system"
rfkill unblock bluetooth 2>/dev/null || true
hostnamectl set-hostname pifire 2>/dev/null || true
grep -q '^127.0.1.1' /etc/hosts && sed -i 's/^127.0.1.1.*/127.0.1.1\tpifire/' /etc/hosts || echo -e '127.0.1.1\tpifire' >> /etc/hosts
systemctl enable --now avahi-daemon NetworkManager >/dev/null 2>&1 || true
udevadm control --reload-rules && udevadm trigger --subsystem-match=pwm || true

# baseline boot config: I2C, SPI, hardware watchdog. Pins/PWM/1-Wire are applied from the wizard.
/usr/local/bin/pifire-boardcfg --i2c --spi --watchdog >/tmp/pifire-boardcfg.out || true

echo "+ service"
systemctl daemon-reload
systemctl enable pifired
systemctl restart pifired
sleep 2
systemctl --no-pager --lines=5 status pifired || true

echo
echo "PiFire installed. Open http://pifire.local/ (or this Pi's IP address)."
grep -q REBOOT /tmp/pifire-boardcfg.out 2>/dev/null && echo "Boot configuration changed: reboot to enable I2C/SPI/watchdog."
exit 0
