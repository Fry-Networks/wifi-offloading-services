#!/usr/bin/env ucode

import { cursor } from 'uci';
import * as fs from 'fs';

function boolToUci(value) {
    return type(value) == 'bool' ? (value ? '1' : '0') : "" + value;
}

// Guard for the WPA2 passphrase. This renderer has NO environment resolver, so
// a wifi-iface "key" must already be a real passphrase. An unresolved
// "${NAME}" placeholder is a configuration error (it would otherwise be
// applied as a publicly known passphrase), and an empty, whitespace-only or
// too-short value would produce an OPEN network. Both are refused loudly
// before anything is written to UCI.
// Returns null when usable, else an error text.
function pskProblem(psk) {
    if (type(psk) != 'string') return 'must be a string';
    let n = length(psk);
    if (n >= 4 && substr(psk, 0, 2) == '${' && substr(psk, n - 1, 1) == '}')
        return 'is an unresolved environment placeholder; the production renderer does not resolve placeholders';
    if (n == 0) return 'is empty and would create an OPEN network';
    let blank = true;
    for (let i = 0; i < n; i++) {
        let c = ord(psk, i);
        if (c != 0x20 && c != 0x09 && c != 0x0a && c != 0x0d) { blank = false; break; }
    }
    if (blank) return 'is whitespace-only and would create an OPEN network';
    if (n < 8) return 'is shorter than the WPA2 minimum of 8 characters and would create an OPEN network';
    return null;
}

// Validate every PSK field in the config, so a failure aborts before any
// section is written to the UCI cursor.
function validatePsks(config) {
    let wireless = config.device_config?.wireless;
    if (type(wireless) != 'array') return null;
    for (let section in wireless) {
        if (type(section) != 'object' || section.meta_type != 'wifi-iface') continue;
        for (let k in section) {
            if (k != 'key') continue;
            let problem = pskProblem(section[k]);
            if (problem != null)
                return sprintf('wireless.%s.key %s', section.meta_section || '?', problem);
        }
    }
    return null;
}

// Clear all options in a section
function clearAllSectionOptions(ctx, packageName, sectionName) {
    printf("Clearing all options in %s.%s\n", packageName, sectionName);
    
    try {
        // Get all the options of the section
        let sectionData = ctx.get_all(packageName, sectionName);
        if (!sectionData) {
            printf("Section %s.%s does not exist (will be created)\n", packageName, sectionName);
            return true;
        }
        
        let cleanedCount = 0;
        for (let option in sectionData) {
            // Do not delete UCI meta-information (section type, etc.)
            if (option == '.type' || option == '.name' || option == '.anonymous') {
                continue;
            }
            
            try {
                printf("Deleting option %s.%s.%s\n", packageName, sectionName, option);
                ctx.delete(packageName, sectionName, option);
                cleanedCount++;
            } catch (e) {
                printf("Warning: Could not delete option %s.%s.%s: %s\n", packageName, sectionName, option, "" + e);
            }
        }
        
        printf("Cleared %d options in %s.%s\n", cleanedCount, packageName, sectionName);
        return true;
    } catch (e) {
        printf("Warning: Could not clear options in %s.%s: %s\n", packageName, sectionName, "" + e);
        return true; // Continue anyway
    }
}

// Clean all options in the fry section
function cleanFrySection(ctx, packageName, sectionName) {
    printf("Cleaning existing %s section: %s\n", packageName, sectionName);
    return clearAllSectionOptions(ctx, packageName, sectionName);
}

// Clean all options in the OpenNDS section
function cleanOpenNdsSection(ctx, sectionName) {
    printf("Cleaning existing OpenNDS section: %s\n", sectionName);
    return clearAllSectionOptions(ctx, 'opennds', sectionName);
}

// Ensure section exists before configuring
function ensureSectionExists(ctx, packageName, sectionType, sectionName) {
    try {
        let sectionData = ctx.get_all(packageName, sectionName);
        if (sectionData) {
            printf("Section %s.%s already exists\n", packageName, sectionName);
            return true;
        }
    } catch (e) {
        // Section doesn't exist, will create it
    }
    
    // Create the named section using ctx.set() with the section type
    try {
        printf("Creating new section %s.%s (type: %s)\n", packageName, sectionName, sectionType);
        // In UCI, to create a named section you must set its type using ctx.set()
        ctx.set(packageName, sectionName, sectionType);
        printf("Successfully created section %s.%s\n", packageName, sectionName);
        return true;
    } catch (e) {
        printf("Error creating section %s.%s: %s\n", packageName, sectionName, "" + e);
        return false;
    }
}

