# RYLR998 AT Command Reference

Quick reference for the RYLR998 LoRa module AT commands used in this project.

## Basic Commands

| Command | Description | Example | Response |
|---------|-------------|---------|----------|
| `AT` | Test connection | `AT` | `+OK` |
| `AT+RESET` | Software reset | `AT+RESET` | `+RESET` then `+READY` |
| `AT+VER?` | Get firmware version | `AT+VER?` | `+VER=RYLRxx8_Vx.x.x` |
| `AT+UID?` | Get unique ID | `AT+UID?` | `+UID=xxxxxxxxxxxx` |
| `AT+FACTORY` | Reset to defaults | `AT+FACTORY` | `+FACTORY` |

## Configuration Commands

### Address
```
AT+ADDRESS=<0-65535>
AT+ADDRESS?
```
- Default: 0
- Address 0 = broadcast to all

### Network ID
```
AT+NETWORKID=<3-15, 18>
AT+NETWORKID?
```
- Default: 18
- Devices must share same Network ID to communicate

### Frequency Band
```
AT+BAND=<frequency_hz>
AT+BAND=<frequency_hz>,M    # Save to flash
AT+BAND?
```
- Default: 915000000 (RYLR998)
- Range: 820000000 - 960000000 Hz
- Common: 868000000 (EU), 915000000 (US/AU)

### RF Parameters
```
AT+PARAMETER=<SF>,<BW>,<CR>,<Preamble>
AT+PARAMETER?
```

| Parameter | Values | Default |
|-----------|--------|---------|
| Spreading Factor | 5-11 | 9 |
| Bandwidth | 7=125kHz, 8=250kHz, 9=500kHz | 7 |
| Coding Rate | 1=4/5, 2=4/6, 3=4/7, 4=4/8 | 1 |
| Preamble | 4-24 (NETWORKID=18), else 12 | 12 |

### TX Power
```
AT+CRFOP=<0-22>
AT+CRFOP?
```
- Default: 22 (max)
- CE compliance requires ≤14

### UART Baud Rate
```
AT+IPR=<rate>
AT+IPR?
```
- Options: 300, 1200, 4800, 9600, 19200, 28800, 38400, 57600, 115200
- Default: 115200

### Encryption
```
AT+CPIN=<8_hex_chars>
AT+CPIN?
```
- Example: `AT+CPIN=EEDCAA90`
- Resets after power cycle

## Operating Mode
```
AT+MODE=<mode>
AT+MODE?
```

| Mode | Description |
|------|-------------|
| 0 | Transceiver (default) |
| 1 | Sleep (10µA) |
| 2 | Smart RX power saving |

### Smart RX Mode
```
AT+MODE=2,<RX_time_ms>,<Sleep_time_ms>
```
- RX time: 30-60000 ms
- Sleep time: 30-60000 ms

## Data Transmission

### Send Data
```
AT+SEND=<Address>,<Length>,<Data>
```
- Address: 0 = broadcast, 1-65535 = specific
- Length: 1-240 bytes
- Example: `AT+SEND=2,5,HELLO`

### Receive Format
```
+RCV=<Address>,<Length>,<Data>,<RSSI>,<SNR>
```
- Automatically output when data received
- Example: `+RCV=1,5,HELLO,-45,12`

## Error Codes

| Code | Meaning |
|------|---------|
| +ERR=1 | Missing \r\n |
| +ERR=2 | Not starting with AT |
| +ERR=4 | Unknown command |
| +ERR=5 | Data length mismatch |
| +ERR=10 | TX timeout |
| +ERR=12 | CRC error |
| +ERR=13 | Data exceeds 240 bytes |
| +ERR=14 | Flash write failed |
| +ERR=17 | Previous TX not complete |
| +ERR=18 | Invalid preamble |
| +ERR=19 | RX header error |

## Power Consumption

| State | Current |
|-------|---------|
| TX @ 22dBm | 144.7 mA |
| TX @ 14dBm | 115.5 mA |
| TX @ 10dBm | 94.4 mA |
| RX | 17.5 mA |
| Sleep | 10 µA |

## Important Notes

1. All commands must end with `\r\n` (CR+LF)
2. Wait for `+OK` before sending next command
3. ADDRESS and NETWORKID are saved to flash
4. BAND can be saved with `,M` suffix
5. CPIN (encryption) resets on power cycle
6. Avoid frequencies 4MHz apart (crosstalk)
