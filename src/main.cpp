// Working UniFi Dashboard with prioritized touch handling
#include <Arduino.h>
#include <WiFi.h>
#include <TFT_eSPI.h>
#include "config.h"
#include "credentials.h"
#include "unifi_api.h"

#ifndef UNIFI_API_KEY
#error "UNIFI_API_KEY is not defined. Add it to include/credentials.h (see credentials_template.h)."
#endif
#include "network_handler.h"

// Simple approach: Touch priority, minimal API blocking

// Display and data
TFT_eSPI tft = TFT_eSPI();
UniFiAPI unifiAPI;
NetworkHandler network;
NetworkStats currentStats;
NetworkStats statusStats;    // Latest health response, kept even when it has no traffic
bool statusKnown = false;    // False until the first API attempt

// Devices tab: fetched only while the tab is open
UniFiDevice devices[MAX_DEVICES_TRACKED];
int deviceTotal = -1;            // -1 until the first successful fetch
bool deviceFetchFailed = false;  // True when the latest fetch failed
unsigned long lastDeviceFetch = 0;

// Clients tab: fetched only while the tab is open
ClientSummary clientSummary;
bool clientsLoaded = false;      // False until the first successful fetch
bool clientFetchFailed = false;  // True when the latest fetch failed
unsigned long lastClientFetch = 0;

// Smooth animation variables
float smoothDownload = 0;
float smoothUpload = 0;
float targetDownload = 0;
float targetUpload = 0;
int targetDevices = 0;
float targetPing = 0;


// Graph data storage
#define GRAPH_POINTS 60
float downloadHistory[GRAPH_POINTS] = {0};
float uploadHistory[GRAPH_POINTS] = {0};
int graphScrollCounter = 0;
float graphMaxScale = 10.0;

// Tabs, left to right
#define TAB_OVERVIEW 1
#define TAB_CLIENTS 2
#define TAB_DEVICES 3
#define TAB_PORTS 4
#define TAB_COUNT 4

// Display layout (SCREEN_WIDTH/SCREEN_HEIGHT come from config.h)
#define HEADER_HEIGHT 40
#define TAB_BAR_HEIGHT 30
#define TAB_BAR_Y HEADER_HEIGHT
#define CONTENT_START_Y (HEADER_HEIGHT + TAB_BAR_HEIGHT)
#define GRAPH_X 10
#define GRAPH_Y (CONTENT_START_Y + 10)
#define GRAPH_WIDTH 280
#define GRAPH_HEIGHT 135
#define SPEED_BOX_X 300
#define SPEED_BOX_Y (CONTENT_START_Y + 10)
#define SPEED_BOX_WIDTH 170
#define SPEED_BOX_HEIGHT 135
#define STATS_Y (CONTENT_START_Y + 155)
#define STATS_HEIGHT 95
#define DEVICE_COLS 2
#define DEVICE_ROWS 9
#define DEVICE_ROW_HEIGHT 27
#define DEVICE_TOP (CONTENT_START_Y + 4)
#define PORT_ROWS 11
#define PORT_ROW_HEIGHT 20
#define PORT_TOP (CONTENT_START_Y + 4)
#define PORT_STRIP_X 140             // Port squares start right of the device name
#define PORT_PITCH 12                // 10px square plus a 2px gap
#define PORT_LEGEND_Y (PORT_TOP + PORT_ROWS * PORT_ROW_HEIGHT + 4)

// Dark Theme Colors (explicit color values for true dark theme)
#define COLOR_BG 0x0000              // Pure black background (explicit)
#define COLOR_PANEL_BG 0x2104        // Very dark grey panel background
#define COLOR_HEADER_START 0x0000    // Black header start (explicit)
#define COLOR_HEADER_END 0x0000      // Black header end (explicit)
#define COLOR_BORDER TFT_DARKGREY    // Dark gray border
#define COLOR_GRID TFT_DARKGREY      // Dark gray grid
#define COLOR_TEXT_PRIMARY TFT_WHITE // White text
#define COLOR_TEXT_SECONDARY TFT_LIGHTGREY // Light gray text
#define COLOR_DOWNLOAD 0x5D9F       // Turquoise/Cornflower blue for download
#define COLOR_UPLOAD 0xC11F         // Purple for upload
#define COLOR_STAT_VALUE TFT_CYAN    // Cyan for stat values
#define COLOR_PORT_10G 0xC11F        // Purple for 10G+ links
#define COLOR_PORT_2G5 TFT_CYAN      // Cyan for 2.5G/5G links
#define COLOR_PORT_1G 0x07E0         // Green for 1G links
#define COLOR_PORT_100M TFT_ORANGE   // Orange for 10/100 links

// Timing
unsigned long lastApiUpdate = 0;
unsigned long lastGraphScroll = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastStatsRedraw = 0;

const unsigned long GRAPH_SCROLL_INTERVAL = 1000;   // Slower graph scroll for touch responsiveness
const unsigned long DISPLAY_UPDATE_INTERVAL = 500;   // Less frequent display updates
const unsigned long STATS_REDRAW_INTERVAL = 2000;   // Much slower stats refresh for touch priority

// Function declarations
void drawHeader();
void drawHeaderContent();
void drawStaticElements();
void updateSpeedDisplay();
void updateGraphDisplay();
void scrollGraphData();
void updateStatsBar();
void drawUsageRow(int x, int y, uint64_t bytes, bool isDownload, bool valid);
void drawLabeledRow(int x, int y, const char* label, const String& value, const char* unit = "");
void drawTabBar();
void handleTouch();
void drawDevicesTab();
void drawDeviceSummary(const String& note);
void drawPortLegend(const String& note);
void drawPortsTab();
void drawClientsTab();
void drawClientsNote(const String& note);

