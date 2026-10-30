#include <Arduino.h>
#include "unifi_api.h"
#include "config.h"
#include "credentials.h"
#include <HTTPClient.h>
#include <StreamUtils.h>

// Set in credentials.h; default to skipping verification for UniFi's self-signed cert
#ifndef UNIFI_SKIP_SSL_VERIFY
#define UNIFI_SKIP_SSL_VERIFY true
#endif

UniFiAPI::UniFiAPI() : authenticated(false), lastAuthTime(0),
                       dayUsage{0, 0, false}, monthUsage{0, 0, false}, lastUsageFetch(0),
                       vpnDown(-1), lastVpnFetch(0) {
    #if UNIFI_SKIP_SSL_VERIFY
    client.setInsecure();
    #endif
}

UniFiAPI::~UniFiAPI() {
    disconnect();
}

bool UniFiAPI::connect(String url, String key) {
    baseUrl = url;
    apiKey = key;
    
    // Remove trailing slash if present
    if (baseUrl.endsWith("/")) {
        baseUrl = baseUrl.substring(0, baseUrl.length() - 1);
    }
    
    return authenticate();
}

// Open a request with the API key attached
bool UniFiAPI::beginRequest(HTTPClient& https, const String& url) {
    if (!https.begin(client, url)) {
        return false;
    }
    https.addHeader("X-API-KEY", apiKey);
    https.addHeader("Accept", "application/json");
    return true;
}

// API keys don't need a login; verify the key with a lightweight request
bool UniFiAPI::authenticate() {
    HTTPClient https;
    lastAuthTime = millis();
    
    #if DEBUG_API
    Serial.println("Verifying UniFi API key...");
    Serial.println("URL: " + baseUrl + UNIFI_API_SYSINFO);
    #endif
    
    if (!beginRequest(https, baseUrl + UNIFI_API_SYSINFO)) {
        Serial.println("Failed to begin HTTPS connection");
        authenticated = false;
        return false;
    }
    
    int httpCode = https.GET();
    authenticated = (httpCode == HTTP_CODE_OK);
    
    #if DEBUG_API
    if (authenticated) {
        Serial.println("API key accepted");
    } else {
        Serial.printf("API key check failed. HTTP code: %d\n", httpCode);
    }
    #endif
    
    https.end();
    return authenticated;
}

bool UniFiAPI::isConnected() {
    // API keys don't expire, but retry the check if it failed (e.g. controller was down)
    if (!authenticated && !apiKey.isEmpty() && (millis() - lastAuthTime > API_RETRY_DELAY)) {
        authenticate();
    }
    return authenticated;
}

void UniFiAPI::disconnect() {
    authenticated = false;
}

// Main function to get all network stats
NetworkStats UniFiAPI::getNetworkStats() {
    NetworkStats stats = {0};
    stats.timestamp = millis();
    stats.data_source = "Data/Mo";  // Initialize data source label with default
    stats.connected_devices = 0;    // Explicitly initialize device count
    stats.uptime_percent = 0;       // Initialize uptime
    stats.gateway_cpu = -1;         // -1 shows "--" until the gateway reports
    stats.gateway_mem = -1;

    if (!isConnected()) {
        Serial.println("Not connected to UniFi");
        return stats;
    }
    
    Serial.println("\n=== Getting Network Stats ===");

    // One small request for throughput, ping, uptime, gateway load and client counts
    getHealthStats(stats);

    // Usage totals change slowly, so refresh them on a longer interval
    if (!dayUsage.valid || !monthUsage.valid || millis() - lastUsageFetch >= USAGE_REPORT_INTERVAL) {
        lastUsageFetch = millis();
        fetchUsageReport(UNIFI_API_DAILY, dayUsage);
        fetchUsageReport(UNIFI_API_MONTHLY, monthUsage);
    }
    stats.day_usage = dayUsage;
    stats.month_usage = monthUsage;

    #if SHOW_VPN_STATUS
    // Tunnel state rarely changes; a failed fetch also waits a full interval before retrying
    if (lastVpnFetch == 0 || millis() - lastVpnFetch >= VPN_STATUS_INTERVAL) {
        lastVpnFetch = millis();
        fetchVpnStatus();
    }
    #endif
    stats.vpn_down = vpnDown;

    Serial.println("Final data: " + String(stats.connected_devices) + " devices, " + String(stats.uptime_percent) + "% uptime, gateway CPU " + String(stats.gateway_cpu) + "% mem " + String(stats.gateway_mem) + "%");
    
    // Always show final stats
    Serial.printf("Final Stats: Down: %.2f Mbps, Up: %.2f Mbps, Devices: %d, Ping: %.1f\n",
                 stats.download_mbps, stats.upload_mbps, stats.connected_devices, stats.ping_ms);
    Serial.printf("  Today: %.1f/%.1f GB, Month: %.1f/%.1f GB (down/up), Uptime: %.1f%%\n",
                 stats.day_usage.rx_bytes / 1e9, stats.day_usage.tx_bytes / 1e9,
                 stats.month_usage.rx_bytes / 1e9, stats.month_usage.tx_bytes / 1e9,
                 stats.uptime_percent);
    
    return stats;
}

