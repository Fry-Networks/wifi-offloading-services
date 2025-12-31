# Debugging in OpenWrt

## Build

### Building with debug symbols
Use the commented out `Build/Compile` step in the main Makefile that includes the `-g -O0` flags to build with debug information and remove compiler optimizations.

### Configuring the build system
Configure the build system to include debug information in the `fry-os` repo with `make menuconfig`:

Open the **Global build settings** menu, and make sure these are checked:
- Collect kernel debug information
- Compile packages with debugging info

Make sure that **Binary stripping method** is set to `none`

Make sure these are not checked:
- Make debug information reproducible
- Strip unnecessary exports from the kernel image
- Strip unnecessary functions from libraries

Also, within the **Kernel built options** submenu, make sure that these are checked:
- Compile the kernel with debug filesystem enabled
- Compile the kernel with symbol table information
- Compile the kernel with debug information

## Install and run

### Installing the debugger
On your OpenWrt device, install `gdb`:
```bash
opkg update
opkg install gdb
```

### Running the debugger
On OpenWrt:
- Run `gdb` with the binary from its install location `/usr/bin`
- Set the arguments needed by the binary for proper operation
- Set breakpoints at certain line numbers or function names
- Run the binary

```bash
gdb /usr/bin/fry-os-services

# Within the gdb process
(gdb) set args --config-enabled "1" --config-main-api "https://api.internal.fry.tech" --config-accounting-enabled "1" --config-accounting-api "https://wifi.api.internal.fry.tech" --config-access-interval "120" --config-device-status-interval "120" --config-console-log-level "4"

(gdb) break main.c:20

(gdb) run
```
