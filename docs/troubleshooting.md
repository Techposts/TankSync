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
- Ensure transmitter and receiver are using the same LoRa frequency and network ID
- Transmitter must be powered on within 60 seconds of pressing "Start Pairing" on the receiver
- Try power cycling the transmitter

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