// Previous values for change detection
float prevDrawnDownload = -1;
float prevDrawnUpload = -1;
int prevDeviceCount = -1;
float prevPing = -1;

// Tab management
int currentTab = TAB_OVERVIEW;
int prevTab = 0;
bool tabNeedsRedraw = true;

// Touch state - NON-BLOCKING
unsigned long lastTouchTime = 0;
unsigned long touchDebounce = 300;  // Increased debounce
bool touchPressed = false;
uint16_t lastTouchX = 0;
uint16_t lastTouchY = 0;

// Removed dual-core complexity - using simple prioritized approach

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    Serial.println("\n=======================");
    Serial.println("UniFi Network Monitor");
    Serial.println("=======================");
    
    // Initialize display
    pinMode(27, OUTPUT);
    digitalWrite(27, HIGH);  // Backlight ON
    
    tft.init();
    tft.setRotation(DISPLAY_ROTATION);
    tft.fillScreen(0x0000);  // Explicit black color
    
    #if TOUCH_CALIBRATION_MODE
    // Interactive calibration: tap each corner arrow, then copy the printed values
    Serial.println("TOUCH CALIBRATION MODE - tap each corner arrow");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(2);
    tft.setCursor(60, 140);
    tft.print("Touch the corner arrows");
    uint16_t calData[5];
    tft.calibrateTouch(calData, TFT_MAGENTA, TFT_BLACK, 15);

    Serial.printf("CALIBRATION RESULT: #define TOUCH_CAL_DATA { %u, %u, %u, %u, %u }\n",
                  calData[0], calData[1], calData[2], calData[3], calData[4]);

    // Echo touches so the calibration can be checked before flashing it in
    tft.fillScreen(TFT_BLACK);
    tft.setCursor(20, 140);
    tft.print("Calibrated - tap to test");
    while (true) {
        uint16_t x, y;
        if (tft.getTouch(&x, &y)) {
            Serial.printf("Test touch at X=%u, Y=%u\n", x, y);
            tft.fillCircle(x, y, 3, TFT_GREEN);
        }
        delay(20);
    }
    #else
    uint16_t calData[5] = TOUCH_CAL_DATA;
    tft.setTouch(calData);
    #endif
    
    // Splash screen
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(3);
    tft.setCursor(100, 100);
    tft.print("UniFi Monitor");
    tft.setTextSize(2);
    tft.setCursor(150, 150);
    tft.print("Connecting...");
    
    // Connect to WiFi
    Serial.print("Connecting to WiFi");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 60) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi connected!");
        Serial.print("IP: ");
        Serial.println(WiFi.localIP());
        
        // Connect to UniFi
        String unifiUrl = "https://" + String(UNIFI_HOST) + ":" + String(UNIFI_PORT);
        if (unifiAPI.connect(unifiUrl, UNIFI_API_KEY)) {
            Serial.println("UniFi connected!");
        } else {
            Serial.println("UniFi connection failed!");
        }
    } else {
        Serial.println("\nWiFi connection failed!");
    }
    
    // Clear screen and draw UI - ensure true black background
    tft.fillScreen(0x0000);  // Use explicit black color value
    drawHeader();
    drawTabBar();
    drawStaticElements();
    
    // Simple single-threaded approach with touch priority
}

