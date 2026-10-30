# Run in PowerShell, from any directory.
# This sets up a known Arduino CLI / ESP32 toolchain.
$ErrorActionPreference = "Stop"

if (-not (get-command arduino-cli -erroraction silentlycontinue)) {
    $cliDir = "c:\program files\arduino cli"
    if (test-path (join-path $cliDir "arduino-cli.exe")) {
        $env:path += ";$cliDir"
    } else {
        throw "arduino-cli missing. run: winget install --id ArduinoSA.CLI -e"
    }
}

arduino-cli version
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
if ($LASTEXITCODE -ne 0) { throw "esp32 index url setup failed" }
arduino-cli core update-index
if ($LASTEXITCODE -ne 0) { throw "core update-index failed" }
arduino-cli core install esp32:esp32@3.2.1
if ($LASTEXITCODE -ne 0) { throw "esp32 core installation failed" }
arduino-cli lib install "LiquidCrystal"
if ($LASTEXITCODE -ne 0) { throw "LiquidCrystal installation failed" }
arduino-cli core list
write-host "toolchain ready. flash frequency is set by build/flash commands, not here."
