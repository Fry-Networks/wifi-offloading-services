# Default recipe - shows available commands
default:
    @just --list

# Build with CMake
build:
    mkdir -p build
    cd build && cmake .. && make

# Development environment setup for specific app
# Available apps: agent, config, collector
run app="agent":
    just build
    @if [ ! -f "build/fry-{{app}}" ]; then echo "Error: App 'fry-{{app}}' not found in build directory."; exit 1; fi
    mkdir -p run/fry-{{app}}
    cp build/fry-{{app}} run/fry-{{app}}/fry-{{app}}
    bash tools/run.sh {{app}}

# Generate compilation database (compile_commands.json)
compdb:
    bash tools/compdb.sh

# Format code
format:
    cd tools/format && ./format.sh

# Compile for specific architecture (optional: add 'debug' as second parameter)
compile arch debug="":
    #!/usr/bin/env bash
    cd tools/compile
    if [ "{{debug}}" = "debug" ]; then
        echo "Running in debug mode with -j1 V=s"
        go run compile.go {{arch}} --debug
    else
        go run compile.go {{arch}}
    fi

# Upload IPK release package
release arch:
    bash tools/compile/release.sh {{arch}}

# Delete the ./build and ./run folder
clean:
    rm -rf ./build
    rm -rf ./run

# Delete the ./build, ./run, ./output folders
clean-full:
    rm -rf ./build
    rm -rf ./run
    rm -rf ./output
