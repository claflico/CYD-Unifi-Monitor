#ifndef UNIFI_API_H
#define UNIFI_API_H

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// WAN data usage over one report period (today or this month)
struct UsageTotals {
    uint64_t rx_bytes;  // Download
    uint64_t tx_bytes;  // Upload
    bool valid;         // False until the report has loaded
};

struct NetworkStats {
    float download_mbps;
    float upload_mbps;
    int connected_devices;
    int wired_devices;       // Wired device count
    int wireless_devices;    // Wireless device count
    int stable_device_count; // Smoothed device count to reduce fluctuation
    unsigned long uptime_seconds;
    float ping_ms;
    float packet_loss;
    unsigned long total_bytes_today;
    UsageTotals day_usage;    // WAN usage today
    UsageTotals month_usage;  // WAN usage this month
    String data_source;      // "Total", "RX", "TX" to show what data is displayed
    float uptime_percent;    // WAN availability percentage
    float gateway_cpu;       // Gateway CPU usage percent (-1 until reported)
    float gateway_mem;       // Gateway memory usage percent (-1 until reported)
    String wan_status;
    String www_status;       // Internet reachability check ("" if not reported)
    int vpn_down;            // VPN tunnels not connected (-1 if unknown)
    int devices_offline;     // Gateways, switches and APs the controller can't reach
    bool health_ok;          // True when the health request succeeded
    String wan_ip;           // Public IP
    String gateway_name;     // e.g. "UDMSE"
    String isp_name;
    unsigned long timestamp;
};

#define MAX_DEVICE_PORTS 28  // Ports kept per device; 28 squares fill the Ports tab row

// One UniFi network device (gateway, switch, AP) for the Devices and Ports tabs
struct UniFiDevice {
    String name;
    String model;
    String type;    // "udm", "ugw", "usw", "uap", ...
    int state;      // 1 = connected, 0 = disconnected, other values are transitional
    int num_sta;    // Clients on this device
    int port_count; // Ports reported (may exceed MAX_DEVICE_PORTS)
    int port_up;    // Ports with a link, counted across all of them
    uint16_t port_speed[MAX_DEVICE_PORTS];  // Link speed in Mbps, 0 when down
    uint32_t uplink_ports;                  // Bit i set when port i is the uplink
};

#define TOP_CLIENTS 5     // Busiest clients kept for the Clients tab
#define NEWEST_CLIENTS 2  // Most recently connected clients kept

// One client row on the Clients tab
struct ClientInfo {
    String name;
    float down_bps;        // Download (the controller's tx_bytes-r), bytes per second
    float up_bps;          // Upload (the controller's rx_bytes-r), bytes per second
    unsigned long uptime;  // Seconds since the client connected
};

// Everything the Clients tab shows, built in one pass over the client list
struct ClientSummary {
    int total;
    ClientInfo top[TOP_CLIENTS];  // Busiest first, by download plus upload
    int top_count;
    ClientInfo newest[NEWEST_CLIENTS];
    int newest_count;
};

class UniFiAPI {
private:
    WiFiClientSecure client;
    String baseUrl;
    String apiKey;
    bool authenticated;
    unsigned long lastAuthTime;

    // Cached usage reports (refreshed every USAGE_REPORT_INTERVAL)
    UsageTotals dayUsage;
    UsageTotals monthUsage;
    unsigned long lastUsageFetch;

    // Cached VPN state (refreshed every VPN_STATUS_INTERVAL)
    int vpnDown;
    unsigned long lastVpnFetch;

    // Helper methods
    bool beginRequest(HTTPClient& https, const String& url);
    float bytesToMbps(unsigned long bytes, unsigned long timeDelta);
    void getHealthStats(NetworkStats& stats);
    void fetchUsageReport(const char* path, UsageTotals& usage);
    void fetchVpnStatus();

public:
    UniFiAPI();
    ~UniFiAPI();
    
    // Connection management
    bool connect(String url, String key);
    bool authenticate();
    bool isConnected();
    void disconnect();
    
    // API calls
    NetworkStats getNetworkStats();
    // Fills up to maxDevices, sorted gateway, switches, APs; returns the site's total or -1 on failure
    int getDevices(UniFiDevice* devices, int maxDevices);
    // Builds the Clients tab summary; returns false (leaving summary untouched) on failure
    bool getClients(ClientSummary& summary);
    
    // Utility
    bool testConnection();
    void setInsecure(bool insecure);
};

#endif // UNIFI_API_H