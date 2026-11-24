# Collecting data from the four vehicles

CARF uses one Bluetooth Classic MX+ directly. On Core2, tap the left touch zone while parked to select the vehicle shown on screen. The selection persists across reboots. Changing vehicles starts another recording. Check the selection whenever you move the adapter. The label is supplied by the operator. Generic OBD is the default.

## What the Core2 records

Every profile polls supported engine RPM, speed and coolant temperature. Once a baseline sample is available, the logger records one supplemental request per cycle. At the beginning of each recording it requests the Mode 01 support bitmaps, following continuation bits through the advertised pages. Then it rotates through the profile's signals. The receiver keeps the original adapter text with line boundaries normalized, the request, the response format and the elapsed time when the observation completed. Raw-header mode retains individual responders on 11-bit diagnostic CAN. Standard requests on other protocols use formatted payloads with unknown responder identity.

| Selected profile | Supplemental measurements | Hardware check |
| --- | --- | --- |
| `sienna_2009` | Short- and long-term fuel trims for banks 1 and 2 using supported PIDs 06–09. The engine/transmission controller's pre-2010 ATF query is also collected as raw evidence. | Confirm the drivetrain and compare the ATF reply to a known transmission temperature on this van before assigning a decoder. |
| `x3_2017_n55` | Engine oil and automatic transmission temperatures using the F25 engine and transmission controller definitions. | Public 2017 X3 captures supply test values. Confirm both controller replies on the xDrive35i N55. |
| `335i_2007_n54` | Standard direct-injection fuel-rail gauge pressure (PID 23, kPa) if advertised. Pair this time series with RPM. | Inspect the support bitmap. A missing PID leaves BMW-specific high-pressure readings unverified. Short polling windows or a late connection can miss engine cranking. |
| `prius_2005` | Battery ECU reply containing hybrid state of charge, pack current and 14 block voltages. All 14 values in a reported spread come from the same complete response. | With the Prius parked in READY, verify block count, plausibility and units against a diagnostic reference. Engine RPM can be zero while the hybrid system is active. |

A missing reply, negative diagnostic response or incomplete multi-frame message produces no decoded value. A zero measurement remains valid. Manufacturer-specific polling runs on detected 11-bit diagnostic CAN. An older generic log has a null vehicle label. `list_trips(vehicle=...)` selects a profile, and `read_trip_samples(trip_id=..., request="21CE")` selects a particular observed request. Raw CAN surveys below are separate workstation files.

## Parked surveys and passive captures

Pair the MX+ with a computer that exposes a Bluetooth Classic serial port. Pause and disconnect the Core2 or power it off first. Use Python 3.10+ with pyserial, which is also included in PlatformIO's Python environment. List ports with `python3 -m serial.tools.list_ports`. Replace PORT with the MX+ port, not the Core2 USB port. Place recordings in the ignored `captures/` directory. The tools create new files exclusively.

```sh
python3 tools/snapshot.py --port PORT --vehicle sienna_2009 --output captures/sienna-survey.ndjson
python3 tools/listen_can.py --port PORT --vehicle sienna_2009 --seconds 15 --output captures/sienna-can.ndjson
```

1. With the car parked and diagnostics awake, run the snapshot. It saves the detected protocol, support pages, one raw reply per advertised standard live-data PID, stored/pending/permanent emissions fault responses, the freeze-frame trigger and support queries for monitor tests and vehicle information. An advertised parameter may still return no data. During the survey, other supported ECUs can respond to the same request. Each reading has its own time.
2. For a vehicle with confirmed 500 kbit/s high-speed CAN at the diagnostic connector, run the listener as a separate session. It selects raw ISO 11898 on the MX+, enables silent monitoring, and records visible arbitration IDs, payload bytes, byte lengths, adapter status and host arrival times. A Bluetooth listener can miss frames. Treat BUFFER FULL, an early stop or a partial capture as incomplete. A quiet gateway can produce an empty recording.
3. Compare two short captures made in documented conditions, such as ignition on versus engine running. An arbitration ID identifies a message on that bus. Assign a module or physical unit only when a documented mapping or independent comparison supports it. ECU response IDs in the diagnostic survey have a different purpose from broadcast IDs. Generic OBD discovery inventories responding emissions controllers and their PIDs. Manufacturer tools can enumerate additional installed modules.