void loop() {
    unsigned long now = millis();
    
    #if ENABLE_TOUCH
    // Handle touch with MAXIMUM PRIORITY - check every loop
    handleTouch();

    // Additional touch checks for ultra responsiveness
    static unsigned long lastQuickTouch = 0;
    if (now - lastQuickTouch >= 1) {  // Check every 1ms for ULTRA responsiveness
        lastQuickTouch = now;
        handleTouch();
    }
    #endif

    // Check if tab changed
    if (currentTab != prevTab || tabNeedsRedraw) {
        prevTab = currentTab;
        tabNeedsRedraw = false;
        
        // Clear content area
        tft.fillRect(0, CONTENT_START_Y, SCREEN_WIDTH, SCREEN_HEIGHT - CONTENT_START_Y, COLOR_BG);
        
        // Draw appropriate tab content
        if (currentTab == TAB_OVERVIEW) {
            drawStaticElements();
            // Panels were just cleared, so force the speed values to redraw
            prevDrawnDownload = -1;
            prevDrawnUpload = -1;
        } else if (currentTab == TAB_CLIENTS) {
            drawClientsTab();
        } else if (currentTab == TAB_DEVICES) {
            drawDevicesTab();
        } else if (currentTab == TAB_PORTS) {
            drawPortsTab();
        }
    }

    // Health data runs on every tab so the header stays live; only the Overview draws the rest
    if (now - lastApiUpdate >= API_UPDATE_INTERVAL) {
        lastApiUpdate = now;

        if (unifiAPI.isConnected()) {
            Serial.printf("Real API call at %lums\n", now);

            // Get real network stats with fast timeout
            NetworkStats newStats = unifiAPI.getNetworkStats();
            // The header tracks every response, including a WAN outage with no traffic
            statusStats = newStats;

            // Only update if we got valid data
            if (newStats.download_mbps > 0 || newStats.upload_mbps > 0) {
                currentStats = newStats;

                // Update targets for smooth animation
                targetDownload = currentStats.download_mbps;
                targetUpload = currentStats.upload_mbps;
                targetDevices = currentStats.connected_devices;
                targetPing = currentStats.ping_ms;

                Serial.printf("REAL DATA: %d devices, %.1f/%.1f Mbps, %.1fms ping, gw %.0f%% cpu %.0f%% mem\n",
                             currentStats.connected_devices, currentStats.download_mbps,
                             currentStats.upload_mbps, currentStats.ping_ms,
                             currentStats.gateway_cpu, currentStats.gateway_mem);
            } else {
                Serial.println("API call failed or returned no data");
            }
        } else {
            statusStats.health_ok = false;
        }
        statusKnown = true;
        drawHeaderContent();
    }

    // Overview - Continue normal updates
    if (currentTab == TAB_OVERVIEW) {
        #if ENABLE_TOUCH
        // CRITICAL: Check touch again for Overview tab since it has heavy updates
        handleTouch();
        #endif

        // Smooth animation
        if (now - lastDisplayUpdate >= DISPLAY_UPDATE_INTERVAL) {
            lastDisplayUpdate = now;
            
            float smoothingFactor = 0.15;
            smoothDownload += (targetDownload - smoothDownload) * smoothingFactor;
            smoothUpload += (targetUpload - smoothUpload) * smoothingFactor;
            
            updateSpeedDisplay();
        }
        
        // Scroll graph
        if (now - lastGraphScroll >= GRAPH_SCROLL_INTERVAL) {
            lastGraphScroll = now;
            scrollGraphData();
            updateGraphDisplay();
        }
        
        // Update stats bar
        if (now - lastStatsRedraw >= STATS_REDRAW_INTERVAL) {
            lastStatsRedraw = now;
            updateStatsBar();
        }
    }
    
    // Clients: the ~145 KB client list is only fetched while this tab is open
    if (currentTab == TAB_CLIENTS &&
        (lastClientFetch == 0 || now - lastClientFetch >= CLIENT_LIST_INTERVAL)) {
        lastClientFetch = now;
        drawClientsNote("Updating...");  // The fetch blocks the screen (and touch) for a few seconds

        clientFetchFailed = !unifiAPI.getClients(clientSummary);
        if (!clientFetchFailed) {
            clientsLoaded = true;
        }
        drawClientsTab();
    }

    // Devices and Ports share one ~110 KB device list, fetched only while they're open
    if ((currentTab == TAB_DEVICES || currentTab == TAB_PORTS) &&
        (lastDeviceFetch == 0 || now - lastDeviceFetch >= DEVICE_LIST_INTERVAL)) {
        lastDeviceFetch = now;
        // The fetch blocks the screen (and touch) for a couple of seconds
        if (currentTab == TAB_DEVICES) {
            drawDeviceSummary("Updating...");
        } else {
            drawPortLegend("Updating...");
        }

        int total = unifiAPI.getDevices(devices, MAX_DEVICES_TRACKED);
        deviceFetchFailed = total < 0;
        if (!deviceFetchFailed) {
            deviceTotal = total;
        }
        if (currentTab == TAB_DEVICES) {
            drawDevicesTab();
        } else {
            drawPortsTab();
        }
    }

    delay(1);  // ULTRA fast loop for maximum touch responsiveness
}

void handleTouch() {
    uint16_t x = 0, y = 0;
    
    // Check for touch
    bool pressed = tft.getTouch(&x, &y);
    unsigned long now = millis();
    
    // Coordinates are already in screen space via TOUCH_CAL_DATA
    if (pressed) {
        Serial.print("Touch DOWN at X=");
        Serial.print(x);
        Serial.print(", Y=");
        Serial.println(y);
    }
    
    // Reset touch state if no touch for a while
    if (!pressed && (now - lastTouchTime > 300)) {
        touchPressed = false;
    }
    
    // Handle touch press with proper debouncing
    if (pressed && !touchPressed && (now - lastTouchTime > touchDebounce)) {
        touchPressed = true;
        lastTouchTime = now;
        
        // Check if touch is in tab bar area
        if (y >= TAB_BAR_Y && y <= TAB_BAR_Y + TAB_BAR_HEIGHT) {
            // Check which tab was pressed
            int tabWidth = SCREEN_WIDTH / TAB_COUNT;
            int newTab = (x / tabWidth) + 1;

            if (newTab >= 1 && newTab <= TAB_COUNT && newTab != currentTab) {
                Serial.printf("Tab %d pressed!\n", newTab);
                currentTab = newTab;
                tabNeedsRedraw = true;
                drawTabBar();  // Redraw tab bar immediately
            }
        }
    } else if (!pressed && touchPressed) {
        // Touch release
        touchPressed = false;
        Serial.println("Touch UP");
    }
}

void drawHeader() {
    // Draw gradient header
    for (int y = 0; y < HEADER_HEIGHT; y++) {
        uint16_t color = tft.alphaBlend(y * 255 / HEADER_HEIGHT, COLOR_HEADER_END, COLOR_HEADER_START);
        tft.drawFastHLine(0, y, SCREEN_WIDTH, color);
    }
    
    tft.drawLine(0, HEADER_HEIGHT, SCREEN_WIDTH, HEADER_HEIGHT, COLOR_BORDER);

    drawHeaderContent();
}

