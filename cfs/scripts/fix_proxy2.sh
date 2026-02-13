#!/bin/bash
# Clean up duplicate proxy configs in ~/.bashrc and unify into one block

# 1. Remove the three conflicting proxy blocks, keep everything else
#    - Block 1 (lines ~124-165): old "Clash Proxy Configuration" section
#    - Block 2 (lines ~179-180): hardcoded alias proxy-on/proxy-off
#    - Block 3 (lines ~184-192): our newly added block

# Create a temp cleaned version
sed '/# ============================================/,/^}$/d; /# 智能代理/d; /^export NO_PROXY=/d; /alias proxy-on=/d; /alias proxy-off=/d; /# 快速切换代理/d; /# WSL Proxy - auto-detect/,/^export no_proxy=/d; /^HOSTIP=/d; /^export WIN_HOST_IP=/d' ~/.bashrc > /tmp/bashrc_cleaned

# 2. Append one clean, unified proxy config
cat >> /tmp/bashrc_cleaned << 'EOF'

# ============================================
# WSL Proxy — route through Windows Clash
# ============================================
WIN_HOST_IP=$(ip route show default | awk '{print $3}')
PROXY_PORT=7897

# Set both upper and lowercase (different tools check different ones)
export http_proxy="http://${WIN_HOST_IP}:${PROXY_PORT}"
export https_proxy="http://${WIN_HOST_IP}:${PROXY_PORT}"
export all_proxy="socks5://${WIN_HOST_IP}:${PROXY_PORT}"
export HTTP_PROXY="$http_proxy"
export HTTPS_PROXY="$https_proxy"
export ALL_PROXY="$all_proxy"
export no_proxy="localhost,127.0.0.1,::1,10.0.0.0/8,172.16.0.0/12,192.168.0.0/16"
export NO_PROXY="$no_proxy"

proxy_on()  { export http_proxy https_proxy all_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY; echo "Proxy ON: $WIN_HOST_IP:$PROXY_PORT"; }
proxy_off() { unset http_proxy https_proxy all_proxy HTTP_PROXY HTTPS_PROXY ALL_PROXY; echo "Proxy OFF"; }
proxy_test() { curl -s -o /dev/null -w "Google: HTTP %{http_code} (%{time_total}s)\n" --connect-timeout 5 https://www.google.com; }
EOF

# 3. Replace bashrc
cp ~/.bashrc ~/.bashrc.bak
mv /tmp/bashrc_cleaned ~/.bashrc

echo "Done. Verifying..."
echo ""

# Source in a subshell to test
bash -ic 'echo "http_proxy=$http_proxy" && proxy_test'