### 2009 Toyota Sienna 3.5L V6

Start with the engine and transmission controller and query both banks' fuel trims. Toyota's 2009 manuals describe transmission fluid temperature and the skid-control, steering-angle and yaw sensors. Equipment such as stability control and laser cruise changes the module inventory. Compare passive DLC3 traffic with the engine survey. Toyota's Techstream CAN Bus Check is a reference for identifying the installed controllers. A Gen 2 Prius battery definition or later Sienna transmission layout does not apply here. [2009 Sienna CAN topology and options](https://charm.li/Toyota/2009/Sienna%20AWD%20V6-3.5L%20%282GR-FE%29/Repair%20and%20Diagnosis/Powertrain%20Management/Computers%20and%20Control%20Systems/Information%20Bus/Testing%20and%20Inspection/Initial%20Inspection%20and%20Diagnostic%20Overview/CAN%20Communication%20System/Diagnosis%20System/) · [ScanGauge pre-2010 transmission request](https://www.scangauge.com/xgauge/toyota-lexus-scion-cansf-transmission-temperature/)

### 2017 BMW X3 xDrive35i (F25, N55)

The DME engine computer and EGS transmission controller are useful first targets. CARF uses the published read requests for oil and transmission temperature with BMW extended addressing. BMW's F25 training diagram also identifies the central gateway, stability-control and transfer-case controllers and multiple internal networks. A connector-level listener sees traffic exposed on the diagnostic segment. For a fuller installed-module list, compare the recording with a BMW ISTA vehicle identification/control-unit tree for this vehicle. [BMW F25 training manual](https://bmwrepairguide.com/virtual-library/technical-training/ST1106-F25-Complete-Vehicle.pdf) · [BMW 2017 X3 specifications](https://www.press.bmwgroup.com/canada/article/attachment/T0267673EN/381987) · [OBDb X3 definitions and captures](https://github.com/OBDb/BMW-X3)

### 2007 BMW 335i convertible (E93, N54)

Run protocol detection first. BMW documents a production-date transition to D-CAN around March 2007. Record the door-label build month and detected protocol before interpreting an empty CAN capture. The N54 engine computer provides fuel-system diagnostics. Transmission equipment, stability control and the convertible top controller have their own data. Identify those modules using the correct E93 wiring/service information or an ISTA control-unit tree. A module's presence does not establish its diagnostic byte layout. This profile uses the standard rail-pressure request only when advertised. [BMW bus architecture and introduction dates](https://bmwtechinfo.bmwgroup.com/tech_training_manual/ST401%20Body%20Electronics%20II.pdf) · [BMW N54 fuel-system diagnostic bulletin](https://static.nhtsa.gov/odi/tsbs/2013/MC-10149587-9999.pdf)

### 2005 Toyota Prius (NHW20)

Survey the engine computer, then collect the battery ECU's multi-frame reply. The Gen 2 battery controller monitors 14 blocks, each containing two modules. Log the pack current alongside block spread to compare readings under similar conditions. The hybrid-control, battery, engine, skid-control, power-steering and gateway computers appear in Toyota's network documentation. Hybrid motor and inverter signals require their own definitions. Use the Prius wiring diagram and a parked passive capture to establish which broadcasts reach DLC3. [2005 Toyota Prius repair manual](https://www.gwzz.com/tmp/Prius-RepairManual-2005.pdf) · [Gen 2 Prius diagnostic definitions](https://docs.google.com/spreadsheets/d/1XfML_Rco29MDkUfYrssnC9WVi1eSZK5ivH6upvF-cpE/htmlview/sheet?gid=0&headers=true)

### Data sources and verification

The X3 oil and transmission definitions and the two corresponding test responses are adapted from the OBDb contributors' BMW-X3 dataset. CARF converts the definitions into Python and C++ and retains their original units. Those adapted definitions and test vectors are available under CC BY-SA 4.0. Other test measurements, including the Prius multi-frame example, are synthetic. The MX+ protocol, monitoring and addressing behavior is specified in the manufacturer's reference manual. [OBDb BMW-X3](https://github.com/OBDb/BMW-X3) · [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/) · [OBDLink programming manual, sections 8–13](https://www.scantool.net/scantool/downloads/682/obdlink_frpm_f.pdf)
