# Pending Configurations in fry-config

## WiFi HTMode Configuration

### Current Status
fry-config currently configures basic WiFi interface parameters (radio0 and radio1), but **does not configure channel width (HTMode)** in OpenWrt.

### Identified Problem
Different Fry devie models have distinct radio configurations:

#### **Frequency Bands per Radio:**
- **radio0**: Can be 2.4GHz or 5GHz depending on model
- **radio1**: Can be 5GHz or 2.4GHz depending on model

#### **Models with Different Configurations:**
```bash
# Example:
# radio0 = 2.4GHz
# radio1 = 5GHz

# Example:  
# radio0 = 5GHz
# radio1 = 2.4GHz
```

### Missing HTMode Configuration

#### **What is HTMode?**
HTMode defines the WiFi channel width in OpenWrt:

```bash
# Current UCI configuration:
uci set wireless.radio0.htmode='HT20'    # 20MHz
uci set wireless.radio0.htmode='HT40'    # 40MHz  
uci set wireless.radio0.htmode='VHT80'   # 80MHz (5GHz)
uci set wireless.radio0.htmode='VHT160'  # 160MHz (5GHz)
```

### Required Implementation

#### **1. Detect Frequency Band per Radio**
```c
// In renderer.c - function to implement:
static char* detect_radio_band(const char* radio_name) {
    // Read UCI configuration to determine band
    // uci get wireless.radio0.band
    // Return "2g" or "5g"
}
```

#### **2. Configure Appropriate HTMode**
```javascript
// In renderer_applier_no_restart.uc - logic to add:
function setOptimalHTMode(radio_name, config_section) {
    let band = detectRadioBand(radio_name);
    let htmode;
    
    if (band == "2g") {
        // 2.4GHz - use HT modes
        htmode = config_section.htmode_2g || "HT40";
    } else if (band == "5g") {
        // 5GHz - use VHT modes  
        htmode = config_section.htmode_5g || "VHT80";
    }
    
    ctx.set("wireless", radio_name, "htmode", htmode);
}
```

#### **3. Server JSON Configuration**
```json
{
  "device_config": {
    "wireless": [
{
        "meta_config": "wireless",
        "meta_type": "wifi-device",
        "meta_section": "radio0",
        "channel": "auto",
        "htmode": "VHT80",
        "disabled": "0",
        "cell_density": "0"
      },
      {
        "meta_config": "wireless",
        "meta_type": "wifi-device",
        "meta_section": "radio1",
        "channel": "auto",
        "htmode": "HT40",
        "disabled": "0",
        "cell_density": "0"
      }
    ]
  }
}
```

### Compatibility Note
This implementation must be compatible with the current hash system and selective service restart implemented in fry-config.

