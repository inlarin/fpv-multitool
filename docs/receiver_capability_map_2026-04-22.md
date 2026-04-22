# Receiver Tab Capability & Dependency Map (2026-04-22)

## Capability Matrix by User Intent

### DIAGNOSE
- **Probe RX** -> POST /api/elrs/rx_mode (Port B free) -> mode (dfu/stub/app/silent)
- **Scan receiver** -> POST /api/elrs/receiver_info (RX in DFU) -> chip+OTADATA+slots
- **Chip/MAC** -> POST /api/elrs/chip_info (RX in DFU) -> ESP magic + MAC

### FLASH FIRMWARE
- **Download** -> POST /api/flash/upload (2 MB PSRAM avail) -> caches firmware
- **Upload from disk** -> POST /api/flash/upload (file selected) -> caches firmware
- **Flash** -> POST /api/flash/start (fw cached, Port B free) -> flashes @115200 or @420000
- **Download dump** -> GET /api/flash/dump/download (dump ready) -> elrs_dump.bin

### MODE SWITCHING
- **Enter stub** -> POST /api/crsf/reboot_to_bl (RX in app) -> CRSF 'bl' frame
- **Exit DFU/stub** -> POST /api/flash/exit_dfu (DFU/stub) -> RUN_USER_CODE
- **Boot app0** -> POST /api/otadata/select slot=0 (DFU) -> OTADATA write
- **Boot app1** -> POST /api/otadata/select slot=1 (DFU) -> OTADATA write
- **Soft reboot** -> POST /api/crsf/reboot_app (app or monitor) -> CRSF COMMAND

### CONFIGURE / PAIR
- **Load params** -> GET /api/elrs/params (RX in app) -> reads LUA fields (~2s)
- **Write param** -> POST /api/elrs/params/write (RX in app) -> PARAMETER_WRITE
- **Bind** -> POST /api/elrs/bind (RX in app) -> CRSF 'bd' frame

### MONITOR
- **Start live** -> POST /api/crsf/start (Port B free) -> CRSFService @420000
- **Stop** -> POST /api/crsf/stop (running) -> releases Port B

## Advanced Cards
**Bootloader:** Boot app0/1, Enter stub, Exit DFU
**Slot-Flash:** Upload, Erase+Flash (DFU)
**OTADATA:** Refresh, Erase OTADATA, Erase app1
**Dump:** Start (DFU, 3-5min for 4MB), Download, Clear
**Hardware.JSON:** Download (client-side JS)

## Stale JS Helpers
- rxWizStepDfu/Otadata/Erase/Flash/OtadataWrite (orphaned wizard, no HTML caller)
- catFetch/catModel/renderCatalog (old catalog picker, unused)
- detectChip (legacy)
- Endpoints unused: /api/elrs/device_info, /api/flash/read_bytes, /api/flash/md5

## Precondition Overlaps
1. Three Flash Paths - Flash (auto), Slot (manual), Boot (OTADATA only) -> confusing
2. Two Reboot Paths - Standalone vs CRSFService -> unclear
3. Load Params - disabled if not app mode, but STUB shows in options
4. Exit DFU vs Soft Reboot - unclear when to use each

## Port B Contention
All UART ops return 409 if busy: Probe(1s), Params(2s), Flash(30-120s), Dump(3-5min)
Auto-pause: CRSF service pauses during flash; UI warns if monitor running

## Architectural Limits
- Force WiFi: vanilla ELRS no MSP dispatch over UART
- Exit WiFi: no CRSF opcode
- Force ROM DFU: requires physical BOOT + power-cycle

## Summary
- Buttons: 45 | Endpoints: 20+ | Stale JS: 6 | Overlaps: 4
- Workflows: Diagnose (Probe->Scan), Update (Catalog->Flash), Switch (Scan->Boot), Dump (DFU->Dump->parse)