// The most severe problem in the latest health data, or "Online" when there is none
void getHeaderStatus(String& text, uint16_t& color) {
    const NetworkStats& s = statusStats;
    if (!statusKnown) {
        text = "Loading"; color = COLOR_TEXT_DIM;
    } else if (!s.health_ok) {
        text = "No API"; color = COLOR_ERROR;
    } else if (s.wan_status != "" && s.wan_status != "ok") {
        if (s.wan_status == "warning") { text = "WAN issue"; color = COLOR_WARNING; }
        else { text = "WAN down"; color = COLOR_ERROR; }
    } else if (s.www_status != "" && s.www_status != "ok") {
        if (s.www_status == "warning") { text = "Net issue"; color = COLOR_WARNING; }
        else { text = "No internet"; color = COLOR_ERROR; }
    } else if (s.devices_offline > 0) {
        text = String(s.devices_offline) + " offline"; color = COLOR_WARNING;
    #if SHOW_VPN_STATUS
    } else if (s.vpn_down > 0) {
        text = "VPN down"; color = COLOR_WARNING;
    #endif
    } else {
        text = "Online"; color = COLOR_SUCCESS;
    }
}

// Status dot and text right-aligned; the title ("UDMSE | 66.109.125.12 | TotelCSI") gets the room
// that leaves, so a short status like "Online" shows more of it. Redraws only when either changes,
// and keeps the last title while the API is unreachable. Runs on every tab
void drawHeaderContent() {
    static bool drawn = false;
    static String prevBase = "";
    static String prevIsp = "";
    static String prevText = "";
    static uint16_t prevColor = 0;

    // Gateway and IP come first; the ISP goes last so it's what gets trimmed
    String base = prevBase;
    String isp = prevIsp;
    if (statusStats.health_ok && !statusStats.gateway_name.isEmpty()) {
        base = statusStats.gateway_name;
        if (!statusStats.wan_ip.isEmpty()) base += " | " + statusStats.wan_ip;
        isp = statusStats.isp_name;
    }

    String text;
    uint16_t color;
    getHeaderStatus(text, color);

    if (drawn && base == prevBase && isp == prevIsp && text == prevText && color == prevColor) {
        return;
    }
    if (text != prevText) {
        Serial.println("Header status: " + text);
    }
    drawn = true;
    prevBase = base;
    prevIsp = isp;
    prevText = text;
    prevColor = color;

    tft.fillRect(0, 0, SCREEN_WIDTH, HEADER_HEIGHT, COLOR_HEADER_START);
    tft.setTextSize(2);
    tft.setTextColor(COLOR_TEXT_PRIMARY);

    int textX = SCREEN_WIDTH - 10 - text.length() * 12;
    int dotX = textX - 15;
    tft.fillCircle(dotX, 20, 6, color);
    tft.setCursor(textX, 12);
    tft.print(text);

    // Until the first health response arrives
    if (base.isEmpty()) {
        base = "UDM Network Monitor";
    }

    // Fit left of the dot with a gap. The ISP is trimmed to fit, or left off entirely
    // when fewer than 4 of its letters would show
    int maxChars = (dotX - 6 - 12 - 10) / 12;
    int ispRoom = maxChars - (int)base.length() - 3;  // After " | "
    String shown = base;
    if (!isp.isEmpty() && ispRoom >= min(4, (int)isp.length())) {
        shown += " | " + isp.substring(0, ispRoom);
    }
    // A long gateway name could still overflow; cut it and any separator left dangling
    shown = shown.substring(0, maxChars);
    shown.trim();
    if (shown.endsWith("|")) {
        shown.remove(shown.length() - 1);
        shown.trim();
    }
    tft.setCursor(10, 12);
    tft.print(shown);
}

void drawTabBar() {
    const char* tabNames[TAB_COUNT] = {"Overview", "Clients", "Devices", "Ports"};
    int tabWidth = SCREEN_WIDTH / TAB_COUNT;

    for (int i = 1; i <= TAB_COUNT; i++) {
        int x = (i - 1) * tabWidth;
        
        // Draw tab background
        if (i == currentTab) {
            tft.fillRect(x, TAB_BAR_Y, tabWidth, TAB_BAR_HEIGHT, 0x2104);
            tft.drawRect(x, TAB_BAR_Y, tabWidth, TAB_BAR_HEIGHT, COLOR_BORDER);
        } else {
            tft.fillRect(x, TAB_BAR_Y, tabWidth, TAB_BAR_HEIGHT, COLOR_BG);
            tft.drawRect(x, TAB_BAR_Y, tabWidth, TAB_BAR_HEIGHT, 0x2104);
        }
        
        // Draw tab text
        tft.setTextColor(i == currentTab ? TFT_WHITE : 0x8410);
        tft.setTextSize(2);
        
        String tabText = tabNames[i - 1];

        int textWidth = tabText.length() * 12;
        tft.setCursor(x + (tabWidth - textWidth) / 2, TAB_BAR_Y + 7);
        tft.print(tabText);
    }
}

void drawStaticElements() {
    // Draw graph panel
    tft.drawRoundRect(GRAPH_X, GRAPH_Y, GRAPH_WIDTH, GRAPH_HEIGHT, 8, COLOR_BORDER);
    tft.fillRoundRect(GRAPH_X+1, GRAPH_Y+1, GRAPH_WIDTH-2, GRAPH_HEIGHT-2, 8, COLOR_PANEL_BG);
    
    // Graph title
    tft.setTextColor(COLOR_TEXT_SECONDARY);
    tft.setTextSize(1);
    tft.setCursor(GRAPH_X + 10, GRAPH_Y + 5);
    tft.print("Network Throughput (Mbps)");
    
    // Draw speed panel
    tft.drawRoundRect(SPEED_BOX_X, SPEED_BOX_Y, SPEED_BOX_WIDTH, SPEED_BOX_HEIGHT, 8, COLOR_BORDER);
    tft.fillRoundRect(SPEED_BOX_X+1, SPEED_BOX_Y+1, SPEED_BOX_WIDTH-2, SPEED_BOX_HEIGHT-2, 8, COLOR_PANEL_BG);
    
    // Draw stats panels
    for (int i = 0; i < 4; i++) {
        int x = i * (SCREEN_WIDTH / 4) + 5;
        tft.drawRoundRect(x, STATS_Y, (SCREEN_WIDTH / 4) - 10, STATS_HEIGHT - 10, 6, COLOR_BORDER);
        tft.fillRoundRect(x+1, STATS_Y+1, (SCREEN_WIDTH / 4) - 12, STATS_HEIGHT - 12, 6, COLOR_PANEL_BG);
    }
}

