#!/bin/bash
# TankSync PWA — Deployment Script
# Run from your local machine to deploy to the Debian server
#
# Usage: ./deploy/deploy.sh [server_ip] [user]
# Example: ./deploy/deploy.sh 192.168.1.100 your-user

set -euo pipefail

SERVER="${1:-192.168.1.100}"
USER="${2:-tanksync}"
REMOTE_DIR="/home/$USER/tanksync-pwa"
LOCAL_DIR="$(cd "$(dirname "$0")/.." && pwd)"

echo "=== TankSync PWA Deployment ==="
echo "Server: $USER@$SERVER"
echo "Local:  $LOCAL_DIR"
echo "Remote: $REMOTE_DIR"
echo ""

# Step 1: Build frontend locally
echo "[1/5] Building frontend..."
cd "$LOCAL_DIR"
npm run build
echo "  -> dist/ built successfully"

# Step 2: Sync files to server
echo "[2/5] Syncing files to server..."
rsync -avz --delete \
  --exclude 'node_modules' \
  --exclude '.git' \
  --exclude 'data' \
  --exclude '.env' \
  "$LOCAL_DIR/" "$USER@$SERVER:$REMOTE_DIR/"
echo "  -> Files synced"

# Step 3: Install dependencies on server
echo "[3/5] Installing dependencies on server..."
ssh "$USER@$SERVER" "cd $REMOTE_DIR && npm ci --omit=dev"
echo "  -> Dependencies installed"

# Step 4: Setup .env if not exists
echo "[4/5] Checking .env..."
ssh "$USER@$SERVER" "
  if [ ! -f $REMOTE_DIR/.env ]; then
    cp $REMOTE_DIR/.env.example $REMOTE_DIR/.env
    # Generate a random JWT secret
    JWT_SECRET=\$(openssl rand -hex 32)
    sed -i \"s/change-this-to-a-random-64-char-string/\$JWT_SECRET/\" $REMOTE_DIR/.env
    echo '  -> .env created with random JWT secret'
  else
    echo '  -> .env already exists (keeping existing)'
  fi
"

# Step 5: Restart service
echo "[5/5] Restarting service..."
ssh "$USER@$SERVER" "
  # Install systemd service if not already installed
  if [ ! -f /etc/systemd/system/tanksync.service ]; then
    sudo cp $REMOTE_DIR/deploy/tanksync.service /etc/systemd/system/
    sudo systemctl daemon-reload
    sudo systemctl enable tanksync
    echo '  -> Service installed and enabled'
  fi
  sudo systemctl restart tanksync
  sleep 2
  sudo systemctl status tanksync --no-pager
"

echo ""
echo "=== Deployment Complete ==="
echo "Access at: http://$SERVER:4800"
echo ""
echo "Optional: Setup Nginx reverse proxy:"
echo "  sudo cp $REMOTE_DIR/deploy/nginx-tanksync.conf /etc/nginx/sites-available/tanksync"
echo "  sudo ln -sf /etc/nginx/sites-available/tanksync /etc/nginx/sites-enabled/"
echo "  sudo nginx -t && sudo systemctl reload nginx"
