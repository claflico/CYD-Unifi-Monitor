#!/usr/bin/env python3
"""Test UniFi API key authentication"""

import os
import re
import requests
from requests.packages import urllib3
urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)


def read_credentials():
    """Read credentials from include/credentials.h."""
    with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'include', 'credentials.h'), 'r') as f:
        content = f.read()
    return {
        'host': re.search(r'#define UNIFI_HOST "([^"]*)"', content).group(1),
        'api_key': re.search(r'#define UNIFI_API_KEY "([^"]*)"', content).group(1),
    }


_creds = read_credentials()
UNIFI_HOST = _creds['host']
UNIFI_API_KEY = _creds['api_key']
BASE_URL = f"https://{UNIFI_HOST}"

session = requests.Session()
session.verify = False
session.headers["X-API-KEY"] = UNIFI_API_KEY

print("=== Testing UniFi API Key ===")
print(f"Host: {UNIFI_HOST}")

# Endpoints the firmware uses
endpoints = [
    ("/proxy/network/api/s/default/stat/sysinfo", "Sysinfo"),
    ("/proxy/network/api/s/default/stat/device", "Devices"),
    ("/proxy/network/api/s/default/stat/health", "Health"),
    ("/proxy/network/api/s/default/stat/sta", "Clients"),
]

for endpoint, name in endpoints:
    response = session.get(f"{BASE_URL}{endpoint}")
    if response.status_code == 200:
        print(f"{name}: OK ({len(response.json().get('data', []))} items)")
    else:
        print(f"{name}: FAILED ({response.status_code}) {response.text[:200]}")
