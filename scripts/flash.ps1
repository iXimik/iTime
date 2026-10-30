param(
    [string]$port = "com3"
)

$ErrorActionPreference = "Stop"

if (-not (get-command arduino-cli -erroraction silentlycontinue)) {
    $cliDir = "c:\program files\arduino cli"
    if (test-path (join-path $cliDir "arduino-cli.exe")) {
        $env:path += ";$cliDir"
    } else {
        throw "arduino-cli not found"
    }
}

$root = (resolve-path (join-path $PSScriptRoot "..")).Path
$sketch = join-path $root "firmware\itime"
$secrets = join-path $sketch "secrets.h"
$fqbn = "esp32:esp32:esp32:FlashFreq=40,FlashMode=dio"

if (-not (test-path $secrets)) {
    throw "missing secrets.h. copy firmware\itime\secrets.example.h to secrets.h and edit wifi credentials."
}

write-host "compiling with spi flash 40 mhz / dio..."
arduino-cli compile --clean --fqbn $fqbn $sketch
if ($LASTEXITCODE -ne 0) { throw "compile failed; nothing uploaded" }

write-host "uploading to $port..."
arduino-cli upload -p $port --fqbn $fqbn $sketch
if ($LASTEXITCODE -ne 0) { throw "upload failed" }

write-host "done. serial monitor: arduino-cli monitor -p $port --config baudrate=115200"
