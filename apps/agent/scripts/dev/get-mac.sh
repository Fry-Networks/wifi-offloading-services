#!/bin/sh

MODEL="${FRY_MODEL:-unknown}"

# Return different MACs based on device name for testing

case "$MODEL" in
    "Kratos"|"Hemera"|"Artemisa"|"Atenea")
        echo "5C:63:D2:5C:63:D2"
        ;;
    "Loki"|"Gaia"|"Helios")
        echo "5C:63:D2:5C:63:D3"
        ;;
    *)
        echo "5C:63:D2:5C:63:D4"
        ;;
esac
