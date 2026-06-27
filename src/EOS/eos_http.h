// eos_http.h -- tiny non-blocking HTTP server for the Eos web UI.
//
// Single-connection, tick-polled (no threads): call Http_Poll() once per frame.
// It accepts one client at a time, parses the request across frames, and for a
// flash upload streams the body into a staging buffer over several ticks so the
// loader's frame loop never stalls. All endpoints act on the same Bank_* /
// Config_Save / Flash_WriteImage table the on-console UI uses.
//
// Endpoints:
//   GET  /                 web UI (HTML)
//   GET  /logo.bmp         the EOS splash, expanded to a BMP
//   GET  /api/banks        JSON: every bank's name/occupied/size/boot
//   POST /api/rename?b=N   body = new name        -> Bank_SetName + Config_Save
//   POST /api/delete?b=N                           -> erase + clear + Config_Save
//   POST /api/flash?b=N    body = BIOS image bytes -> Flash_WriteImage + Config_Save
//   POST /api/launch?b=N                           -> warm-reset into bank N
#pragma once

void Http_Start(void);   // bind + listen on :8008 (call once the net is up)
void Http_Stop(void);    // close listener + any active connection
int  Http_IsUp(void);    // 1 if listening
void Http_Poll(void);    // advance accept/recv/process/send (non-blocking)#pragma once
