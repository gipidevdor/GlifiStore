#!/bin/sh
# Become systemd as PID 1 inside the digest-pinned packaging container.
#
# The lifecycle's service-lifecycle row only PASSes when systemd is PID 1
# (/run/systemd/system). A one-shot entry script cannot claim that, so the
# outer dispatcher starts this script as the container command, waits for the
# runtime directory, then docker/podman execs linux-container-entry.sh.
#
# Mask units that fail noisily without a real machine; do not invent a PASS.
set -eu

if [ -x /lib/systemd/systemd ]; then
    SYSTEMD=/lib/systemd/systemd
elif [ -x /usr/lib/systemd/systemd ]; then
    SYSTEMD=/usr/lib/systemd/systemd
else
    if command -v apt-get >/dev/null 2>&1; then
        export DEBIAN_FRONTEND=noninteractive
        apt-get update
        apt-get install -y --no-install-recommends systemd systemd-sysv dbus
        SYSTEMD=/lib/systemd/systemd
    elif command -v dnf >/dev/null 2>&1; then
        dnf install -y systemd dbus
        SYSTEMD=/usr/lib/systemd/systemd
    else
        echo "error: cannot install systemd in this image" >&2
        exit 1
    fi
fi

[ -x "$SYSTEMD" ] || {
    echo "error: systemd binary missing after install: $SYSTEMD" >&2
    exit 1
}

mkdir -p /etc/systemd/system /run /tmp
# Mask before exec: systemctl is not available until systemd is PID 1.
for unit in \
    console-getty.service \
    getty@tty1.service \
    serial-getty@ttyS0.service \
    systemd-udevd.service \
    systemd-udevd-control.socket \
    systemd-udevd-kernel.socket \
    proc-sys-fs-binfmt_misc.automount; do
    ln -sfn /dev/null "/etc/systemd/system/$unit"
done

export container="${container:-docker}"
exec "$SYSTEMD"
