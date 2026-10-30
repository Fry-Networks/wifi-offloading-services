#!/usr/bin/env ucode

import * as fs from 'fs';

// ---------------------------------------------------------------------------
// WPA2 passphrase handling
//
// Config JSON never carries a PSK. The one secret field - the "key" option of a
// wifi-iface section in the wireless package - holds a whole-string "${NAME}"
// placeholder that is resolved from the process environment at render time.
//
// Resolution is restricted to that field, identified structurally. Every other
// value in every section is rendered verbatim, so no arbitrary config value can
// be turned into an environment read.
//
// Both failure modes fail CLOSED: if the environment variable is missing, or
// resolves to something that is not a usable WPA2 passphrase (empty, whitespace
// only, too short), the renderer refuses to emit any UCI command, names the
// offending field and variable on stderr and exits non-zero. It never emits an
// empty key (an OPEN network) and never emits an unresolved placeholder (a
// publicly known passphrase).
// ---------------------------------------------------------------------------

const PSK_MIN_LEN = 8;   // WPA2-PSK passphrase minimum (802.11i / hostapd)
const PSK_MAX_LEN = 63;  // WPA2-PSK passphrase maximum

function boolToUci(value) {
    return type(value) == 'bool' ? (value ? '1' : '0') : "" + value;
}

// Return the environment variable name when `value` is a whole-string
// "${NAME}" placeholder, otherwise null.
function placeholderName(value) {
    if (type(value) != 'string') return null;
    let n = length(value);
    if (n < 4 || substr(value, 0, 2) != '${' || substr(value, n - 1, 1) != '}') return null;
    let name = substr(value, 2, n - 3);
    if (!match(name, /^[A-Za-z_][A-Za-z0-9_]*$/)) return null;
    return name;
}

// Structural identification of the one secret field: the "key" option of a
// wifi-iface section. Nothing else is ever resolved from the environment.
function isPskField(packageName, sectionType, key) {
    return packageName == 'wireless' && sectionType == 'wifi-iface' && key == 'key';
}

// Validate a final passphrase. Returns null when usable, else an error text.
// A single quote is rejected because the UCI command is emitted single-quoted;
// control characters are rejected because they cannot survive that emission.
function pskProblem(psk) {
    if (type(psk) != 'string') return 'must be a string';
    let n = length(psk);
    if (n == 0) return 'is empty';
    let blank = true;
    for (let i = 0; i < n; i++) {
        let c = ord(psk, i);
        if (c != 0x20 && c != 0x09 && c != 0x0a && c != 0x0d) { blank = false; break; }
    }
    if (blank) return 'is whitespace-only';
    if (n < PSK_MIN_LEN)
        return sprintf('is shorter than the WPA2 minimum of %d characters', PSK_MIN_LEN);
    if (n > PSK_MAX_LEN)
        return sprintf('is longer than the WPA2 maximum of %d characters', PSK_MAX_LEN);
    for (let i = 0; i < n; i++) {
        let c = ord(psk, i);
        if (c == 0x27 || c < 0x20 || c == 0x7f)
            return 'contains a quote or control character that cannot be emitted safely';
    }
    return null;
}

// Resolve one PSK field. Never returns an unusable value: on failure it pushes
// a message onto `errors` and returns null.
function resolvePsk(where, value, errors) {
    let name = placeholderName(value);
    if (name == null) {
        let problem = pskProblem(value);
        if (problem != null) {
            push(errors, sprintf('%s: configured value %s', where, problem));
            return null;
        }
        return value;
    }
    let resolved = getenv(name);
    if (resolved == null) {
        push(errors, sprintf('%s: required environment variable %s is not set (placeholder "%s" cannot be resolved)', where, name, value));
        return null;
    }
    let problem = pskProblem(resolved);
    if (problem != null) {
        push(errors, sprintf('%s: environment variable %s %s', where, name, problem));
        return null;
    }
    return resolved;
}

// Resolve every PSK field in place, before any output is produced, so that a
// failure cannot leave part of a UCI stream behind.
function preparePsks(config, errors) {
    let wireless = config.device_config?.wireless;
    if (type(wireless) != 'array') return;
    for (let section in wireless) {
        if (type(section) != 'object' || section.meta_type != 'wifi-iface') continue;
        for (let key in section) {
            if (!isPskField('wireless', section.meta_type, key)) continue;
            let where = sprintf('wireless.%s.key', section.meta_section || '?');
            let resolved = resolvePsk(where, section[key], errors);
            if (resolved != null) section[key] = resolved;
        }
    }
}

