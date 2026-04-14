#pragma once

// USB CDC <-> UART bridge for ELRS receiver flashing
// Enters bridge mode, returns when user holds BOOT button
void runUSB2TTL();
