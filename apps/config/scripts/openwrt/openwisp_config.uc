#!/usr/bin/env ucode

import { cursor } from 'uci';

function log_info(msg) {
    printf('[openwisp-config] INFO: %s\n', msg);
}

function log_warn(msg) {
    printf('[openwisp-config] WARN: %s\n', msg);
}

function log_debug(msg) {
    printf('[openwisp-config] DEBUG: %s\n', msg);
}

// Check if OpenWisp config exists using UCI API
function openwisp_exists() {
    let ctx = cursor();
    try {
        let config = ctx.get_all('openwisp');
        return (config != null);
    } catch (e) {
        return false;
    }
}

// Get current unmanaged list using UCI API
function get_unmanaged_list() {
    let ctx = cursor();
    try {
        let unmanaged = ctx.get('openwisp', 'http', 'unmanaged');
        return (unmanaged || []);
    } catch (e) {
        log_debug('No existing unmanaged list found');
        return [];
    }
}

// Check if item is in unmanaged list
function is_unmanaged(item, unmanaged_list) {
    for (let entry in unmanaged_list) {
        if (entry == item) return true;
    }
    return false;
}

// Create array without duplicates
function merge_arrays(existing_array, new_items) {
    let merged = [...existing_array]; // Copy existing array
    
    for (let item in new_items) {
        if (!is_unmanaged(item, merged)) {
            push(merged, item);
            log_debug(sprintf('Added %s to unmanaged list', item)); 
        } else {
            log_debug(sprintf('Item %s already in array', item));
        }
    }
    
    return merged;
}

// Restart Openwisp service
function restart_openwisp_service() {
    log_info('Restarting OpenWisp config agent...');
    
    let command = '/etc/init.d/openwisp_config restart 2>/dev/null &';
    let result = system(command);
    
    if (result == 0) {
        log_info('Openwisp service restart initiated');
        return true;
    } else {
        log_warn('Failed to restart openwisp_config service');
        return false;
    }
}

// Main configuration function
function main() {
    log_info('Checking Openwisp configuration...');

    if (!openwisp_exists()) {
        log_info('Openwisp config not found, skipping configuration');
        return 0;
    }

    // Fry sections that should be unmanaged
    let fry_sections = [
        'opennds.opennds1',              
        'wireless.captive_wifi_5ghz',    
        'wireless.captive_wifi_2ghz'     
    ];

    // Get current list and merge with new items
    let current_unmanaged = get_unmanaged_list();
    log_debug(sprintf('Current unmanaged list has %d items', length(current_unmanaged)));

    // Create merged array without duplicates
    let new_unmanaged = merge_arrays(current_unmanaged, fry_sections);
    
    // Check if changes are needed
    let changes_made = (length(new_unmanaged) != length(current_unmanaged));
    
    if (changes_made) {
        log_info(sprintf('Updating Openwisp unmanaged list (%d -> %d items)', 
                        length(current_unmanaged), length(new_unmanaged)));
        
        try {
            let ctx = cursor();

            // Clear existing list first
            ctx.set('openwisp', 'http', 'unmanaged', []);
            log_debug('Cleared existing unmanaged list');
            
            // Set the entire array at once
            ctx.set('openwisp', 'http', 'unmanaged', new_unmanaged);
            log_debug(sprintf('Set new unmanaged list with %d items', length(new_unmanaged)));
            
            // Save and commit
            ctx.save('openwisp');
            ctx.commit('openwisp');

            log_info('Openwisp configuration updated successfully');

            // Show final configuration
            log_info('Final unmanaged sections:');
            for (let section in new_unmanaged) {
                log_info(sprintf('  - %s', section));
            }

            // Restart OpenWisp config agent
            restart_openwisp_service();

            return 1; // Changes made
            
        } catch (e) {
            log_warn(sprintf('Failed to update Openwisp configuration: %s', e));
            return -1; // Error
        }
    } else {
        log_info('No Openwisp configuration changes needed');
        log_debug('All fry sections already in unmanaged list');
        return 0; // No changes
    }
}

exit(main());