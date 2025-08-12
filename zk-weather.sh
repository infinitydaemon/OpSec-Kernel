#!/bin/bash
# zk_weather_setup.sh
# Automated Weather Sensor Setup for ZK Secure (BME280 via I2C)
# Author: Dr. Raziek K.
# sudo bash zk_weather_setup.sh
# Do a reboot
# sudo journalctl -u zk_weather -f
set -e

echo "[INFO] Starting ZK Secure Weather Sensor Node setup..."

# Update system and install dependencies
echo "[STEP] Updating system and installing dependencies..."
apt update && apt upgrade -y
apt install -y python3-pip python3-smbus i2c-tools git

# Enable I2C interface non-interactively
# This is enabled by defauly on ZerOS but might append. Best to leave it like that for non ZK devices.
echo "[STEP] Enabling I2C interface..."
if ! grep -q "^dtparam=i2c_arm=on" /boot/config.txt; then
    echo "dtparam=i2c_arm=on" >> /boot/config.txt 
fi

if ! grep -q "^i2c-dev" /etc/modules; then
    echo "i2c-dev" >> /etc/modules
fi

modprobe i2c-dev

# Verify I2C bus presence
if ! i2cdetect -y 1 | grep -q 0x76; then
    echo "[WARNING] BME280 sensor (address 0x76) not detected on I2C bus 1."
    echo "Make sure the sensor is connected properly."
else
    echo "[INFO] BME280 sensor detected."
fi

# Install Adafruit BME280 Python library
echo "[STEP] Installing Python libraries..."
pip3 install --upgrade adafruit-circuitpython-bme280

# Create Python script directory
mkdir -p /opt/zk_weather
cat > /opt/zk_weather/bme280_reader.py << 'EOF'
import time
import board
import adafruit_bme280

def main():
    i2c = board.I2C()
    sensor = adafruit_bme280.Adafruit_BME280_I2C(i2c)

    with open("/var/log/zk_weather.log", "a") as logfile:
        while True:
            temp = sensor.temperature
            hum = sensor.humidity
            pres = sensor.pressure
            line = f"Temp: {temp:.1f} C, Humidity: {hum:.1f} %, Pressure: {pres:.1f} hPa\n"
            print(line.strip())
            logfile.write(line)
            logfile.flush()
            time.sleep(10)

if __name__ == "__main__":
    main()
EOF

# Create systemd service
cat > /etc/systemd/system/zk_weather.service << EOF
[Unit]
Description=ZK Secure BME280 Weather Sensor Service
After=network.target

[Service]
ExecStart=/usr/bin/python3 /opt/zk_weather/bme280_reader.py
Restart=always
User=root

[Install]
WantedBy=multi-user.target
EOF

# Enable and start the service
echo "[STEP] Enabling and starting zk_weather service..."
systemctl daemon-reload
systemctl enable zk_weather.service
systemctl start zk_weather.service

echo "[DONE] ZK Secure Weather Sensor setup complete!"
echo "Sensor data will be logged to /var/log/zk_weather.log"
echo "Use 'journalctl -u zk_weather' to check the service logs."
echo "Reboot is recommended to ensure I2C is fully enabled."