function printUciCommand(packageName, sectionName, key, value) {
    printf("uci set %s.%s.%s='%s'\n", packageName, sectionName, key, value);
}

function printUciListCommands(packageName, sectionName, key, values) {
    // Clear existing list first
    printf("uci delete %s.%s.%s 2>/dev/null || true\n", packageName, sectionName, key);

    // Add each value to the list
    for (let value in values) {
        printf("uci add_list %s.%s.%s='%s'\n", packageName, sectionName, key, value);
    }
}

function renderSection(packageName, sectionType, section) {
    let name = section.meta_section;
    if (!name) {
        printf("Error: Section missing 'meta_section'\n");
        return false;
    }

    printf("\n# Configuring %s section: %s in package %s\n", sectionType, name, packageName);

    for (let k in section) {
        if (k == 'meta_section' || k == 'meta_type' || k == 'meta_config') continue;

        let v = section[k];

        //HANDLE ARRAY VALUES AS UCI LISTS
        if (type(v) == 'array') {
            printf("# Setting list %s.%s.%s with %d items\n", packageName, name, k, length(v));
            printUciListCommands(packageName, name, k, v);
        } else {
            // Handle single values. PSK fields were already resolved (and
            // validated) by preparePsks(); nothing else is environment-read.
            let uciValue = boolToUci(v);
            printUciCommand(packageName, name, k, uciValue);
        }
    }

    return true;
}

function renderWirelessConfig(config) {
    let wirelessArray = config.device_config?.wireless;
    if (!wirelessArray || type(wirelessArray) != 'array') {
        printf("# No wireless configuration found\n");
        return false;
    }

    printf("\n# === Wireless Configuration ===\n");
    for (let section in wirelessArray) {
        let pkg = "wireless";
        let t = section.meta_type;
        if (t == 'wifi-device' || t == 'wifi-iface') {
            renderSection(pkg, t, section);
        }
    }
    printf("uci commit wireless\n");
    printf("# WiFi reload will be handled by fry-config main process\n");
    return true;
}

function renderFryConfig(config) {
    let fryArray = config.device_config?.fry;
    if (!fryArray || type(fryArray) != 'array') {
        printf("# No Fry configuration found\n");
        return false;
    }

    printf("\n# === Fry Services Configuration ===\n");
    for (let section in fryArray) {
        let pkg = section.meta_config;
        let t = section.meta_type;
        renderSection(pkg, t, section);
        printf("uci commit %s\n", pkg);
        printf("# %s restart will be handled by fry-config main process\n", pkg);
    }
    return true;
}

function renderOpenNdsConfig(config) {
    let openndsArray = config.device_config?.opennds;
    if (!openndsArray || type(openndsArray) != 'array') {
        printf("# No OpenNDS configuration found\n");
        return false;
    }

    printf("\n# === OpenNDS Configuration ===\n");
    for (let section in openndsArray) {
        let pkg = "opennds";
        let t = section.meta_type || "opennds";
        renderSection(pkg, t, section);
    }
    printf("uci commit opennds\n");
    printf("# OpenNDS restart will be handled by fry-config main process\n");
    return true;
}

function readConfig(filename) {
    try {
        let file = fs.open(filename, 'r');
        let content = file.read('all');
        file.close();
        return json(content);
    } catch (e) {
        printf("Error reading or parsing config file '%s': %s\n", filename, "" + e);
        return null;
    }
}

function main() {
    let configFile = ARGV[0];
    if (!configFile) {
        printf("Error: No config file specified\n");
        printf("Usage: ucode script.uc <config_file>\n");
        return 1;
    }

    let config = readConfig(configFile);
    if (!config) return 1;

    // Fail closed before emitting anything: a missing or unusable passphrase
    // must never produce an open network or a publicly known key.
    let errors = [];
    preparePsks(config, errors);
    if (length(errors) > 0) {
        for (let e in errors) warn(sprintf("Error: refusing to render: %s\n", e));
        return 1;
    }

    renderWirelessConfig(config);
    renderFryConfig(config);
    renderOpenNdsConfig(config);

    printf("\n# === End of UCI Commands ===\n");
    return 0;
}

exit(main());
