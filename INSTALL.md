# Installation

The following instructions are tested on a Raspberry Pi 5 running Raspberry Pi OS (Debian Trixie ARM64). The daemon is designed to run as a background service under `systemd`, and it requires InfluxDB 3 Enterprise and Grafana for telemetry storage and visualization.

It should work on other Linux distributions as well, but the build and runtime dependencies may differ.

## Building the Project

### 1. Install Prerequisites
Install the system packages for Debian and Ubuntu environments required to build and configure the daemon:

```bash
sudo apt-get update
sudo apt-get install libuhd-dev uhd-host libcurl4-openssl-dev cmake build-essential neovim git
```

To download the UHD driver images, run the following command:

```bash
sudo uhd_images_downloader
```

Additionally, to run the daemon as a background service, ensure that `systemd` is installed and enabled on your system.

Running InfluxDB and Grafana in Docker containers is recommended for ease of deployment and management. To have the latest version of Docker and Docker Compose installed, you can install it manually, following the official
**[Docker for Debian manual installation](https://docs.docker.com/engine/install/debian/#install-from-a-package)** guide and the **[post installation steps](https://docs.docker.com/engine/install/linux-postinstall)**.

### 2. Build from source
Clone the repository and navigate to the project directory:
```bash
git clone https://github.com/Tlozs/ADS-B-receiver.git
cd ADS-B-receiver
```

Configure the build environment using CMake and compile the application in Release mode:

```bash
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j4
cd ..
```

### 3. Create runtime directories
Create the necessary directories for the daemon's configuration and binary file, as well as for InfluxDB and Grafana data storage:

```bash
sudo mkdir -p /opt/adsb-radar/{bin,conf,data/influxdb,data/grafana,data/grafana/plugins,resources/grafana}
```

Copy the compiled binary to the appropriate directory:

```bash
sudo cp build/adsb_rx /opt/adsb-radar/bin/
```

### 4. Create a dedicated user
To enhance security, create a dedicated system user for the radar daemon, InfluxDB, and Grafana. This user will be a system account without home directory and without login privileges:

```bash
sudo useradd -r -M -s /usr/sbin/nologin adsb
```

To allow the daemon user to access the SDR hardware, add it to the `plugdev` group:

```bash
sudo usermod -aG plugdev adsb
```

Change the ownership of the `/opt/adsb-radar` directory to the `adsb` user and group:

```bash
sudo chown -R adsb:adsb /opt/adsb-radar
```

## Configuring InfluxDB and Grafana

### 1. Create an admin secret token for InfluxDB
To secure access to InfluxDB, create an admin token secret file. This token will be used to authenticate and manage the InfluxDB instance.

First, generate a random 128-bit token using OpenSSL:

```bash
openssl rand -hex 16
```

Next, create a JSON file:

```bash
sudo nvim /opt/adsb-radar/conf/admin-token.json
```

And populate it with the following content, replacing `YOUR_128_BIT_RANDOM_STRING_HERE` with the generated token. Note the `apiv3_` prefix, which is required for InfluxDB 3.x tokens and should be included in the token string:

```json
{
  "token": "apiv3_YOUR_128_BIT_RANDOM_STRING_HERE",
  "name": "admin",
  "description": "Admin token for automated deployment"
}
```

Then, secure the token file by setting appropriate permissions:

```bash
sudo chown adsb:adsb /opt/adsb-radar/conf/admin-token.json
sudo chmod 600 /opt/adsb-radar/conf/admin-token.json
```

### 2. Configure Docker Compose for InfluxDB and Grafana
Copy the Docker Compose blueprint and Grafana resources provided in this repository to the `/opt/adsb-radar` directory:

```bash
sudo cp ./influxdb-grafana/docker-compose.yaml /opt/adsb-radar/docker-compose.yaml
sudo cp ./influxdb-grafana/grafana-resources/* /opt/adsb-radar/resources/grafana/
```

Set up docker to use the correct user and group IDs to run the containers. Additionally, set up the email address for the InfluxDB license (change `your.email@example.com` to your actual email):

```bash
sudo tee /opt/adsb-radar/.env > /dev/null <<EOF
DAEMON_UID=$(id -u adsb)
DAEMON_GID=$(id -g adsb)
LICENSE_EMAIL=your.email@example.com
EOF
```

Secure the `.env` file:

```bash
sudo chown adsb:adsb /opt/adsb-radar/.env
sudo chmod 600 /opt/adsb-radar/.env
```

### 3. Ensure the Pi uses correct kernel
A page size of 4KB is required for the InfluxDB 3.x database to function correctly. To ensure the Pi uses the correct kernel, edit the `/boot/firmware/config.txt` file:

```bash
sudo nvim /boot/firmware/config.txt
```

Add the following line to the end of the file:

```ini
kernel=kernel8.img
```

Reboot the Pi to apply the changes:

```bash
sudo reboot
```

Verify that the page size is 4KB by running:

```bash
getconf PAGE_SIZE
```

### 4. Start the Docker containers
Navigate to the `/opt/adsb-radar` directory and start the Docker containers for InfluxDB and Grafana:

```bash
cd /opt/adsb-radar
sudo docker compose up -d
```

The database will pause its boot sequence to ping the InfluxDB license server. Check your email for the license confirmation and follow the instructions to complete the setup.

### 5. Create the radar database in InfluxDB
Once the InfluxDB container is running, create the `radar` database using the admin token you generated earlier:

```bash
docker exec -it influxdb_enterprise influxdb3 create database radar --token $(sudo grep -Po '"token": "\K[^"]*' /opt/adsb-radar/conf/admin-token.json)
```

### 6. Verify the database creation using InfluxDB Explorer
InfluxDB 3.x provides a web-based interface called InfluxDB Explorer. On a laptop connected to the same Wi-Fi network as the Pi, open a browser and navigate to: `http://<RASPBERRY_PI_IP>:8888`

Click Configure Server in the top right.

- **Name**: Local Radar
- **Server URL**:  `http://influxdb_enterprise:8181` (Note: Use the container name, not localhost. The UI will resolve this internally over the isolated virtual Docker network)
- **Token**: Paste your `apiv3_` token

**Note** that the use of InfluxDB Explorer UI this way is **intended only for development and testing purposes**. For this reason, the UI configuration is not persisted across container restarts. Exposing the Explorer on the local network with an admin token is not recommended for production deployments.

### 7. Generate a read-only token
To enhance security, generate a read-only token for the radar database. This token will be used by Grafana to query the database without having full administrative privileges:

```bash
docker exec influxdb_enterprise influxdb3 create token \
  --token "$(sudo grep -Po '"token": "\K[^"]*' /opt/adsb-radar/conf/admin-token.json)" \
  --permission "db:radar:read" \
  --name "Radar-ReadOnly" | grep -o 'apiv3_[A-Za-z0-9_-]*' | head -n 1
```

### 8. Configure Grafana to connect to InfluxDB
Open a browser and navigate to `http://<RASPBERRY_PI_IP>:3000`.
Log in with the default credentials (username: `admin`, password: `admin`) and change the password when prompted.

When logged in, on the left panel, go to **Connections** -> **Data Sources** -> **Add Data Source** -> **InfluxDB**.

Set the following options:
- **Query Language**: `SQL`
- **URL**: `http://influxdb_enterprise:8181`

At the bottom of the page, under **InfluxDB Details**, enter the following:
- **Database**: `radar`
- **Token**: Paste the read-only token generated above
- **Insecure Connection**: Tick the box

Press **Save & Test** to verify the connection. If successful, you will see a confirmation message indicating that the data source is working.
Now you can set up dashboards and panels in Grafana to visualize the radar data.

## Configuring the Daemon

### 1. Create and populate the configuration file
The daemon requires an InfluxDB token and the UHD driver image directory. The command will automatically generate a write-only token and locate the correct UHD images directory for your installed version:

```bash
sudo tee /opt/adsb-radar/conf/radar.env > /dev/null <<EOF
INFLUX_TOKEN=$(docker exec influxdb_enterprise influxdb3 create token \
  --token "$(sudo grep -Po '"token": "\K[^"]*' /opt/adsb-radar/conf/admin-token.json)" \
  --permission "db:radar:write" \
  --name "Radar-WriteOnly" | grep -o 'apiv3_[A-Za-z0-9_-]*' | head -n 1)
UHD_IMAGES_DIR=$(find /usr/share/uhd -maxdepth 2 -type d -name "images" | sort -V | tail -n 1)
CONFIG_PATH=/opt/adsb-radar/conf/optimal_sdr_gain.conf
EOF
```

### 2. Secure the configuration file
To ensure that the configuration file is secure and only accessible by the daemon user, execute the following commands:

```bash
sudo chown adsb:adsb /opt/adsb-radar/conf/radar.env
sudo chmod 600 /opt/adsb-radar/conf/radar.env
```

### 3. Create the `systemd` service file
Create a new `systemd` service file for the radar daemon:

```bash
sudo nvim /etc/systemd/system/adsb-radar.service
```

### 4. Populate the service file
Add the following content to the service file:

```ini
[Unit]
Description=ADS-B Radar Daemon
After=network.target docker.service
Requires=docker.service

[Service]
User=adsb
Group=adsb
WorkingDirectory=/opt/adsb-radar/bin/
EnvironmentFile=/opt/adsb-radar/conf/radar.env
ExecStart=/opt/adsb-radar/bin/adsb_rx
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

### 5. Enable and start the service
Reload the systemd manager configuration, enable the radar service to start on boot, and start the service:

```bash
sudo systemctl daemon-reload
sudo systemctl enable adsb-radar.service
sudo systemctl start adsb-radar.service
```

### 6. Verify the service status
Check the status of the radar service to ensure it is running correctly:

```bash
sudo systemctl status adsb-radar.service
```

### 7. View logs
To view the logs generated by the radar daemon, use the following command:

```bash
sudo journalctl -u adsb-radar.service -f
```