// Everything the dashboard shows each cycle comes from the ~2.5 KB health endpoint:
// WAN throughput, latency, availability, gateway CPU/memory, and client counts
void UniFiAPI::getHealthStats(NetworkStats& stats) {
    HTTPClient https;
    String healthUrl = baseUrl + UNIFI_API_HEALTH;
    
    if (!beginRequest(https, healthUrl)) {
        Serial.println("Failed to begin HTTPS for health");
        return;
    }
    
    int httpCode = https.GET();
    if (httpCode == HTTP_CODE_OK) {
        String response = https.getString();
        
        JsonDocument doc;
        if (!deserializeJson(doc, response) && doc["meta"]["rc"] == "ok") {
            int wanClients = -1;
            int lanUsers = 0;
            int wlanUsers = 0;
            
            for (JsonObject h : doc["data"].as<JsonArray>()) {
                String subsystem = h["subsystem"] | "";
                
                if (subsystem == "wan") {
                    // rx_bytes-r and tx_bytes-r are bytes per second
                    stats.download_mbps = (h["rx_bytes-r"] | 0.0f) * 8.0f / 1000000.0f;
                    stats.upload_mbps = (h["tx_bytes-r"] | 0.0f) * 8.0f / 1000000.0f;
                    stats.wan_status = h["status"] | "UNKNOWN";
                    stats.wan_ip = h["wan_ip"] | "";
                    stats.gateway_name = h["gw_name"] | "";
                    stats.isp_name = h["isp_name"] | "";
                    
                    // Latency and availability from uptime_stats
                    JsonObject wan = h["uptime_stats"]["WAN"];
                    if (!wan.isNull()) {
                        stats.ping_ms = wan["latency_average"] | 0.0f;
                        stats.uptime_percent = wan["availability"] | 0.0f;
                    }
                    
                    // The UDM reports these as strings ("28.2"), so parse either form
                    JsonObject gw = h["gw_system-stats"];
                    if (!gw.isNull()) {
                        stats.gateway_cpu = gw["cpu"].is<const char*>() ? atof(gw["cpu"]) : (gw["cpu"] | -1.0f);
                        stats.gateway_mem = gw["mem"].is<const char*>() ? atof(gw["mem"]) : (gw["mem"] | -1.0f);
                    }
                    
                    if (!h["num_sta"].isNull()) {
                        // Total of all clients on the site
                        wanClients = h["num_sta"].as<int>();
                    }
                    stats.devices_offline += h["num_disconnected"] | 0;
                } else if (subsystem == "lan") {
                    lanUsers = (h["num_user"] | 0) + (h["num_guest"] | 0);
                    stats.devices_offline += h["num_disconnected"] | 0;
                } else if (subsystem == "wlan") {
                    wlanUsers = (h["num_user"] | 0) + (h["num_guest"] | 0);
                    stats.devices_offline += h["num_disconnected"] | 0;
                } else if (subsystem == "www") {
                    stats.www_status = h["status"] | "";
                }
            }
            stats.health_ok = true;

            stats.connected_devices = wanClients >= 0 ? wanClients : lanUsers + wlanUsers;
            stats.wired_devices = lanUsers;
            stats.wireless_devices = wlanUsers;
            
            #if DEBUG_API
            Serial.printf("  Health: %.2f/%.2f Mbps, %.0f ms, %.1f%% up, WAN %s\n",
                          stats.download_mbps, stats.upload_mbps, stats.ping_ms,
                          stats.uptime_percent, stats.wan_status.c_str());
            Serial.printf("  Clients: %d (wan num_sta %d, lan %d, wlan %d)\n",
                          stats.connected_devices, wanClients, lanUsers, wlanUsers);
            Serial.printf("  Status: www %s, %d devices offline\n",
                          stats.www_status.c_str(), stats.devices_offline);
            #endif
        } else {
            Serial.println("Health response could not be parsed");
        }
    } else {
        Serial.println("Health request failed. HTTP code: " + String(httpCode));
        if (httpCode == 401) {
            Serial.println("API key rejected - check UNIFI_API_KEY");
            authenticated = false;  // isConnected() will re-check after API_RETRY_DELAY
        }
    }
    
    https.end();
}

