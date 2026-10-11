#pragma once
// EOS Phase 5 - shared WebUI file manager. No changes to BIOS/flash interfaces.
// Public API never accepts native Xbox file paths: only /HDD0/E/..., /HDD1/E/... or /SD/...
#define EOS_WF_PATH 240
#define EOS_WF_JSON 8192

enum EosWfResult {
    WF_OK = 0, WF_BADPATH = -1, WF_DENIED = -2, WF_NOTFOUND = -3,
    WF_EXISTS = -4, WF_IOERROR = -5, WF_UNAVAILABLE = -6,
    WF_BUSY = -7, WF_DEPTH = -8, WF_INCOMPLETE = -9, WF_STALE = -10
};

const char* Wf_Error(int code);
int Wf_List(const char* virtualPath, int page, char* json, int capacity);
int Wf_Mkdir(const char* virtualPath);
int Wf_Rename(const char* virtualPath, const char* newName);
// Files delete immediately; directory trees run a bounded deletion job.
// Returns 1 for a running job, 0 when completed synchronously, or an error.
int Wf_Delete(const char* virtualPath);
void Wf_Tick(void);
int Wf_JobInfo(char* json, int capacity);
int Wf_JobBusy(void);
int Wf_UploadBegin(const char* virtualPath, int byteCount, int overwrite);
int Wf_UploadWrite(const char* data, int len);
int Wf_UploadFinish(void);
void Wf_UploadAbort(void);
int Wf_DownloadOpen(const char* virtualPath, unsigned long long* sizeOut, char* nameOut, int nameCapacity);
int Wf_DownloadRead(char* dst, int size); // 0 for EOF, -1 for I/O failure
void Wf_DownloadClose(void);
void Wf_Shutdown(void);