void updateSpeedDisplay() {
    // Only update if values changed significantly
    bool downloadChanged = abs(smoothDownload - prevDrawnDownload) >= 0.01;
    bool uploadChanged = abs(smoothUpload - prevDrawnUpload) >= 0.01;
    
    if (!downloadChanged && !uploadChanged) {
        return;
    }
    
    int centerX = SPEED_BOX_X + SPEED_BOX_WIDTH / 2;
    
    // Update speeds
    if (downloadChanged) {
        prevDrawnDownload = smoothDownload;
        
        tft.fillRect(SPEED_BOX_X + 10, SPEED_BOX_Y + 20, SPEED_BOX_WIDTH - 20, 35, COLOR_PANEL_BG);
        
        tft.setTextColor(COLOR_DOWNLOAD);
        tft.setTextSize(3);
        String downStr = String(smoothDownload, 1);
        tft.setCursor(centerX - downStr.length() * 9, SPEED_BOX_Y + 25);
        tft.print(downStr);
        
        tft.setTextColor(COLOR_TEXT_SECONDARY);
        tft.setTextSize(1);
        tft.setCursor(centerX - 35, SPEED_BOX_Y + 55);
        tft.print("Download Mbps");
    }
    
    if (uploadChanged) {
        prevDrawnUpload = smoothUpload;
        
        tft.fillRect(SPEED_BOX_X + 10, SPEED_BOX_Y + 80, SPEED_BOX_WIDTH - 20, 35, COLOR_PANEL_BG);
        
        tft.setTextColor(COLOR_UPLOAD);
        tft.setTextSize(3);
        String upStr = String(smoothUpload, 1);
        tft.setCursor(centerX - upStr.length() * 9, SPEED_BOX_Y + 85);
        tft.print(upStr);
        
        tft.setTextColor(COLOR_TEXT_SECONDARY);
        tft.setTextSize(1);
        tft.setCursor(centerX - 30, SPEED_BOX_Y + 115);
        tft.print("Upload Mbps");
    }
}

void updateGraphDisplay() {
    // Clear graph area
    int graphAreaY = GRAPH_Y + 20;
    int graphAreaHeight = GRAPH_HEIGHT - 25;
    tft.fillRect(GRAPH_X + 10, graphAreaY, GRAPH_WIDTH - 20, graphAreaHeight, COLOR_PANEL_BG);
    
    // Auto-scale
    float maxVal = 0.1;
    for (int i = 0; i < GRAPH_POINTS; i++) {
        if (downloadHistory[i] > maxVal) maxVal = downloadHistory[i];
        if (uploadHistory[i] > maxVal) maxVal = uploadHistory[i];
    }
    
    if (smoothDownload > maxVal) maxVal = smoothDownload;
    if (smoothUpload > maxVal) maxVal = smoothUpload;
    
    maxVal = maxVal * 1.3;
    if (maxVal < 0.5) maxVal = 0.5;
    
    // Draw grid
    for (int i = 0; i <= 4; i++) {
        int y = graphAreaY + (graphAreaHeight * i / 4);
        for (int x = GRAPH_X + 10; x < GRAPH_X + GRAPH_WIDTH - 10; x += 6) {
            tft.drawPixel(x, y, COLOR_GRID);
        }
    }
    
    // Draw curves
    for (int i = 1; i < GRAPH_POINTS; i++) {
        int x1 = GRAPH_X + 10 + ((i-1) * (GRAPH_WIDTH - 20) / GRAPH_POINTS);
        int x2 = GRAPH_X + 10 + (i * (GRAPH_WIDTH - 20) / GRAPH_POINTS);
        
        // Download line
        int y1 = graphAreaY + graphAreaHeight - (downloadHistory[i-1] * graphAreaHeight / maxVal);
        int y2 = graphAreaY + graphAreaHeight - (downloadHistory[i] * graphAreaHeight / maxVal);
        tft.drawLine(x1, y1, x2, y2, COLOR_DOWNLOAD);
        
        // Upload line
        y1 = graphAreaY + graphAreaHeight - (uploadHistory[i-1] * graphAreaHeight / maxVal);
        y2 = graphAreaY + graphAreaHeight - (uploadHistory[i] * graphAreaHeight / maxVal);
        tft.drawLine(x1, y1, x2, y2, COLOR_UPLOAD);
    }
}

void scrollGraphData() {
    // Shift data left
    for (int i = 0; i < GRAPH_POINTS - 1; i++) {
        downloadHistory[i] = downloadHistory[i + 1];
        uploadHistory[i] = uploadHistory[i + 1];
    }
    
    // Add new data
    downloadHistory[GRAPH_POINTS - 1] = smoothDownload;
    uploadHistory[GRAPH_POINTS - 1] = smoothUpload;
}

// 10x10 download (down, blue) or upload (up, purple) arrow with its top-left corner at x, y
void drawArrow(int x, int y, bool isDownload) {
    if (isDownload) {
        tft.fillTriangle(x, y, x + 10, y, x + 5, y + 10, COLOR_DOWNLOAD);
    } else {
        tft.fillTriangle(x, y + 10, x + 10, y + 10, x + 5, y, COLOR_UPLOAD);
    }
}

