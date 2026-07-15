#!/usr/bin/env bash
set -euo pipefail

QUADLET_DST="${HOME}/.config/containers/systemd"

for unit in \
    openhes-mqtt-cli.timer \
    openhes-mqtt-cli.service \
    openhes-ohsrv.service \
    mqtt-broker.service \
    openhes-pod.service
    do
    systemctl --user disable --now "${unit}" 2>/dev/null || true
done

rm -f "${QUADLET_DST}/openhes.pod"
rm -f "${QUADLET_DST}/mqtt-broker.container"
rm -f "${QUADLET_DST}/openhes-ohsrv.container"
rm -f "${QUADLET_DST}/openhes-mqtt-cli.container"
rm -f "${QUADLET_DST}/openhes-mqtt-cli.timer"

systemctl --user daemon-reload

echo "OpenHES Quadlet files removed from ${QUADLET_DST}."
