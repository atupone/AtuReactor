#!/bin/bash

# Configuration
BRIDGE_BIN="./build/radar_ui_bridge"
TCP_PORT=9000
UDP_PORT=10001

# 1. Start the UI Listener (TCP)
# -l: listen, -p: port, -k: keep open for multiple tests
echo "[Test] Starting Mock UI Listener on TCP:$TCP_PORT..."
nc -l -p $TCP_PORT > ui_received.log &
UI_PID=$!

# 2. Start your Bridge
if [ ! -f "$BRIDGE_BIN" ]; then
    echo "Error: $BRIDGE_BIN not found. Did you run 'make'?"
    kill $UI_PID
    exit 1
fi

echo "[Test] Starting AtuReactor Bridge..."
$BRIDGE_BIN &
BRIDGE_PID=$!

sleep 2 # Wait for connection to establish

# 3. Send Mock Radar Data (UDP)
echo "[Test] Sending Mock Radar Packet to UDP:$UDP_PORT..."
echo "RADAR_TARGET_XY_100_200" | nc -u -w1 127.0.0.1 $UDP_PORT

# 4. Wait for processing
sleep 1

# 5. Show Results
echo "------------------------------------"
echo "Contents of UI log (Data received by TCP):"
cat ui_received.log
echo "------------------------------------"

# 6. Cleanup
kill $BRIDGE_PID
kill $UI_PID
rm ui_received.log
