# Self-Hosting TankSync Cloud

This guide covers deploying TankSync Cloud on your own server.

## Requirements

- Ubuntu 22.04+ or Debian 12+ server
- Node.js 20+
- PostgreSQL 14+ (or SQLite for simple setups)
- Mosquitto MQTT broker
- Domain name (optional, for HTTPS)

## Quick Setup (SQLite, local use)

```bash
cd pwa
npm install
cp .env.example .env
# Edit .env: set JWT_SECRET, MQTT_URL
npm run build
npm start
```

Access at `http://localhost:4800`.

## Production Setup (PostgreSQL + Nginx)

### 1. Install dependencies

```bash
sudo apt install postgresql mosquitto mosquitto-clients nginx certbot
curl -fsSL https://deb.nodesource.com/setup_22.x | sudo bash -
sudo apt install nodejs
```

### 2. Create database

```bash
sudo -u postgres createuser tanksync
sudo -u postgres createdb tanksync -O tanksync
sudo -u postgres psql -c "ALTER USER tanksync PASSWORD 'your-password';"
```

### 3. Configure Mosquitto

```bash
sudo mosquitto_passwd -c /etc/mosquitto/tanksync_passwd tanksync_server
# Enter a strong password

cat << EOF | sudo tee /etc/mosquitto/conf.d/tanksync.conf
listener 1883 127.0.0.1
allow_anonymous false
password_file /etc/mosquitto/tanksync_passwd
EOF

sudo systemctl restart mosquitto
```

### 4. Deploy the app

Use `pwa/server-cloud/` for PostgreSQL, or `pwa/server/` for SQLite.

```bash
# Clone and setup
git clone https://github.com/Techposts/LoRa-Water-Tank-Monitor.git
cd LoRa-Water-Tank-Monitor/pwa

# For PostgreSQL cloud version
cp server-cloud/* server/
cp package-cloud.json package.json
npm install --omit=dev

# Configure
cp .env.example .env
# Edit .env with your database URL, MQTT credentials, etc.

# Build frontend
npm run build
```

### 5. Environment variables

```bash
# Required
JWT_SECRET=<random-64-char-string>
DATABASE_URL=postgresql://tanksync:password@localhost:5432/tanksync
MQTT_URL=mqtt://tanksync_server:password@localhost:1883

# Optional
RESEND_API_KEY=<your-resend-api-key>
TURNSTILE_SECRET=<your-cloudflare-turnstile-secret>
VAPID_PUBLIC_KEY=<generate-with-npx-web-push-generate-vapid-keys>
VAPID_PRIVATE_KEY=<matching-private-key>
```

### 6. Systemd service

Copy `pwa/deploy/tanksync.service` to `/etc/systemd/system/` and adjust paths and user.

```bash
sudo systemctl enable tanksync
sudo systemctl start tanksync
```

### 7. Nginx reverse proxy

Copy `pwa/deploy/nginx-tanksync.conf` to `/etc/nginx/sites-available/` and adjust server_name.

```bash
sudo ln -s /etc/nginx/sites-available/tanksync /etc/nginx/sites-enabled/
sudo certbot --nginx -d your-domain.com
sudo systemctl reload nginx
```

## MQTT for External Receivers

To allow receivers outside your network to connect:

1. Add a TLS listener to Mosquitto (port 8883)
2. Get a Let's Encrypt certificate for your MQTT domain
3. Configure per-user MQTT credentials
4. Set `MQTT_PUBLIC_HOST` and `MQTT_PUBLIC_PORT` in `.env`

See the main README for the full architecture diagram.