// WAN download/upload totals for the current period of a daily or monthly site report
void UniFiAPI::fetchUsageReport(const char* path, UsageTotals& usage) {
    HTTPClient https;
    String reportUrl = baseUrl + path;
    
    if (!beginRequest(https, reportUrl)) {
        Serial.println("Failed to begin HTTPS for usage report " + String(path));
        return;
    }
    https.addHeader("Content-Type", "application/json");
    
    // With no start/end, the controller returns recent periods ending with the current one
    int httpCode = https.POST("{\"attrs\":[\"wan-rx_bytes\",\"wan-tx_bytes\",\"time\"]}");
    if (httpCode == HTTP_CODE_OK) {
        String response = https.getString();
        
        JsonDocument doc;
        if (!deserializeJson(doc, response) && doc["meta"]["rc"] == "ok") {
            // The newest entry is the current day or month
            double latestTime = -1;
            for (JsonObject r : doc["data"].as<JsonArray>()) {
                double t = r["time"] | 0.0;
                if (t > latestTime) {
                    latestTime = t;
                    // Byte counts arrive as doubles and exceed 32 bits
                    usage.rx_bytes = (uint64_t)(r["wan-rx_bytes"] | 0.0);
                    usage.tx_bytes = (uint64_t)(r["wan-tx_bytes"] | 0.0);
                    usage.valid = true;
                }
            }
            
            #if DEBUG_API
            Serial.printf("  Usage %s: %.1f GB down, %.1f GB up\n",
                          path, usage.rx_bytes / 1e9, usage.tx_bytes / 1e9);
            #endif
        }
    } else {
        Serial.println("Usage report request failed. HTTP code: " + String(httpCode));
    }
    
    https.end();
}

// Count site-to-site VPN tunnels that aren't connected; leaves vpnDown at -1 if the request fails
void UniFiAPI::fetchVpnStatus() {
    HTTPClient https;
    String vpnUrl = baseUrl + UNIFI_API_VPN;

    if (!beginRequest(https, vpnUrl)) {
        Serial.println("Failed to begin HTTPS for VPN status");
        return;
    }

    int httpCode = https.GET();
    if (httpCode == HTTP_CODE_OK) {
        String response = https.getString();

        // v2 responses have no meta/rc wrapper: {"connections":[{"status":"CONNECTED",...}]}
        JsonDocument doc;
        if (!deserializeJson(doc, response)) {
            int total = 0;
            int down = 0;
            for (JsonObject conn : doc["connections"].as<JsonArray>()) {
                total++;
                String status = conn["status"] | "";
                if (status != "CONNECTED") {
                    down++;
                }
            }
            vpnDown = down;

            #if DEBUG_API
            Serial.printf("  VPN: %d of %d tunnels connected\n", total - down, total);
            #endif
        } else {
            Serial.println("VPN status response could not be parsed");
        }
    } else {
        Serial.println("VPN status request failed. HTTP code: " + String(httpCode));
        vpnDown = -1;
    }

    https.end();
}

