#!/bin/bash
# Test and configure WSL proxy to use Windows Clash

HOST_IP=$(ip route show default | awk '{print $3}')
PROXY_PORT=7897

echo "Windows host IP: $HOST_IP"
echo "Proxy: http://$HOST_IP:$PROXY_PORT"
echo ""

echo "=== Test 1: Can reach proxy port? ==="
timeout 3 bash -c "echo >/dev/tcp/$HOST_IP/$PROXY_PORT" 2>/dev/null
if [ $? -eq 0 ]; then
    echo "Port $PROXY_PORT is OPEN"
else
    echo "Port $PROXY_PORT is CLOSED"
    echo "Please enable 'Allow LAN' in Clash settings!"
fi

echo ""
echo "=== Test 2: Proxy to Google ==="
curl -s -o /dev/null -w "HTTP %{http_code} (%{time_total}s)\n" \
    --connect-timeout 5 \
    -x "http://$HOST_IP:$PROXY_PORT" \
    https://www.google.com 2>&1

echo ""
echo "=== To set proxy permanently, add to ~/.bashrc: ==="
echo "export http_proxy=http://$HOST_IP:$PROXY_PORT"
echo "export https_proxy=http://$HOST_IP:$PROXY_PORT"
echo "export all_proxy=http://$HOST_IP:$PROXY_PORT"
echo "export no_proxy=localhost,127.0.0.1,::1"