// One row of a data-usage box: colored arrow, then the amount in GB (or TB once it passes 1000 GB)
void drawUsageRow(int x, int y, uint64_t bytes, bool isDownload, bool valid) {
    drawArrow(x + 10, y + 3, isDownload);

    tft.setTextColor(COLOR_STAT_VALUE);
    tft.setTextSize(2);
    tft.setCursor(x + 26, y);

    if (!valid) {
        tft.print("--");
        return;
    }

    double gb = bytes / 1e9;
    String unit = " GB";
    if (gb >= 1000) {
        tft.print(String(gb / 1000.0, 2));
        unit = " TB";
    } else if (gb >= 100) {
        tft.print(String((int)gb));
    } else {
        tft.print(String(gb, 1));
    }
    tft.setTextSize(1);
    tft.print(unit);
}

// One row of a two-row box: small label, then the value and an optional unit
void drawLabeledRow(int x, int y, const char* label, const String& value, const char* unit) {
    tft.setTextColor(COLOR_TEXT_SECONDARY);
    tft.setTextSize(1);
    tft.setCursor(x + 10, y + 4);
    tft.print(label);

    tft.setTextColor(COLOR_STAT_VALUE);
    tft.setTextSize(2);
    tft.setCursor(x + 40, y);
    tft.print(value);
    tft.setTextSize(1);
    tft.print(unit);
}

void updateStatsBar() {
    // Each box shows a title and two stacked rows
    const char* titles[4] = {"Daily", "Monthly", "Network", "Clients"};
    const int row1Y = STATS_Y + 27;
    const int row2Y = STATS_Y + 50;

    for (int i = 0; i < 4; i++) {
        int x = i * (SCREEN_WIDTH / 4) + 5;
        
        // Clear both value rows
        tft.fillRect(x + 5, STATS_Y + 25, (SCREEN_WIDTH / 4) - 20, 45, COLOR_PANEL_BG);
        
        tft.setTextColor(COLOR_TEXT_SECONDARY);
        tft.setTextSize(1);
        tft.setCursor(x + 10, STATS_Y + 10);
        tft.print(titles[i]);
        
        switch(i) {
            case 0:  // Today's data usage - download over upload
                drawUsageRow(x, row1Y, currentStats.day_usage.rx_bytes, true, currentStats.day_usage.valid);
                drawUsageRow(x, row2Y, currentStats.day_usage.tx_bytes, false, currentStats.day_usage.valid);
                break;
                
            case 1:  // This month's data usage - download over upload
                drawUsageRow(x, row1Y, currentStats.month_usage.rx_bytes, true, currentStats.month_usage.valid);
                drawUsageRow(x, row2Y, currentStats.month_usage.tx_bytes, false, currentStats.month_usage.valid);
                break;
                
            case 2: { // Ping over gateway CPU/memory
                // Whole milliseconds once ping reaches 10 so the row fits the box
                float ping = currentStats.ping_ms;
                drawLabeledRow(x, row1Y, "Ping", ping >= 10 ? String((int)ping) : String(ping, 1), " ms");

                // Whole percents as "cpu/mem" - five characters fit at text size 2
                if (currentStats.gateway_cpu >= 0 && currentStats.gateway_mem >= 0) {
                    String load = String((int)lroundf(currentStats.gateway_cpu)) + "/" +
                                  String((int)lroundf(currentStats.gateway_mem));
                    // Skip the unit if 100% would push the row past the box edge
                    drawLabeledRow(x, row2Y, "C/M", load, load.length() <= 5 ? "%" : "");
                } else {
                    drawLabeledRow(x, row2Y, "C/M", "--");
                }
                break;
            }
                
            case 3:  // Connected clients - wired over wireless
                drawLabeledRow(x, row1Y, "LAN", String(currentStats.wired_devices));
                drawLabeledRow(x, row2Y, "WiFi", String(currentStats.wireless_devices));
                break;
        }
    }
}

// Top-left corner of a Devices tab cell; cells fill the left column, then the right
void deviceCellOrigin(int cell, int& x, int& y) {
    x = (cell / DEVICE_ROWS) * (SCREEN_WIDTH / DEVICE_COLS);
    y = DEVICE_TOP + (cell % DEVICE_ROWS) * DEVICE_ROW_HEIGHT;
}

// Green when connected, red when the controller has lost it, yellow while upgrading/adopting
uint16_t deviceStateColor(int state) {
    if (state == 1) return COLOR_SUCCESS;
    // 0 disconnected, 6 heartbeat missed, 9 adoption failed, 10 isolated
    if (state == 0 || state == 6 || state == 9 || state == 10) return COLOR_ERROR;
    return COLOR_WARNING;
}

// Status dot and name, with the model and client count in small text underneath
void drawDeviceCell(int cell, const UniFiDevice& dev) {
    int x, y;
    deviceCellOrigin(cell, x, y);

    tft.fillCircle(x + 14, y + 8, 5, deviceStateColor(dev.state));

    tft.setTextColor(COLOR_TEXT_PRIMARY);
    tft.setTextSize(2);
    tft.setCursor(x + 26, y);
    tft.print(dev.name.substring(0, 17));  // 17 characters fill the column

    tft.setTextColor(COLOR_TEXT_SECONDARY);
    tft.setTextSize(1);
    tft.setCursor(x + 26, y + 18);
    tft.print(dev.model);

    String clients = String(dev.num_sta) + (dev.num_sta == 1 ? " client" : " clients");
    tft.setCursor(x + (SCREEN_WIDTH / DEVICE_COLS) - 8 - clients.length() * 6, y + 18);
    tft.print(clients);
}