// Devices tab order: gateway first, then switches, APs, everything else
static int deviceTypeRank(const String& type) {
    if (type == "udm" || type == "ugw" || type == "uxg") return 0;
    if (type == "usw") return 1;
    if (type == "uap") return 2;
    return 3;
}

int UniFiAPI::getDevices(UniFiDevice* devices, int maxDevices) {
    if (!isConnected()) {
        return -1;
    }

    HTTPClient https;
    String devicesUrl = baseUrl + UNIFI_API_DEVICES;

    // The body is read straight off the socket, so ask for HTTP/1.0 to avoid chunked encoding
    https.useHTTP10(true);
    if (!beginRequest(https, devicesUrl)) {
        Serial.println("Failed to begin HTTPS for device list");
        return -1;
    }

    unsigned long start = millis();
    int httpCode = https.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.println("Device list request failed. HTTP code: " + String(httpCode));
        if (httpCode == 401) {
            authenticated = false;  // isConnected() will re-check after API_RETRY_DELAY
        }
        https.end();
        return -1;
    }

    // Parse as the response streams in, keeping only these fields of each device
    JsonDocument filter;
    JsonObject fields = filter["network_devices"][0].to<JsonObject>();
    fields["name"] = true;
    fields["model"] = true;
    fields["type"] = true;
    fields["state"] = true;
    fields["num_sta"] = true;
    JsonObject portFields = fields["port_table"][0].to<JsonObject>();
    portFields["up"] = true;
    portFields["speed"] = true;
    portFields["is_uplink"] = true;

    // Buffer reads so the parser isn't pulling one byte at a time through TLS
    ReadBufferingStream stream(https.getStream(), 1024);
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, stream, DeserializationOption::Filter(filter));
    https.end();

    if (error) {
        Serial.println("Device list could not be parsed: " + String(error.c_str()));
        return -1;
    }

    int total = 0;
    for (JsonObject d : doc["network_devices"].as<JsonArray>()) {
        if (total < maxDevices) {
            UniFiDevice& dev = devices[total];
            dev.model = d["model"] | "";
            dev.name = d["name"] | "";
            if (dev.name.isEmpty()) {
                dev.name = dev.model;  // Unnamed devices show their model
            }
            dev.type = d["type"] | "";
            dev.state = d["state"] | 0;
            dev.num_sta = d["num_sta"] | 0;

            dev.port_count = 0;
            dev.port_up = 0;
            dev.uplink_ports = 0;
            for (JsonObject p : d["port_table"].as<JsonArray>()) {
                bool up = p["up"] | false;
                if (up) dev.port_up++;
                if (dev.port_count < MAX_DEVICE_PORTS) {
                    dev.port_speed[dev.port_count] = up ? (p["speed"] | 0) : 0;
                    if (p["is_uplink"] | false) {
                        dev.uplink_ports |= 1UL << dev.port_count;
                    }
                }
                dev.port_count++;
            }
        }
        total++;
    }

    // Insertion sort by type rank, then name
    int count = min(total, maxDevices);
    for (int i = 1; i < count; i++) {
        UniFiDevice dev = devices[i];
        int rank = deviceTypeRank(dev.type);
        int j = i - 1;
        while (j >= 0 && (deviceTypeRank(devices[j].type) > rank ||
                          (deviceTypeRank(devices[j].type) == rank && devices[j].name.compareTo(dev.name) > 0))) {
            devices[j + 1] = devices[j];
            j--;
        }
        devices[j + 1] = dev;
    }

    #if DEBUG_API
    Serial.printf("  Devices: %d loaded in %lu ms\n", total, millis() - start);
    #endif
    return total;
}

