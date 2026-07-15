#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
QUADLET_SRC="${SCRIPT_DIR}/systemd/user"
QUADLET_DST="${HOME}/.config/containers/systemd"

if ! command -v podman >/dev/null 2>&1; then
    echo "podman is not installed or not in PATH" >&2
    exit 1
fi

if ! command -v systemctl >/dev/null 2>&1; then
    echo "systemctl is required for Quadlet setup" >&2
    exit 1
fi

echo "Building OpenHES image..."
podman build -t localhost/openhes:dev -f "${SCRIPT_DIR}/Containerfile.openhes" "${REPO_DIR}"

echo "Installing Quadlet files into ${QUADLET_DST}"
mkdir -p "${QUADLET_DST}"
cp "${QUADLET_SRC}"/*.pod "${QUADLET_DST}/"
cp "${QUADLET_SRC}"/*.container "${QUADLET_DST}/"
cp "${QUADLET_SRC}"/*.timer "${QUADLET_DST}/"

echo "Reloading user systemd and starting services"
systemctl --user daemon-reload
systemctl --user enable --now openhes-pod.service
systemctl --user enable --now mqtt-broker.service
systemctl --user enable --now openhes-ohsrv.service
systemctl --user enable --now openhes-mqtt-cli.service

echo ""
echo "Optional: enable periodic mqtt_cli runs"
echo "systemctl --user enable --now openhes-mqtt-cli.timer"
echo ""
echo "OpenHES podman infrastructure is installed."
