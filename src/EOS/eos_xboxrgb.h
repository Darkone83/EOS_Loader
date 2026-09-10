// eos_xboxrgb.h -- optional EOS Loader -> XBOX-RGB LAN integration.
// Discovery and transient bank-event control use XBOX-RGB's existing UDP 7777
// protocol. Failure/no device is always non-fatal to bank launch.
#pragma once

void XboxRgb_Init(void);
void XboxRgb_Tick(void);
int  XboxRgb_Present(void);

// bank is a display/event id (1..4 user banks, 5 recovery); rgb = 0x00RRGGBB.
// Returns 1 if the UDP packet was queued by Winsock, 0 if networking is down.
int  XboxRgb_BankEvent(int bank, unsigned int rgb, unsigned long durationMs);