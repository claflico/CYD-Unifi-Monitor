# CYD UniFi Monitor

![CYD UniFi Monitor in a 3D-printed desktop case showing live network throughput](docs/hero.png)

A small always-on display for your home network, pulled live from your UniFi controller. The main screen shows your current download and upload speeds, daily and monthly data usage, ping, your gateway's CPU and memory load, and how many clients are connected (wired and WiFi). Tap the tabs to see your busiest clients, the status of every UniFi device, and a map of which switch ports are in use.

Built for the 4-inch ESP32-S3 CYD (the "Cheap Yellow Display"), which you can find on AliExpress or Amazon for around $20.

> This is a personal hobby project. It's not affiliated with Ubiquiti Inc. "UniFi" and "Dream Machine" are trademarks of Ubiquiti.

## What you'll need

- An ESP32-S3 CYD with a 4-inch 320x480 screen (the one with an ST7796 driver)
- A USB-C cable that supports data (not the cheap charge-only kind)
- A UniFi Dream Machine on your network (UDM, UDM Pro, or UDM SE)
- A computer with [VS Code](https://code.visualstudio.com) installed
- A 2.4 GHz Wi-Fi network (the ESP32 doesn't do 5 GHz)
- Optional: a 3D-printed case. I designed one for this exact board — [Desktop Case for CYD ESP32 4" Display on MakerWorld](https://makerworld.com/en/models/1810329-desktop-case-for-cyd-esp32-4-display#profileId-1942741).

## Setup

### 1. Install PlatformIO

Open VS Code, go to Extensions (Ctrl+Shift+X), search for "PlatformIO IDE", and install the official one. Restart VS Code when it asks.

### 2. Create a UniFi API key

The CYD authenticates with an API key, so it never needs a UniFi username or password. In the UniFi Network app, go to Settings → Control Plane → Integrations and click **Create API Key**. Copy the key somewhere safe; UniFi only shows it once. You can revoke it from the same page at any time.

While you're there, note your controller's IP address (Settings → System → Advanced). If you can't find it, see [FIND_UNIFI_IP.md](FIND_UNIFI_IP.md).

### 3. Add your credentials

Clone or download this repo. In the `include/` folder, copy `credentials_template.h` to `credentials.h` and fill in the blanks:

```cpp
#define WIFI_SSID      "your-wifi-name"
#define WIFI_PASSWORD  "your-wifi-password"
#define UNIFI_HOST     "192.168.1.1"
#define UNIFI_API_KEY  "the-api-key-you-just-made"
```

`credentials.h` is gitignored, so it stays on your machine.

### 4. Plug in the CYD

Connect the board to your computer with a USB-C cable.

On Windows you may need a USB driver for the COM port to appear. Most of these boards use a CH340 USB chip, which needs the [CH340 driver from WCH](https://www.wch-ic.com/downloads/CH341SER_EXE.html). Some use a CP210x chip instead, which needs the [CP210x driver](https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers). After installing it, open Device Manager and look under "Ports (COM & LPT)" for something like COM3 or COM6. The name there also tells you which chip your board has.

PlatformIO finds the port on its own. If it picks the wrong one (for example, when more than one board is plugged in), add a line like `upload_port = COM3` under `[env:esp32-cyd]` in `platformio.ini`.

### 5. Build and upload

Click the PlatformIO icon (the alien head) in the sidebar, expand the project tasks, then click **Upload**. The first build downloads some libraries and can take a couple of minutes. When it's done, the CYD will reboot and start showing data within about 10 seconds.

![Close-up of the dashboard showing download and upload speeds](docs/display.png)

## What's on the screen

**Header.** Your gateway's name, public IP, and ISP on the left, and a status dot on the right. The dot is green and says "Online" when everything's fine. Otherwise it shows the most serious problem: red for "No API", "WAN down", or "No internet", and yellow for a WAN or internet warning, UniFi devices that are offline ("2 offline"), or a site-to-site VPN that isn't connected ("VPN down"). The header updates every 5 seconds, whichever tab is open. If the status text is long, the ISP name gets shortened or left off to make room.

**Overview.** Live download and upload speeds with a scrolling graph, then four boxes along the bottom:
- **Daily** and **Monthly**: data downloaded and uploaded so far today and this month
- **Network**: ping, and your gateway's CPU and memory use (C/M)
- **Clients**: how many clients are connected by cable (LAN) and over WiFi

**Clients.** The five clients using the most bandwidth right now, with download (▼) and upload (▲) rates for each, plus the two clients that connected most recently. A client you haven't named in UniFi shows its hostname, or its MAC address if it doesn't have one, so you can look it up in the UniFi app. These rates are averages from the controller, so they can read lower than the Overview's speeds during a short burst of traffic.

**Devices.** Every UniFi gateway, switch, and access point, with a status dot (green online, red offline, yellow while upgrading or adopting), its model, and how many clients it has. The bottom-right corner shows how many are online, like "17/17 online".

**Ports.** One row per device that has ports. Each port is a small square: purple for 10G, cyan for 2.5G, green for 1G, orange for 10/100, and a gray outline if nothing is plugged in. The uplink port (the one leading back toward the gateway) has a white outline.

The Clients, Devices, and Ports lists are large, so the CYD only downloads them while that tab is open: every 30 seconds for Clients and every minute for Devices and Ports. Each download takes about 2 seconds, and the screen won't respond to taps during that time. "Updating..." shows while it loads.

## When something's wrong

**Screen is blank.** Try a different USB cable. The cheap charger-only ones have no data lines. If that doesn't help, hit the reset button on the back of the CYD.

**"WiFi Connection Failed".** Double-check the SSID and password. Make sure it's a 2.4 GHz network. Many modern routers have separate 2.4 and 5 GHz networks even when they share a name.

**"API key check failed. HTTP code: 401".** The API key is wrong or was revoked. Create a new one and update `UNIFI_API_KEY`.

**All values are zero.** Wait 30 seconds. UniFi doesn't always return throughput numbers on the first request.

**"Update failed" on the Clients, Devices, or Ports tab.** These tabs use UniFi's newer v2 API, which older versions of the UniFi Network app may not have. The serial monitor shows why the request failed. The tab tries again on its next refresh, and if a list loaded earlier, it keeps showing that one in the meantime.

**The header says "VPN down" but your VPN is fine.** Set `SHOW_VPN_STATUS` to 0 in `include/config.h` to turn the VPN check off, and please open an issue with your setup.

For more detail, open the PlatformIO serial monitor (115200 baud) and watch the boot output.

If you're stuck on something that isn't covered here, paste the error from the serial monitor or the PlatformIO build log into ChatGPT or Claude. They're surprisingly good at decoding ESP32 build errors and UniFi API responses.

## Customizing

The things you might want to tweak live in `include/config.h`:

| Setting | Default | What it does |
|---|---|---|
| `API_UPDATE_INTERVAL` | 5 seconds | How often the header and Overview refresh |
| `USAGE_REPORT_INTERVAL` | 5 minutes | How often the Daily and Monthly totals refresh |
| `VPN_STATUS_INTERVAL` | 1 minute | How often the VPN status is checked |
| `CLIENT_LIST_INTERVAL` | 30 seconds | How often the Clients tab refreshes while it's open |
| `DEVICE_LIST_INTERVAL` | 1 minute | How often the Devices and Ports tabs refresh while open |
| `SHOW_VPN_STATUS` | 1 (on) | Set to 0 if you don't want a VPN problem to turn the header status yellow |

All times are in milliseconds in the file (for example, 5 seconds is `5000`). The status colors are in there too.

## Helper scripts

The `tools/` folder has a few Python scripts that hit the same UniFi endpoints from your computer. Handy for testing whether your credentials work or for poking at the API. They read the same `include/credentials.h` file you already filled in.

```
cd cyd-unifi-monitor
python tools/test_auth.py
```

You'll need Python 3 and the `requests` library (`pip install requests`).

## License

MIT. See [LICENSE](LICENSE).
