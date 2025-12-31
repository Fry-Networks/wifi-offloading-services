#!/usr/bin/env bash

set -e

APP="${1:-agent}"
RUN_DIR="run"
APP_DIR="$RUN_DIR/fry-$APP"

echo "Setting up development environment for app: $APP"

case "$APP" in
    "agent")
        # Copy scripts and data files for agent
        echo "Copying agent scripts and data files..."
        mkdir -p $APP_DIR/scripts
        cp apps/agent/scripts/dev/* $APP_DIR/scripts/
        chmod +x $APP_DIR/scripts/*

        mkdir -p $APP_DIR/data
        mkdir -p $APP_DIR/data/did-key

        cp VERSION $RUN_DIR/VERSION

        # Run the agent program
        echo "Starting fry-os-services agent in development mode..."
        cd $APP_DIR && ./fry-agent --dev
        ;;

    "config")
        # Set up config app dev environment
        echo "Setting up config app in development environment..."
        mkdir -p $APP_DIR/scripts/dev
        mkdir -p $APP_DIR/scripts/dev/hashes

        # Copy maintaining directory structure
        echo "Copying config scripts maintaining directory structure..."
        cp -r apps/config/scripts/dev/* $APP_DIR/scripts/ 2>/dev/null || echo "No dev files to copy"
        chmod +x $APP_DIR/scripts/dev/* 2>/dev/null || echo "No scripts to make executable"

        # Create hashes directory
        echo "Created hashes directory: $APP_DIR/scripts/hashes"

        cp VERSION $RUN_DIR/VERSION

        # Run the config program
        echo "Starting config app in development mode..."
        cd $APP_DIR && ./fry-config --dev
        ;;

    "collector")
        # Set up collector app dev environment
        echo "Setting up collector app development environment..."
        mkdir -p $APP_DIR/data
        mkdir -p $APP_DIR/logs
        mkdir -p $APP_DIR/scripts

        # Copy collector development scripts and config
        cp apps/collector/scripts/dev/* $APP_DIR/scripts/
        chmod +x $APP_DIR/scripts/*

        # Copy the collector development configuration file to the working directory
        echo "Copying collector development configuration file..."
        cp apps/collector/scripts/dev/fry-collector.config $APP_DIR/
        echo "Configuration file: fry-collector.config (optimized for local development)"

        cp VERSION $RUN_DIR/VERSION

        # Run the collector program
        cd $APP_DIR && ./fry-collector --dev
        ;;

    *)
        echo "Error: Unknown app '$APP'"
        echo "Supported apps: agent, config, collector"
        exit 1
        ;;
esac
