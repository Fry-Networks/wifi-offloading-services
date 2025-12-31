#!/bin/bash

#KEY_PATH="/etc/mosquitto/certs/device.key"
#CSR_PATH="/etc/mosquitto/certs/device.csr"
#CERT_PATH="/etc/mosquitto/certs/device.crt"
#BACKEND_URL="https://wifi.api.internal.fry.tech/certificate-signing/sign"

KEY_PATH=$1
CSR_PATH=$2
CERT_PATH=$3
BACKEND_URL=$4

# Generate private key
echo "INFO: Generating private key..."
openssl genrsa -out $KEY_PATH 2048
if [ $? -ne 0 ]; then
  echo "ERROR: Error generating private key."
  exit 1
fi
echo "INFO: Private key generated in $KEY_PATH"

# Generate CSR
echo "INFO: Generating CSR..."
openssl req -new -key $KEY_PATH -out $CSR_PATH -subj "/C=US/ST=State/L=City/O=Organization/OU=OrgUnit/CN=common.name"
if [ $? -ne 0 ]; then
  echo "ERROR: Error generating CSR."
  exit 1
fi
echo "INFO: CSR generated in $CSR_PATH"

# Send CSR to backend to be signed
echo "INFO: Sending CSR to backend to be signed..."
response=$(curl -s -X POST $BACKEND_URL -F "csr=@$CSR_PATH" -o $CERT_PATH)
if [ $? -ne 0 ]; then
  echo "ERROR: Error sending CSR to backend."
  exit 1
fi

# Check backend response
if [ -s $CERT_PATH ]; then
  echo "INFO: Signed certificate received and saved in $CERT_PATH"
else
  echo "ERROR: Error: A valid signed certificate was not received."
  exit 1
fi