function applySection(ctx, packageName, sectionType, section) {
    let name = section.meta_section;
    if (!name) {
        printf("Error: Section missing 'meta_section'\n");
        return false;
    }

    printf("Configuring %s section: %s in package %s\n", sectionType, name, packageName);

    // ENSURE SECTION EXISTS FIRST
    if (!ensureSectionExists(ctx, packageName, sectionType, name)) {
        printf("Error: Failed to ensure section %s.%s exists\n", packageName, name);
        return false;
    }

    for (let k in section) {
        if (k == 'meta_section' || k == 'meta_type' || k == 'meta_config') continue;

        let v = section[k];
        
        // HANDLE ARRAY VALUES AS UCI LISTS
        if (type(v) == 'array') {
            printf("Setting list %s.%s.%s with %d items\n", packageName, name, k, length(v));
            
            try {
                ctx.set(packageName, name, k, v);
                printf("Successfully set list %s.%s.%s with %d items\n", packageName, name, k, length(v));
            } catch (e) {
                printf("Error setting list %s.%s.%s: %s\n", packageName, name, k, "" + e);
                return false;
            }
        } else {
            // Handle single values
            let uciValue = boolToUci(v);
            try {
                ctx.set(packageName, name, k, uciValue);
                printf("Set %s.%s.%s = %s\n", packageName, name, k, uciValue);
            } catch (e) {
                printf("Error setting %s.%s.%s=%s: %s\n", packageName, name, k, uciValue, "" + e);
                return false;
            }
        }
    }

    printf("Successfully configured section: %s in %s\n", name, packageName);
    return true;
}

function applyWirelessConfig(ctx, config) {
    let wirelessArray = config.device_config?.wireless;
    if (!wirelessArray || type(wirelessArray) != 'array') {
        printf("No wireless configuration found\n");
        return true;
    }

    printf("Applying wireless configuration...\n");  
      
    for (let section in wirelessArray) {
        let pkg = "wireless";
        let t = section.meta_type;
        if (t == 'wifi-device' || t == 'wifi-iface') {
            if (!applySection(ctx, pkg, t, section)) {
                return false;
            }
        }
    }

    printf("Wireless configuration applied successfully\n");
    return true;
}

// Clean all options before applying
function applyFryConfig(ctx, config) {
    let fryArray = config.device_config?.fry;
    if (!fryArray || type(fryArray) != 'array') {
        printf("No Fry configuration found\n");
        return true; 
    }

    printf("Applying Fry services configuration...\n");
    for (let section in fryArray) {
        let pkg = section.meta_config;
        let t = section.meta_type;
        let sectionName = section.meta_section;

        // Clean all options in the section
        if (sectionName && !cleanFrySection(ctx, pkg, sectionName)) {
            return false;
        }
        
        if (!applySection(ctx, pkg, t, section)) {
            return false;
        }
    }

    return true;
}

// Clean section by section 
function applyOpenNdsConfig(ctx, config) {
    let openndsArray = config.device_config?.opennds;
    if (!openndsArray || type(openndsArray) != 'array') {
        printf("No OpenNDS configuration found\n");
        return true; 
    }

    printf("Applying OpenNDS configuration...\n");

    for (let section in openndsArray) {
        let pkg = "opennds";
        let t = section.meta_type || "opennds";
        let sectionName = section.meta_section;

        // Clean all options in this specific section
        if (sectionName && !cleanOpenNdsSection(ctx, sectionName)) {
            return false;
        }
        
        if (!applySection(ctx, pkg, t, section)) {
            return false;
        }
    }

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

    // Refuse an unusable passphrase before touching UCI: never apply a
    // placeholder literal or an empty key.
    let pskIssue = validatePsks(config);
    if (pskIssue != null) {
        warn(sprintf("Error: refusing to apply configuration: %s\n", pskIssue));
        return 1;
    }

    let ctx;
    try {
        ctx = cursor();
    } catch (e) {
        printf("Error: Failed to initialize UCI cursor: %s\n", "" + e);
        return 1;
    }

    printf("Starting configuration application with selective cleanup...\n");

    if (!applyWirelessConfig(ctx, config)) {
        printf("Error: Failed to apply wireless configuration\n");
        return 1;
    }

    if (!applyFryConfig(ctx, config)) {
        printf("Error: Failed to apply Fry configuration\n");
        return 1;
    }

    if (!applyOpenNdsConfig(ctx, config)) {  
        printf("Error: Failed to apply OpenNDS configuration\n");
        return 1;
    }

    // COMMIT PACKAGES WITH ERROR HANDLING
    printf("Committing configuration changes...\n");
    let packages = ['wireless', 'fry-agent', 'fry-collector', 'fry-config', 'opennds'];
    
    for (let pkg in packages) {
        try {
            printf("Saving and committing package: %s\n", pkg);
            ctx.save(pkg);
            ctx.commit(pkg);
            printf("Successfully committed package: %s\n", pkg);
        } catch (e) {
            // WARNING INSTEAD OF ERROR - SOME PACKAGES MAY NOT EXIST
            printf("Warning: Error committing %s: %s\n", pkg, "" + e);
            // Continue with other packages instead of failing completely
        }
    }

    printf("Configuration application completed successfully\n");
    printf("Services will be restarted by fry-config main process\n");
    return 0;
}

exit(main());