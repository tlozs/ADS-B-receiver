#!/bin/bash

# Exit immediately if any command fails
set -e

# ==========================================
# CONFIGURATION VARIABLES
# ==========================================
# Set this to the absolute path of the cloned GitHub repository
REPO_PATH="/home/admin/Desktop/ADS-B-receiver"

# Set this to the absolute directory path where the executable should be copied
DEST_PATH="/home/admin/Desktop/adsb_demo/adsb_radar"

# The name of the executable defined in the CMake configuration
EXE_NAME="adsb_rx"

# The name of the systemd service
SERVICE_NAME="adsb-radar.service"
# ==========================================

echo "==> Navigating to repository: $REPO_PATH"
cd "$REPO_PATH"

echo "==> Configuring build environment with CMake..."
mkdir -p build
cd build

# Generate the makefiles
cmake -DCMAKE_BUILD_TYPE=Release ..

echo "==> Compiling the application..."
make -j4

echo "==> Stopping $SERVICE_NAME..."
sudo systemctl stop "$SERVICE_NAME"

echo "==> Deploying $EXE_NAME to $DEST_PATH..."
# The executable is compiled into the current 'build' directory, so we copy it to DEST_PATH
sudo cp "$EXE_NAME" "$DEST_PATH/$EXE_NAME"

# Capture the exact time just before starting the service 
# so we can filter the logs accurately.
RESTART_TIME=$(date '+%Y-%m-%d %H:%M:%S')

echo "==> Restarting $SERVICE_NAME..."
sudo systemctl start "$SERVICE_NAME"

echo "==> Deployment complete. Waiting 10 seconds for telemetry to initialize..."
# The C daemon needs a moment to initialize the SDR ring buffers 
# and execute its first InfluxDB curl request.
sleep 10

echo "==> Checking journal for startup or connection errors..."
# Fetch all logs since restart, and use grep to filter by our specific keywords.
# The '|| true' prevents the script from crashing due to 'set -e' if grep finds no matches.
STARTUP_ERRORS=$(sudo journalctl -u "$SERVICE_NAME" --since "$RESTART_TIME" --no-pager | grep -E "WARNING|ERROR" || true)

if [ -n "$STARTUP_ERRORS" ]; then
    echo -e "\n[!] WARNING: The daemon reported errors during startup:"
    echo "--------------------------------------------------------"
    echo "$STARTUP_ERRORS"
    echo "--------------------------------------------------------"
    echo "Check the database container status or hardware connections."
else
    echo "[+] No errors detected. Daemon connected successfully."
fi

echo -e "\n==> Current status:"
sudo systemctl status "$SERVICE_NAME" --no-pager | grep "Active:"
