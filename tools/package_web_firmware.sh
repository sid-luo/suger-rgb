#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
output_dir="${project_dir}/docs/installer/firmware"
output_bin="${output_dir}/suger-rgb-esp32c3.bin"
project_version="$(tr -d '[:space:]' < "${project_dir}/VERSION")"
manifest_version="$(sed -n 's/.*"version": "\([^"]*\)".*/\1/p' "${project_dir}/docs/installer/manifest.json" | head -n 1)"

if [[ -z "${IDF_PATH:-}" ]]; then
  echo "ESP-IDF environment is not active. Run: . ~/esp/esp-idf/export.sh" >&2
  exit 1
fi

if [[ "${project_version}" != "${manifest_version}" ]]; then
  echo "VERSION (${project_version}) does not match installer manifest (${manifest_version})." >&2
  exit 1
fi

mkdir -p "${output_dir}"
cd "${project_dir}"
idf.py build

python "${IDF_PATH}/components/esptool_py/esptool/esptool.py" \
  --chip esp32c3 merge_bin \
  --output "${output_bin}" \
  --flash_mode dio \
  --flash_freq 80m \
  --flash_size 4MB \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/suger_rgb.bin

python "${IDF_PATH}/components/esptool_py/esptool/esptool.py" \
  --chip esp32c3 image_info "${output_bin}"

(
  cd "${output_dir}"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum suger-rgb-esp32c3.bin > SHA256SUMS.txt
  else
    shasum -a 256 suger-rgb-esp32c3.bin > SHA256SUMS.txt
  fi
)

echo "Web firmware ready: ${output_bin}"
