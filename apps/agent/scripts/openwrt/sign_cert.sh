#!/bin/sh

#KEY_PATH="/etc/mosquitto/certs/device.key"
#CSR_PATH="/etc/mosquitto/certs/device.csr"
#CERT_PATH="/etc/mosquitto/certs/device.crt"
#BACKEND_URL="https://wifi.api.internal.fry.tech/certificate-signing/sign"

KEY_PATH=$1
CSR_PATH=$2
CERT_PATH=$3
BACKEND_URL=$4

logger -t "fry-os-services" "INFO: KEY_PATH: $KEY_PATH"
logger -t "fry-os-services" "INFO: CSR_PATH: $CSR_PATH"
logger -t "fry-os-services" "INFO: CERT_PATH: $CERT_PATH"
logger -t "fry-os-services" "INFO: BACKEND_URL: $BACKEND_URL"

# Generate private key
logger -t "fry-os-services" "INFO: Generating private key..."
openssl genrsa -out $KEY_PATH 2048
if [ $? -ne 0 ]; then
  logger -t "fry-os-services" "ERROR: Error generating private key."
  exit 1
fi
logger -t "fry-os-services" "INFO: Private key generated in $KEY_PATH"

# Generate CSR
logger -t "fry-os-services" "INFO: Generating CSR..."
openssl req -new -key $KEY_PATH -out $CSR_PATH -subj "/C=US/ST=State/L=City/O=Organization/OU=OrgUnit/CN=common.name"
if [ $? -ne 0 ]; then
  logger -t "fry-os-services" "ERROR: Error generating CSR."
  exit 1
fi
logger -t "fry-os-services" "INFO: CSR generated in $CSR_PATH"

# Send CSR to backend to be signed
logger -t "fry-os-services" "INFO: Sending CSR to backend to be signed..."
response=$(curl -s -X POST $BACKEND_URL -F "csr=@$CSR_PATH" -o $CERT_PATH)
if [ $? -ne 0 ]; then
  logger -t "fry-os-services" "ERROR: Error sending CSR to backend."
  exit 1
fi

# Check backend response
if [ -s $CERT_PATH ]; then
  logger -t "fry-os-services" "INFO: Signed certificate received and saved in $CERT_PATH"
else
  logger -t "fry-os-services" "ERROR: Error: A valid signed certificate was not received."
  exit 1
fi
