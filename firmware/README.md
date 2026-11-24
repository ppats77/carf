# Core2 driveway test

Use the M5Stack Core2 v1.3, a FAT32 microSD card, a USB-C **data** cable, and the OBDLink MX+. The Core2 stores live readings on the card and sends sealed batches whenever it can reach the home receiver. The default profile requests supported engine speed, vehicle speed and coolant temperature. Tap the left touch zone while parked to select a vehicle profile and enable its supplemental reads. See [vehicle profiles and diagnostic collection](../VEHICLES.md). Each vehicle can support a different subset. [Core2 specifications and pinout](https://docs.m5stack.com/en/core/Core2_v1.3) · [MX+ pairing guide](https://www.obdlink.com/mxp/qsg/)

## Configure and flash

From the repository root, configure the running receiver and its credentials using the main README. On the computer with USB access, run the helper below and use the same upload token as the receiver. Choose a 2.4 GHz home SSID and a NAS address reachable from that SSID. Firmware uses HTTP with a bearer token on the trusted home LAN. Keep the receiver port private to that LAN.

```sh
python3 configure_logger.py
```

The helper creates `firmware/include/settings.h`. To configure by hand, copy `firmware/include/settings.example.h` to that path and edit the copy. Firmware and configuration containing your credentials should stay private. The example builds an offline logger for compilation checks. The upload target requires a configured Wi-Fi SSID, receiver URL and token.

Insert the card with the Core2 turned off. Connect the USB-C data cable and run these commands from the repository root with PlatformIO Core on your PATH. On the development Mac the executable is also at `~/.platformio/penv/bin/platformio`.

```sh
platformio device list
platformio run -d firmware -t upload
```

If several serial devices appear, add `--upload-port /dev/cu.usbserial-…` with the actual Core2 port. For diagnostics, run `platformio device monitor -b 115200 -p PORT` with that same port. On first use, let the Core2 join home Wi-Fi until the screen shows `UTC: set`. It then maintains time using the battery-backed clock. If it starts up before the clock has been set, readings still carry elapsed milliseconds and omit the unknown UTC time. A local NTP server can be set through `CARF_NTP`.

## Parked acceptance test

1. Start the home receiver. Power the Core2 by USB. Confirm that the display shows `SD ready` and `Wi-Fi: home` within network range.
2. If you unplugged USB to carry the Core2 to the car, reconnect USB power there or tap the middle button to resume acquisition on the internal battery. With the car parked and the diagnostics electronics awake, plug the MX+ into the OBD-II port. Disconnect any app currently using the adapter. Turn the ignition to Key On, Engine Off first, then press the physical pairing button on the MX+. The adapter stays in pairing mode for only two minutes. The Core2 searches for `OBDLink MX+`. Set `CARF_OBD_NAME` to the exact advertised name if yours has been renamed. The logger remembers the adapter address as soon as the authenticated Bluetooth connection opens, even if the vehicle has not answered yet, so later attempts reconnect without another button press. To switch or re-pair an adapter, hold the right touch zone for three seconds and release it. Press the MX+ pairing button again. This resets only the saved adapter pairing.
3. Watch `Samples saved` increase. For each sample the firmware polls the supported PIDs in sequence, then waits one second. A sample timestamp marks the end of that polling cycle. Check the screen for `Upload confirmed`, then use the MCP client to inspect the trip. A zero speed while parked is a valid measurement.
4. Test store-and-forward by stopping the receiver, recording for a minute, and restarting it. Previously saved batches should arrive once. To exercise power recovery, let the device finish a sample, turn it off, and restart it. Inspect the preceding and new sessions via MCP.
5. Unplug USB for less than 15 seconds and plug it back in. Capture continues in the same trip, because short drops such as the dip while the engine cranks are ignored. Then unplug USB for longer than 15 seconds, or tap the middle touch button. Capture pauses, and the battery can sustain a final upload. Restoring USB resumes capture immediately. To end the test with the battery still charged, power off the Core2 after it confirms the upload, then run the auto-wake test below.

## Auto-wake test (before the first drive)

The logger only records a drive if the Core2 is running when the car starts. Whether the Core2 v1.3 power chip switches the device on by itself when USB power returns has not been verified on this hardware. Check it once before relying on the logger:

1. Unplug USB and power off the Core2 (hold the power button on the left side for four to six seconds until the screen goes dark).
2. Plug in USB power, either car USB with the ignition on or a USB charger.
3. **Expected:** within about 10 seconds the screen shows `CARF trip logger` and then `SD ready`, with no button press.
4. Repeat once after letting the internal battery run flat on battery power, since the power chip may treat that shutdown differently.

If the Core2 stays dark in either case, it misses every drive that starts after a power-off or a flat battery. Until that is solved, do not power it off at the end of a drive, and press the power button whenever the screen is dark when you start the car.

## What the files mean

The device syncs each reading to `/carf/current.open` on microSD. It seals files after a maximum of 24 samples, approximately 24 seconds, a change of trip, or a pause in readings. A reboot seals the previous open file before capturing additional readings. Each sealed `.ndj` file contains one trip and ends each JSON record with a newline. A new trip starts when the polling session reconnects, after pausing and resuming, or after restarting the device. A Bluetooth interruption can split a physical drive into two logged sessions. Multiple ECU replies use the first matching measurement for each PID.

A retained `.ndj` file is queued for upload. The device removes it after HTTP 200 and a matching SHA-256 receipt for its contents. A lost response triggers a retry. Files ending in `.hold` are kept for inspection if the receiver rejects their contents or an interrupted write leaves a damaged line. For a torn final line, the device uploads complete preceding records before archiving the original.

If the card is missing at startup, or a write, seal or file check fails, the logger unmounts the card and retries mounting after 1, 2, 4, ... seconds, up to every 30 seconds, and shows the SD error meanwhile. Capture starts only after the first successful mount. Readings that are not yet on the card stay in memory, up to 16, and are written after the card returns. When memory is full, polling waits (`Waiting for SD`) instead of discarding readings. After a failed write the open file is sealed before the reading is retried in a new file. A reading that did reach the card can therefore appear in two batches with identical values, which the receiver accepts once. A full card keeps failing and needs to be emptied or replaced.

## Troubleshooting

| Display | Check |
| --- | --- |
| Pairing or connecting | Confirm ignition state, adapter name, pairing LED, and that the phone app has released the connection. |
| No supported ECU reply | Use the parked survey in the vehicle guide to check which standard PIDs the car advertises, then release the MX+ connection. EVs and some hybrids may require additional vehicle-specific PIDs. |
| No FAT32 SD; retrying, SD check/seal/write failed | Verify card format and insert it fully. The logger retries by itself, at most every 30 seconds, without a restart. The firmware does not format cards. |
| Waiting for SD | Readings are held in memory until the card works again. See the SD line on the screen. |
| Waiting for receiver | Check Wi-Fi coverage where the car parks, NAS address and port, NAS firewall, and receiver health. The USB serial monitor shows HTTP status. |
| Check upload token | Compare the local firmware token to the receiver configuration and reflash after a change. |
| UTC awaiting NTP | Verify the configured time server is reachable on the home Wi-Fi. |

To repeat the hardware-independent checks from the project root, run the following commands. The first covers the OBD response parser. The second covers adapter address capture, USB power debouncing, SD retry timing and the seal decision.

```sh
c++ -std=c++11 -Wall -Wextra -Ifirmware/include firmware/test/obd_frame_test.cpp -o /tmp/carf-obd-frame-test
/tmp/carf-obd-frame-test
c++ -std=c++11 -Wall -Wextra -Ifirmware/include firmware/test/logic_test.cpp -o /tmp/carf-logic-test
/tmp/carf-logic-test
```