// Clients tab orderings for keepTop()
static bool busier(const ClientInfo& a, const ClientInfo& b) {
    return a.down_bps + a.up_bps > b.down_bps + b.up_bps;
}
static bool newer(const ClientInfo& a, const ClientInfo& b) { return a.uptime < b.uptime; }

// Insert c into list, kept in before() order and capped at cap entries
static void keepTop(ClientInfo* list, int& count, int cap, const ClientInfo& c,
                    bool (*before)(const ClientInfo&, const ClientInfo&)) {
    int pos = count;
    while (pos > 0 && before(c, list[pos - 1])) {
        pos--;
    }
    if (pos >= cap) {
        return;
    }
    for (int i = min(count, cap - 1); i > pos; i--) {
        list[i] = list[i - 1];
    }
    list[pos] = c;
    if (count < cap) {
        count++;
    }
}

bool UniFiAPI::getClients(ClientSummary& summary) {
    if (!isConnected()) {
        return false;
    }

    HTTPClient https;
    String clientsUrl = baseUrl + UNIFI_API_CLIENTS;

    // The body is read straight off the socket, so ask for HTTP/1.0 to avoid chunked encoding
    https.useHTTP10(true);
    if (!beginRequest(https, clientsUrl)) {
        Serial.println("Failed to begin HTTPS for client list");
        return false;
    }

    unsigned long start = millis();
    int httpCode = https.GET();
    if (httpCode != HTTP_CODE_OK) {
        Serial.println("Client list request failed. HTTP code: " + String(httpCode));
        if (httpCode == 401) {
            authenticated = false;  // isConnected() will re-check after API_RETRY_DELAY
        }
        https.end();
        return false;
    }

    // The response is a bare array of clients; keep only the fields the tab uses
    JsonDocument filter;
    JsonObject fields = filter[0].to<JsonObject>();
    fields["name"] = true;
    fields["hostname"] = true;
    fields["mac"] = true;
    fields["rx_bytes-r"] = true;
    fields["tx_bytes-r"] = true;
    fields["uptime"] = true;

    // Buffer reads so the parser isn't pulling one byte at a time through TLS
    ReadBufferingStream stream(https.getStream(), 1024);
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, stream, DeserializationOption::Filter(filter));
    https.end();

    if (error) {
        Serial.println("Client list could not be parsed: " + String(error.c_str()));
        return false;
    }

    ClientSummary s = ClientSummary();  // Value-initialized, so every count starts at 0
    for (JsonObject c : doc.as<JsonArray>()) {
        s.total++;

        ClientInfo info;
        // display_name adds MAC bytes to unnamed clients ("Macmini f9:b2"), so build the name here.
        // With no name or hostname, the full MAC lets you look the client up in the UniFi app
        info.name = c["name"] | "";
        if (info.name.isEmpty()) info.name = c["hostname"] | "";
        if (info.name.isEmpty()) info.name = c["mac"] | "";
        // Rates are from the network's side: tx is sent to the client, rx is received from it
        info.down_bps = c["tx_bytes-r"] | 0.0f;
        info.up_bps = c["rx_bytes-r"] | 0.0f;
        info.uptime = c["uptime"] | 0UL;

        if (info.down_bps + info.up_bps > 0) {
            keepTop(s.top, s.top_count, TOP_CLIENTS, info, busier);
        }
        keepTop(s.newest, s.newest_count, NEWEST_CLIENTS, info, newer);
    }

    #if DEBUG_API
    Serial.printf("  Clients: %d loaded in %lu ms\n", s.total, millis() - start);
    #endif
    summary = s;
    return true;
}

void UniFiAPI::setInsecure(bool insecure) {
    if (insecure) {
        client.setInsecure();
    }
}

bool UniFiAPI::testConnection() {
    return authenticate();
}