// The last cell: how many devices are online, plus a note (updating, failed, more not shown)
void drawDeviceSummary(const String& note) {
    int x, y;
    deviceCellOrigin(DEVICE_COLS * DEVICE_ROWS - 1, x, y);
    tft.fillRect(x, y, SCREEN_WIDTH / DEVICE_COLS, DEVICE_ROW_HEIGHT, COLOR_BG);

    tft.setTextSize(2);
    tft.setCursor(x + 26, y);
    if (deviceTotal >= 0) {
        int online = 0;
        for (int i = 0; i < min(deviceTotal, MAX_DEVICES_TRACKED); i++) {
            if (devices[i].state == 1) online++;
        }
        tft.setTextColor(online == deviceTotal ? COLOR_SUCCESS : COLOR_WARNING);
        tft.print(String(online) + "/" + String(deviceTotal) + " online");
    }

    tft.setTextColor(COLOR_TEXT_SECONDARY);
    tft.setTextSize(1);
    tft.setCursor(x + 26, y + 18);
    tft.print(note);
}

void drawDevicesTab() {
    tft.fillRect(0, CONTENT_START_Y, SCREEN_WIDTH, SCREEN_HEIGHT - CONTENT_START_Y, COLOR_BG);

    // All but the last cell show devices; the last cell is the summary
    const int deviceCells = DEVICE_COLS * DEVICE_ROWS - 1;
    int shown = min(deviceTotal, deviceCells);
    for (int i = 0; i < shown; i++) {
        drawDeviceCell(i, devices[i]);
    }

    String note = "";
    if (deviceTotal < 0) {
        note = deviceFetchFailed ? "Update failed" : "Loading...";
    } else if (deviceFetchFailed) {
        note = "Update failed - showing last list";
    } else if (deviceTotal > deviceCells) {
        note = "+" + String(deviceTotal - deviceCells) + " more not shown";
    }
    drawDeviceSummary(note);
}

uint16_t portSpeedColor(int mbps) {
    if (mbps >= 10000) return COLOR_PORT_10G;
    if (mbps >= 2500) return COLOR_PORT_2G5;
    if (mbps >= 1000) return COLOR_PORT_1G;
    return COLOR_PORT_100M;
}

// Device name and link count, then a square per port: filled by link speed, hollow when down
void drawPortRow(int row, const UniFiDevice& dev) {
    int y = PORT_TOP + row * PORT_ROW_HEIGHT;

    tft.setTextSize(1);
    tft.setTextColor(COLOR_TEXT_PRIMARY);
    tft.setCursor(10, y + 1);
    tft.print(dev.name.substring(0, 21));  // 21 characters reach the port strip
    tft.setTextColor(COLOR_TEXT_SECONDARY);
    tft.setCursor(10, y + 11);
    tft.print(String(dev.port_up) + "/" + String(dev.port_count) + " up");

    int shown = min(dev.port_count, MAX_DEVICE_PORTS);
    for (int p = 0; p < shown; p++) {
        int x = PORT_STRIP_X + p * PORT_PITCH;
        if (dev.port_speed[p] > 0) {
            tft.fillRect(x, y + 3, 10, 13, portSpeedColor(dev.port_speed[p]));
        } else {
            tft.drawRect(x, y + 3, 10, 13, COLOR_BORDER);
        }
        // White outline marks the port leading back toward the gateway
        if (dev.uplink_ports & (1UL << p)) {
            tft.drawRect(x - 1, y + 2, 12, 15, TFT_WHITE);
        }
    }
}

// Speed legend along the bottom, with a note (updating, failed, more not shown) at the right
void drawPortLegend(const String& note) {
    tft.fillRect(0, PORT_LEGEND_Y - 2, SCREEN_WIDTH, 12, COLOR_BG);
    tft.setTextSize(1);
    tft.setTextColor(COLOR_TEXT_SECONDARY);

    const char* labels[] = {"10G", "2.5G", "1G", "10/100", "down", "uplink"};
    const uint16_t colors[] = {COLOR_PORT_10G, COLOR_PORT_2G5, COLOR_PORT_1G, COLOR_PORT_100M};
    int x = 10;
    for (int i = 0; i < 6; i++) {
        if (i < 4) {
            tft.fillRect(x, PORT_LEGEND_Y, 8, 8, colors[i]);
        } else {
            tft.drawRect(x, PORT_LEGEND_Y, 8, 8, i == 4 ? COLOR_BORDER : TFT_WHITE);
        }
        tft.setCursor(x + 12, PORT_LEGEND_Y);
        tft.print(labels[i]);
        x += 12 + strlen(labels[i]) * 6 + 12;
    }

    tft.setCursor(SCREEN_WIDTH - 10 - note.length() * 6, PORT_LEGEND_Y);
    tft.print(note);
}

void drawPortsTab() {
    tft.fillRect(0, CONTENT_START_Y, SCREEN_WIDTH, SCREEN_HEIGHT - CONTENT_START_Y, COLOR_BG);

    // One row per device that has ports, in the Devices tab order
    int rows = 0;
    int withPorts = 0;
    for (int i = 0; i < min(deviceTotal, MAX_DEVICES_TRACKED); i++) {
        if (devices[i].port_count == 0) continue;
        withPorts++;
        if (rows < PORT_ROWS) {
            drawPortRow(rows++, devices[i]);
        }
    }

    String note = "";
    if (deviceTotal < 0) {
        note = deviceFetchFailed ? "Update failed" : "Loading...";
    } else if (deviceFetchFailed) {
        note = "Update failed (old data)";
    } else if (withPorts > rows) {
        note = "+" + String(withPorts - rows) + " more not shown";
    }
    drawPortLegend(note);
}

