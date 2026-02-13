#!/bin/bash
# Fix WSL proxy config in ~/.bashrc

# Remove the broken proxy block we just added
sed -i '/# WSL Proxy - route through Windows Clash/,/^export no_proxy=/d' ~/.bashrc

# Remove old HOSTIP line if exists
sed -i '/^export HOSTIP=/d' ~/.bashrc

# Append correct proxy config with proper variable expansion
cat >> ~/.bashrc << 'EOF'

# WSL Proxy - auto-detect Windows host IP, route through Clash
HOSTIP=$(ip route show default | awk '{print $3}')
export http_proxy="http://${HOSTIP}:7897"
export https_proxy="http://${HOSTIP}:7897"
export all_proxy="http://${HOSTIP}:7897"
export HTTP_PROXY="http://${HOSTIP}:7897"
export HTTPS_PROXY="http://${HOSTIP}:7897"
export ALL_PROXY="socks5://${HOSTIP}:7897"
export no_proxy="localhost,127.0.0.1,::1"
EOF

echo "Fixed. Verifying..."
source ~/.bashrc
echo "http_proxy=$http_proxy"
curl -s -o /dev/null -w "Google: HTTP %{http_code} (%{time_total}s)\n" --connect-timeout 5 https://www.google.com
