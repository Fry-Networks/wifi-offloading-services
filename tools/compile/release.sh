#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"

# --- LOAD .env CONFIGURATION ---
ENV_FILE="$PROJECT_ROOT/.env"

if [ ! -f "$ENV_FILE" ]; then
  echo "Error: .env file not found at $ENV_FILE"
  exit 1
fi

# Load environment variables from the file
set -o allexport
source "$ENV_FILE"
set +o allexport

# Check required variables
if [[ -z "$API_URL" || -z "$BEARER_TOKEN" ]]; then
  echo "Error: API_URL or BEARER_TOKEN not defined in .env"
  exit 1
fi

# Set default release track if not specified
if [[ -z "$RELEASE_TRACK" ]]; then
  RELEASE_TRACK="stable"
fi

# --- CONFIGURATION ---
package_name="fry-os-services"
api_url="$API_URL"
bearer_token="$BEARER_TOKEN"
release_track="$RELEASE_TRACK"

# --- VALID ARCHITECTURES ---
valid_architectures=(
  "aarch64_cortex-a53_filogic"
  "aarch64_cortex-a53_mt7622"
  "arm_cortex-a7_neon-vfpv4_mikrotik"
  "mips_24kc_generic"
  "mipsel_24kc_mt7621"
)

# --- INPUT VALIDATION ---
if [ $# -ne 1 ]; then
  echo "Usage: $0 <package_architecture_name>"
  echo "Valid options are:"
  for arch in "${valid_architectures[@]}"; do
    echo "  - $arch"
  done
  exit 1
fi

package_arch="$1"
full_path="$PROJECT_ROOT/output/$package_arch"

if [ ! -d "$full_path" ]; then
  echo "Error: Directory '$full_path' does not exist."
  exit 1
fi

# --- CHECK IF INPUT IS ALLOWED ---
is_valid=false
for arch in "${valid_architectures[@]}"; do
  if [[ "$package_arch" == "$arch" ]]; then
    is_valid=true
    break
  fi
done

if [ "$is_valid" != "true" ]; then
  echo "Error: '$package_arch' is not a valid architecture."
  echo "Allowed values:"
  for arch in "${valid_architectures[@]}"; do
    echo "  - $arch"
  done
  exit 1
fi

# --- READ VERSION FILE ---
VERSION_FILE="$PROJECT_ROOT/VERSION"
if [ ! -f "$VERSION_FILE" ]; then
  echo "Error: VERSION file not found at $VERSION_FILE"
  exit 1
fi

version=$(cat "$VERSION_FILE" | tr -d '[:space:]')

if [[ -z "$version" ]]; then
  echo "Error: VERSION file is empty."
  exit 1
fi

echo "Package name: $package_name"
echo "Architecture: $package_arch"
echo "Version: $version"
echo "Release track: $release_track"

# --- EXTRACT ARCH AND SUBTARGET FROM package_arch ---
subtarget=$(echo "$package_arch" | awk -F'_' '{print $NF}')
arch=$(echo "$package_arch" | sed "s/_${subtarget}$//")

# --- FIND THE .ipk FILE IN THE FOLDER ---
file_path=$(find "$full_path" -maxdepth 1 -type f -name "${package_name}_${version}-*_*.ipk" | grep "_${arch}.ipk" | sort | tail -n 1)

if [[ ! -f "$file_path" ]]; then
  echo "Error: No .ipk file found for version $version in $full_path"
  exit 1
fi

echo "Found .ipk file: $file_path"

# --- CALCULATE CHECKSUM AND SIZE ---
checksum=$(sha256sum "$file_path" | cut -d' ' -f1)
size_bytes=$(stat -c%s "$file_path")

echo "SHA256 checksum: $checksum"
echo "File size: $size_bytes bytes"

# --- CREATE JSON METADATA USING jq ---
metadata=$(jq -n \
  --arg name "$package_name" \
  --arg arch_name "$package_arch" \
  --arg version "$version" \
  --arg path "$file_path" \
  --arg checksum "$checksum" \
  --arg release_track "$release_track" \
  --argjson size "$size_bytes" \
  '{
    package_name: $name,
    package_architecture_name: $arch_name,
    version: $version,
    storage_path: $path,
    checksum: $checksum,
    size_bytes: $size,
    release_track: $release_track
  }')

echo "Prepared JSON metadata:"
echo "$metadata" | jq

# --- SEND PACKAGE TO BACKEND ---
echo "Sending package release request to $api_url..."

response=$(curl -s -X POST \
  -H "Authorization: Bearer $bearer_token" \
  -H "Content-Type: multipart/form-data" \
  -F "metadata=$metadata" \
  -F "package=@$file_path" \
  "$api_url")

# --- DISPLAY SERVER RESPONSE ---
echo "Server response:"
echo "$response" | jq 2>/dev/null || echo "$response"
