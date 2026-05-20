# Troubleshooting

## Receiver

### WiFi scan button does nothing
The receiver uses APSTA mode for scanning. If scan still fails, try rebooting the receiver.

### Captive portal doesn't appear
After connecting to the "TankSync" WiFi, try opening `http://192.168.4.1` manually. iOS/Android auto-redirect is supported but may not work on all devices.

### OLED shows "NO DATA"
No transmitter is paired yet. Go to the receiver's web UI and press "Start Pairing", then power on the transmitter.

### MQTT shows "disconnected"
- Check WiFi is connected (receiver web UI -> System tab)
- Verify MQTT broker host and port are correct
- If using TLS (port 8883), ensure `use_tls` is enabled
- Check MQTT username and password

### OTA update fails
- Ensure the `.bin` file matches your board (ESP32 DevKit vs ESP32-C3)
- File must be under 1.5MB (OTA partition limit)
- Try uploading via the web UI at `http://<receiver-ip>/api/ota/upload`

## Transmitter

### Transmitter won't pair
- Ensure transmitter and receiver are using the same LoRa frequency
- Transmitter must be powered on within 60 seconds of pressing "Start Pairing" on the receiver
- Try power cycling the transmitter
- On rx-v2.7.10+, both sides rendezvous on the well-known pair NETID (6) automatically during the pair window — you do NOT need to set NETID manually. If a TX from an even older firmware still won't pair, flash it with `tx-v2.0.11` or later.

### "Tank disappeared" / one tank stopped reporting after pairing a second one
- This was a known issue on rx-v2.7.9 and earlier — pairing a second TX rotated the receiver's NETID, silently orphaning the first one. **Fixed in rx-v2.7.10:** the NETID is now generated once on the first-ever pair and kept stable across all subsequent pairs.
- If you're still seeing this on 2.7.10+, the most likely cause is a TX still on old firmware (pre-2.0.6). Either flash the TX, or factory-reset the hub and re-pair both TXs.

### Re-paired a tank and it came back as "Tank N" instead of my custom name
- The name + capacity + alerts are preserved by a *tombstone* archive — but only for TXs that sent their MAC in PAIR_REQ (tx-v2.0.11 onwards).
- A TX paired with older firmware doesn't have a MAC stored in the hub registry, so its delete is a hard-delete and re-pair creates a fresh entry.
- One-time migration: flash the TX with `tx-v2.0.11` or later, then delete + re-pair once. From then on, the MAC is recorded and future deletes preserve the name.

### Tank still shows in PWA after I deleted it
- Was a known issue before rx-v2.7.11 — the cloud DB delete didn't propagate to the receiver, so the hub re-synced the tank back into the cloud on its next refresh.
- **Fixed in rx-v2.7.11 + cloud (deployed 2026-05-21):** the cloud `DELETE` now publishes an MQTT `remove_tx` command to the receiver and waits for ack before removing the DB row. If the receiver is offline, the delete is refused with a clear error so you can retry once it reconnects.
- If you still see the tank after the cloud says "removed", hard-refresh the PWA (close + reopen) to clear the local cache.

### Pair button on PWA seems to do nothing
- The receiver listens for 60 seconds. Make sure to hold the transmitter's BOOT button for 2 seconds and release within that window.
- On rx-v2.7.11+ the PWA shows a "Linking to your account..." spinner while the cloud saves the new tank, followed by "🎉 Paired!" with the tank's name. If you see "no transmitter responded" instead, the receiver heard nothing — check that the receiver and transmitter are within range and that the receiver isn't already paired with the same physical sensor.

### I need to start over completely (resale, lab swap, troubleshooting)
- Web UI → System tab → **Factory Reset** button. Type `ERASE` to confirm.
- This wipes Wi-Fi credentials, MQTT credentials, all paired tanks, the tombstone archive, history, and LoRa NETID.
- The hub reboots into setup AP mode (`TankSync` open network at 192.168.4.1).

### Battery shows 0%
- Check the battery voltage divider wiring
- The ADC calibration assumes a 100K/100K divider on GPIO 0
- If using a different divider ratio, adjust in `config.h`

### Distance reading is stuck or wrong
- SR04T sensor needs a clear line of sight to the water surface
- Minimum distance: 25cm, maximum: 400cm
- Check sensor wiring (TRIG and ECHO pins)

## TankSync Cloud

### Can't sign up (captcha fails)
- Disable VPN or ad blockers temporarily
- Try a different browser
- Clear cookies and try again

### Email verification code not received
- Check spam/junk folder
- Emails come from `onboarding@resend.dev` (or `noreply@smartghar.org` if domain verified)
- Click "Resend code" (rate limited to 3 per 5 minutes)

### QR code scan doesn't work
- Ensure your phone and the TankSync Cloud server are on the same network as the receiver
- The QR code contains the receiver's local IP — it must be reachable from the server
- If on a different network, manually enter MQTT details in the receiver's web UI

### Dashboard shows stale data
- Check receiver's MQTT connection status in its web UI
- If using TankSync Cloud, verify the receiver is connected to `mqtt.smartghar.org:8883`
- The transmitter sends data every sleep interval (default 5 minutes)

### Push notifications not working
- Install the app as a PWA (Add to Home Screen)
- Enable notifications when prompted
- VAPID keys must be configured in the server's `.env` file

## Hardware

### LoRa range is poor
- Ensure antennas are attached to both RYLR998 modules
- Higher mounting = better range
- Avoid metal obstructions between transmitter and receiver
- Try increasing spreading factor in LoRa settings (higher SF = longer range, slower speed)

### Solar charging not working
- TP4056 module needs direct sunlight on the panel
- Check that the solar panel output is 5-6V
- The charge LED on TP4056 should be red when charging
