#!/bin/bash
# PiFire (C) installer for Raspberry Pi OS Lite (Bookworm/Trixie). Run as root.
#   from a source build:     cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && sudo install/install.sh
#   from a release archive:  tar -xzf pifire-<ver>-<arch>.tar.gz && cd pifire-<ver>-<arch> && sudo ./install/install.sh
#   --upgrade   reinstall files + restart only (used by the OTA updater; skips apt, users, hostname)
set -euo pipefail
cd "$(dirname "$0")/.."
UPGRADE=0
[ "${1:-}" = "--upgrade" ] && UPGRADE=1

[ "$(id -u)" -eq 0 ] || { echo "run as root (sudo)"; exit 1; }
BIN=""
for c in build/pifired ./pifired; do [ -x "$c" ] && { BIN="$c"; break; }; done
[ -n "$BIN" ] || { echo "pifired binary not found (build first, or run from an unpacked release)"; exit 1; }

if [ $UPGRADE -eq 0 ]; then
	echo "+ packages"
	apt-get install -y --no-install-recommends libsqlite3-0 libmosquitto1 libcurl4 libsystemd0 \
		network-manager dnsmasq-base avahi-daemon bluez rfkill smbclient

	echo "+ service user"
	getent group pifire >/dev/null || groupadd --system pifire
	id pifire >/dev/null 2>&1 || useradd --system --gid pifire --home /var/lib/pifire --shell /usr/sbin/nologin pifire
	for g in gpio i2c spi dialout netdev bluetooth; do getent group "$g" >/dev/null && usermod -aG "$g" pifire || true; done

	# an existing Python PiFire (supervisor + nginx on port 80) must not run alongside: it would fight over
	# the web port and the relay GPIOs. It is stopped and disabled, not removed.
	if [ -f /etc/supervisor/conf.d/control.conf ] || [ -f /etc/nginx/sites-enabled/pifire ]; then
		echo "+ stopping the Python PiFire services (supervisor, nginx)"
		systemctl disable --now supervisor nginx >/dev/null 2>&1 || true
		pkill -f 'python.*control.py' 2>/dev/null || true
	fi
fi

echo "+ files"
install -m 755 "$BIN" /usr/local/bin/pifired.new && mv -f /usr/local/bin/pifired.new /usr/local/bin/pifired
install -m 755 install/pifire-boardcfg /usr/local/bin/pifire-boardcfg
install -m 755 install/pifire-update-apply /usr/local/bin/pifire-update-apply
install -m 755 install/pifire-tailscale /usr/local/bin/pifire-tailscale
install -d -o pifire -g pifire -m 750 /etc/pifire /var/lib/pifire /var/lib/pifire/cookfiles /var/lib/pifire/update
install -d -m 755 /usr/share/pifire /usr/lib/pifire/controllers /usr/lib/pifire/probes /usr/lib/pifire/display
cp -r share/. /usr/share/pifire/
install -m 644 install/pifired.service /etc/systemd/system/pifired.service
install -m 644 install/99-pifire.rules /etc/udev/rules.d/99-pifire.rules
install -d /etc/NetworkManager/dnsmasq-shared.d
install -m 644 install/pifire-captive.conf /etc/NetworkManager/dnsmasq-shared.d/pifire-captive.conf
cat > /etc/sudoers.d/pifire <<'SUDO'
pifire ALL=(root) NOPASSWD: /usr/local/bin/pifire-boardcfg, /usr/local/bin/pifire-update-apply, /usr/local/bin/pifire-tailscale, /usr/bin/systemctl reboot, /usr/bin/systemctl poweroff, /usr/bin/rfkill unblock bluetooth
SUDO
chmod 440 /etc/sudoers.d/pifire

if [ $UPGRADE -eq 0 ]; then
	echo "+ system"
	rfkill unblock bluetooth 2>/dev/null || true
	hostnamectl set-hostname pifire 2>/dev/null || true
	grep -q '^127.0.1.1' /etc/hosts && sed -i 's/^127.0.1.1.*/127.0.1.1\tpifire/' /etc/hosts || echo -e '127.0.1.1\tpifire' >> /etc/hosts
	systemctl enable --now avahi-daemon NetworkManager >/dev/null 2>&1 || true
	udevadm control --reload-rules && udevadm trigger --subsystem-match=pwm || true
	# baseline boot config: I2C, SPI, hardware watchdog. Pins/PWM/1-Wire are applied from the wizard.
	/usr/local/bin/pifire-boardcfg --i2c --spi --watchdog >/tmp/pifire-boardcfg.out || true
fi

echo "+ service"
systemctl daemon-reload
systemctl enable pifired >/dev/null 2>&1 || true
systemctl restart pifired
sleep 2
systemctl --no-pager --lines=3 status pifired || true

echo
if [ $UPGRADE -eq 1 ]; then
	# backups to a network share go through smbclient, which older installs did not have
	if ! command -v smbclient >/dev/null 2>&1; then
		echo "+ smbclient (for backups to a network share)"
		apt-get install -y --no-install-recommends smbclient >/dev/null 2>&1 || echo "  could not install smbclient now; backups to a share need it (sudo apt install smbclient)"
	fi
	echo "PiFire upgraded to $(cat VERSION 2>/dev/null || echo '?')."
else
	echo "PiFire installed. Open http://pifire.local/ (or this Pi's IP address)."
	grep -q REBOOT /tmp/pifire-boardcfg.out 2>/dev/null && echo "Boot configuration changed: reboot to enable I2C/SPI/watchdog."
fi
exit 0