// Clients tab layout: a large top-talkers panel over a short newest-clients panel
#define CLIENT_PANEL_X 10
#define CLIENT_PANEL_W (SCREEN_WIDTH - 20)
#define TALKERS_Y (CONTENT_START_Y + 10)
#define TALKERS_H 165
#define TALKER_ROW_Y (TALKERS_Y + 22)
#define TALKER_ROW_HEIGHT 28
#define NEWEST_Y (TALKERS_Y + TALKERS_H + 8)
#define NEWEST_H (SCREEN_HEIGHT - NEWEST_Y - 6)

// Largest whole unit: "45s", "12m", "3h", "5d"
String formatDuration(unsigned long seconds) {
    if (seconds < 60) return String(seconds) + "s";
    if (seconds < 3600) return String(seconds / 60) + "m";
    if (seconds < 86400) return String(seconds / 3600) + "h";
    return String(seconds / 86400) + "d";
}

// Rounded panel in the Overview style, with a small gray title
void drawPanel(int x, int y, int w, int h, const char* title) {
    tft.drawRoundRect(x, y, w, h, 8, COLOR_BORDER);
    tft.fillRoundRect(x + 1, y + 1, w - 2, h - 2, 8, COLOR_PANEL_BG);
    tft.setTextColor(COLOR_TEXT_SECONDARY);
    tft.setTextSize(1);
    tft.setCursor(x + 10, y + 5);
    tft.print(title);
}

// Colored arrow, then the rate in the Overview's stat style: "11.1 Mbps", "752 Kbps"
void drawRate(int x, int y, float bytesPerSec, bool isDownload) {
    drawArrow(x, y + 3, isDownload);
    
    float bits = bytesPerSec * 8;
    String value, unit;
    if (bits >= 1e6) {
        float mbps = bits / 1e6;
        value = mbps >= 100 ? String((int)mbps) : String(mbps, 1);
        unit = " Mbps";
    } else if (bits >= 1e3) {
        value = String((int)(bits / 1e3));
        unit = " Kbps";
    } else {
        value = String((int)bits);
        unit = " bps";
    }
    
    tft.setTextColor(COLOR_STAT_VALUE);
    tft.setTextSize(2);
    tft.setCursor(x + 16, y);
    tft.print(value);
    tft.setTextSize(1);
    tft.print(unit);
}

// Status note at the right of the top-talkers title (updating, failed)
void drawClientsNote(const String& note) {
    tft.fillRect(CLIENT_PANEL_X + 200, TALKERS_Y + 4, CLIENT_PANEL_W - 210, 10, COLOR_PANEL_BG);
    tft.setTextColor(COLOR_TEXT_SECONDARY);
    tft.setTextSize(1);
    tft.setCursor(CLIENT_PANEL_X + CLIENT_PANEL_W - 10 - note.length() * 6, TALKERS_Y + 5);
    tft.print(note);
}

void drawClientsTab() {
    tft.fillRect(0, CONTENT_START_Y, SCREEN_WIDTH, SCREEN_HEIGHT - CONTENT_START_Y, COLOR_BG);
    drawPanel(CLIENT_PANEL_X, TALKERS_Y, CLIENT_PANEL_W, TALKERS_H, "Top Talkers");
    drawPanel(CLIENT_PANEL_X, NEWEST_Y, CLIENT_PANEL_W, NEWEST_H, "Newest Clients");
    
    const ClientSummary& s = clientSummary;
    if (clientsLoaded) {
        // Name, then download and upload in fixed columns
        for (int i = 0; i < s.top_count; i++) {
            const ClientInfo& c = s.top[i];
            int y = TALKER_ROW_Y + i * TALKER_ROW_HEIGHT;
            tft.setTextColor(COLOR_TEXT_PRIMARY);
            tft.setTextSize(2);
            tft.setCursor(CLIENT_PANEL_X + 10, y);
            tft.print(c.name.substring(0, 18));  // 18 characters reach the download column
            drawRate(CLIENT_PANEL_X + 245, y, c.down_bps, true);
            drawRate(CLIENT_PANEL_X + 350, y, c.up_bps, false);
        }
        if (s.top_count == 0) {
            tft.setTextColor(COLOR_TEXT_SECONDARY);
            tft.setTextSize(1);
            tft.setCursor(CLIENT_PANEL_X + 10, TALKER_ROW_Y);
            tft.print("No traffic right now");
        }
        
        // Name, with how long ago it connected right-aligned
        for (int i = 0; i < s.newest_count; i++) {
            const ClientInfo& c = s.newest[i];
            int y = NEWEST_Y + 18 + i * 20;
            String ago = formatDuration(c.uptime) + " ago";
            int maxChars = (CLIENT_PANEL_W - 20 - ago.length() * 12 - 12) / 12;
            tft.setTextColor(COLOR_TEXT_PRIMARY);
            tft.setTextSize(2);
            tft.setCursor(CLIENT_PANEL_X + 10, y);
            tft.print(c.name.substring(0, maxChars));
            tft.setTextColor(COLOR_STAT_VALUE);
            tft.setCursor(CLIENT_PANEL_X + CLIENT_PANEL_W - 10 - ago.length() * 12, y);
            tft.print(ago);
        }
    }
    
    String note = "";
    if (!clientsLoaded) {
        note = clientFetchFailed ? "Update failed" : "Loading...";
    } else if (clientFetchFailed) {
        note = "Update failed (old data)";
    }
    drawClientsNote(note);
}
