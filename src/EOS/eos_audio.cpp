/*---------------------------------------------------------------------------
    eos_audio.cpp -- background music for the Eos loader. minimp3 decode ->
    DirectSound, streamed through a 256KB ring buffer + worker thread.

    Ported from DarkDash's dd_audio (music path only). A full 3-4 min track is
    ~40MB of PCM and will not fit one DS buffer, so the whole MP3 is held in RAM
    and a worker thread decodes it into the ring behind the play cursor, looping.

    Defines the minimp3 implementation in this TU. Links dsound.lib.
---------------------------------------------------------------------------*/
#include <xtl.h>
#include <dsound.h>
#include <stdlib.h>          /* malloc / free / realloc */

/* Xbox CPU is a Pentium III (SSE1 only); minimp3's SSE2 SIMD path also pulls
   intrin0.inl.h, which clashes with xtl.h's _Interlocked* decls. The scalar
   path is correct for this CPU and conflict-free. */
#define MINIMP3_NO_SIMD
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#include "eos_audio.h"
#include "eos_file.h"        /* File_ReadInto */

#define AUDIO_STREAM_BUFSIZE (256 * 1024)
#define AUDIO_STREAM_HALF    (AUDIO_STREAM_BUFSIZE / 2)
#define EOS_BGM_FILE_MAX     (10 * 1024 * 1024)   /* cap for the whole MP3 in RAM */
#define AUDIO_PATH_MAX       272

static LPDIRECTSOUND       s_ds = NULL;

/* ---- music streaming state ----------------------------------------------- */
static LPDIRECTSOUNDBUFFER s_music = NULL;     /* the 256KB streaming ring */
static unsigned char* s_musicData = NULL; /* whole mp3 file in RAM    */
static DWORD               s_musicSize = 0;
static DWORD               s_musicPos = 0;     /* read cursor into the mp3 */
static mp3dec_t            s_musicDec;
static HANDLE              s_musicThread = NULL;
static LONG                s_musicStop = 0;
static LONG                s_musicGen = 0;     /* bumped on each stop; a fill thread whose
                                                  captured generation goes stale exits, so a
                                                  detached zombie can never touch a new stream */
static int                 s_musicLoop = 1;

static CRITICAL_SECTION    s_musicCS;          /* guards the decode/read state       */
static CRITICAL_SECTION    s_dsCS;             /* serializes EVERY DirectSound call  */
static int                 s_csReady = 0;

/* carry-over for a decoded frame that straddles a half boundary (avoids pops) */
static short  s_pcmCarry[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
static DWORD  s_carryBytes = 0;

static LONG   s_musicVolPct = 70;              /* 0..100 last requested percent */
static LONG   s_musicVolDb = 0;               /* DirectSound attenuation       */
static char   s_musicFile[AUDIO_PATH_MAX] = { 0 };

/* ---- helpers -------------------------------------------------------------- */
static void strCopyN(char* d, const char* s, int cap)
{
    int i = 0;
    if (s) for (; s[i] && i < cap - 1; ++i) d[i] = s[i];
    d[i] = 0;
}

/* Load the whole file into a malloc'd buffer via File_ReadInto (which returns
   -1 if the file exceeds cap). 1 on success (*out/*outLen set), 0 on failure. */
static int loadFile(unsigned char** out, DWORD* outLen, const char* path)
{
    unsigned char* buf;
    int n;
    *out = NULL; *outLen = 0;
    buf = (unsigned char*)malloc(EOS_BGM_FILE_MAX);
    if (!buf) return 0;
    n = File_ReadInto(path, buf, EOS_BGM_FILE_MAX);
    if (n <= 0) { free(buf); return 0; }
    {
        unsigned char* t = (unsigned char*)realloc(buf, (size_t)n);   /* trim to actual size */
        if (t) buf = t;
    }
    *out = buf; *outLen = (DWORD)n;
    return 1;
}

/* ---- streaming fill ------------------------------------------------------- */
static void FillHalf(int nHalf)
{
    void* pData1 = NULL;
    DWORD  dwLen1 = 0;
    BYTE* pDst;
    DWORD  dwFilled;
    short  pcm[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];

    if (!s_music) return;
    {
        HRESULT hrLock;
        EnterCriticalSection(&s_dsCS);
        hrLock = s_music->Lock((DWORD)nHalf * AUDIO_STREAM_HALF, AUDIO_STREAM_HALF,
            &pData1, &dwLen1, NULL, NULL, 0);
        LeaveCriticalSection(&s_dsCS);
        if (FAILED(hrLock)) return;
    }

    pDst = (BYTE*)pData1;
    dwFilled = 0;

    EnterCriticalSection(&s_musicCS);

    /* drain any carry-over from the previous fill first */
    if (s_carryBytes > 0) {
        DWORD canWrite = s_carryBytes < dwLen1 ? s_carryBytes : dwLen1;
        memcpy(pDst, s_pcmCarry, canWrite);
        dwFilled = canWrite;
        if (canWrite < s_carryBytes)
            memmove(s_pcmCarry, (BYTE*)s_pcmCarry + canWrite, s_carryBytes - canWrite);
        s_carryBytes -= canWrite;
    }

    while (dwFilled < dwLen1) {
        mp3dec_frame_info_t info;
        int   nSamples;
        DWORD dwFrameBytes, dwCanWrite;

        if (s_musicPos >= s_musicSize) {
            if (!s_musicLoop) { memset(pDst + dwFilled, 0, dwLen1 - dwFilled); dwFilled = dwLen1; break; }
            s_musicPos = 0; s_carryBytes = 0;   /* loop back to the start */
        }

        nSamples = mp3dec_decode_frame(&s_musicDec, s_musicData + s_musicPos,
            (int)(s_musicSize - s_musicPos), pcm, &info);

        /* frame_bytes > 0 = forward progress (real frame or skipped junk). == 0
           means nothing usable here: wrap ONCE off a bad spot (never from pos 0,
           which would spin forever), else end the stream with silence. This keeps
           the read position strictly advancing so the fill thread always exits. */
        if (info.frame_bytes > 0) {
            s_musicPos += (DWORD)info.frame_bytes;
        }
        else if (s_musicLoop && s_musicPos != 0) {
            s_musicPos = 0; s_carryBytes = 0;
        }
        else {
            memset(pDst + dwFilled, 0, dwLen1 - dwFilled); dwFilled = dwLen1; break;
        }
        if (nSamples <= 0) continue;

        dwFrameBytes = (DWORD)(nSamples * info.channels * (int)sizeof(short));
        dwCanWrite = dwLen1 - dwFilled;

        if (dwFrameBytes <= dwCanWrite) {
            memcpy(pDst + dwFilled, pcm, dwFrameBytes);
            dwFilled += dwFrameBytes;
        }
        else {
            memcpy(pDst + dwFilled, pcm, dwCanWrite);
            dwFilled = dwLen1;
            s_carryBytes = dwFrameBytes - dwCanWrite;
            memcpy(s_pcmCarry, (BYTE*)pcm + dwCanWrite, s_carryBytes);
        }
    }

    LeaveCriticalSection(&s_musicCS);

    EnterCriticalSection(&s_dsCS);
    s_music->Unlock(pData1, dwLen1, NULL, 0);
    LeaveCriticalSection(&s_dsCS);
}

static DWORD WINAPI MusicThreadProc(LPVOID pParam)
{
    /* StartMusic primes both halves before Play() and the cursor starts in half
       0, so mark half 1 already filled -- don't overwrite unplayed primed audio. */
    int  lastFilled = 1;
    LONG myGen = (LONG)(LONG_PTR)pParam;

    while (!InterlockedCompareExchange(&s_musicStop, 0, 0) &&
        myGen == InterlockedCompareExchange(&s_musicGen, 0, 0)) {
        DWORD dwPlay = 0, dwWrite = 0;
        int   playHalf, fillHalf;

        if (!s_music) { lastFilled = -1; Sleep(4); continue; }

        EnterCriticalSection(&s_dsCS);
        if (s_music) s_music->GetCurrentPosition(&dwPlay, &dwWrite);
        LeaveCriticalSection(&s_dsCS);

        playHalf = (dwPlay < (DWORD)AUDIO_STREAM_HALF) ? 0 : 1;
        fillHalf = 1 - playHalf;           /* the half behind the cursor */
        if (fillHalf != lastFilled) { FillHalf(fillHalf); lastFilled = fillHalf; }
        Sleep(4);
    }
    return 0;
}

/* =========================================================================
   Public
   ========================================================================= */
int Audio_Init(void)
{
    HRESULT hr;
    if (s_ds) return 1;
    hr = DirectSoundCreate(NULL, &s_ds, NULL);
    if (FAILED(hr) || !s_ds) return 0;
    InitializeCriticalSection(&s_musicCS);
    InitializeCriticalSection(&s_dsCS);
    s_csReady = 1;
    return 1;
}

void Audio_Update(void)
{
    /* Required on Xbox: services the DS mixer each frame. Serialized against the
       fill thread's Lock/Unlock on the same device state. */
    EnterCriticalSection(&s_dsCS);
    DirectSoundDoWork();
    LeaveCriticalSection(&s_dsCS);
}

void Audio_SetMusicPath(const char* fullPath) { strCopyN(s_musicFile, fullPath, sizeof(s_musicFile)); }

int Audio_MusicPlaying(void) { return s_music ? 1 : 0; }

/* map 0..100% to a perceptual dB attenuation (quadratic taper, 0..-40 dB). */
static LONG VolPctToDb(int pct)
{
    if (pct <= 0)   return -10000;
    if (pct >= 100) return 0;
    { long span = 4000, t = 100 - pct; return -(span * t * t / 10000); }
}

void Audio_SetMusicVolume(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    s_musicVolPct = pct;
    s_musicVolDb = VolPctToDb(pct);
    EnterCriticalSection(&s_dsCS);
    if (s_music) s_music->SetVolume(s_musicVolDb);
    LeaveCriticalSection(&s_dsCS);
}

int Audio_GetMusicVolume(void) { return (int)s_musicVolPct; }

void Audio_StartMusic(int loop)
{
    unsigned char* data = NULL;
    DWORD size = 0;
    mp3dec_frame_info_t info;
    short probe[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
    WAVEFORMATEX wfx;
    DSBUFFERDESC dsbd;

    if (!s_ds) return;
    if (!s_musicFile[0]) return;         /* no track selected */

    Audio_StopMusic();                   /* tear down any prior stream */

    /* If that stop hit its backstop (a wedged DSound Lock left a fill thread
       lingering), drain bounded before reusing shared globals; else detach and
       hand the new stream a clean slate rather than freeze the dashboard. */
    if (s_musicThread) {
        if (WaitForSingleObject(s_musicThread, 2000) == WAIT_OBJECT_0) {
            CloseHandle(s_musicThread); s_musicThread = NULL;
            EnterCriticalSection(&s_dsCS);
            if (s_music) { s_music->Stop(); s_music->Release(); s_music = NULL; }
            LeaveCriticalSection(&s_dsCS);
            if (s_musicData) { free(s_musicData); s_musicData = NULL; }
            InterlockedExchange(&s_musicStop, 0);
        }
        else {
            s_musicThread = NULL; s_music = NULL; s_musicData = NULL;
            InterlockedExchange(&s_musicStop, 0);
        }
    }

    if (!loadFile(&data, &size, s_musicFile)) return;   /* missing/too big -> no music */
    s_musicData = data;
    s_musicSize = size;
    s_musicPos = 0;
    s_musicLoop = loop ? 1 : 0;
    s_carryBytes = 0;

    /* probe the first frame for channels/hz; require an actually decoded frame
       (a truncated file parses a header but decodes zero samples -> reject). */
    mp3dec_init(&s_musicDec);
    {
        int probeN = mp3dec_decode_frame(&s_musicDec, s_musicData, (int)s_musicSize, probe, &info);
        if (probeN <= 0 || !info.hz || !info.channels) {
            free(s_musicData); s_musicData = NULL; return;
        }
    }
    mp3dec_init(&s_musicDec);            /* re-init: stream cleanly from frame 0 */
    s_musicPos = 0;

    ZeroMemory(&wfx, sizeof(wfx));
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = (WORD)info.channels;
    wfx.nSamplesPerSec = (DWORD)info.hz;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = (WORD)(info.channels * 2);
    wfx.nAvgBytesPerSec = (DWORD)(info.hz * info.channels * 2);

    ZeroMemory(&dsbd, sizeof(dsbd));
    dsbd.dwSize = sizeof(dsbd);
    dsbd.dwFlags = DSBCAPS_CTRLVOLUME;
    dsbd.dwBufferBytes = AUDIO_STREAM_BUFSIZE;   /* 256KB, not the whole track */
    dsbd.lpwfxFormat = &wfx;

    {
        HRESULT hrCreate;
        EnterCriticalSection(&s_dsCS);
        hrCreate = s_ds->CreateSoundBuffer(&dsbd, &s_music, NULL);
        LeaveCriticalSection(&s_dsCS);
        if (FAILED(hrCreate) || !s_music) {
            s_music = NULL; free(s_musicData); s_musicData = NULL; return;
        }
    }

    FillHalf(0);
    FillHalf(1);
    EnterCriticalSection(&s_dsCS);
    if (s_music) {
        s_music->SetVolume(s_musicVolDb);
        s_music->Play(0, 0, DSBPLAY_LOOPING);   /* ring always loops; EOF handled in fill */
    }
    LeaveCriticalSection(&s_dsCS);

    InterlockedExchange(&s_musicStop, 0);
    {
        LONG gen = InterlockedCompareExchange(&s_musicGen, 0, 0);
        s_musicThread = CreateThread(NULL, 0, MusicThreadProc, (LPVOID)(LONG_PTR)gen, 0, NULL);
    }
}

void Audio_StopMusic(void)
{
    int joined = 1;
    if (s_musicThread) {
        int guard = 0;
        InterlockedIncrement(&s_musicGen);      /* invalidate this thread's generation */
        InterlockedExchange(&s_musicStop, 1);
        /* Wait for the fill thread to ACTUALLY exit before touching the buffer:
           releasing s_music mid-Lock is a use-after-free. Bounded backstop for a
           driver-wedged Lock (device already dead) rather than an infinite hang. */
        while (WaitForSingleObject(s_musicThread, 250) == WAIT_TIMEOUT) {
            if (++guard >= 60) { joined = 0; break; }   /* ~15s backstop */
        }
        if (joined) { CloseHandle(s_musicThread); s_musicThread = NULL; }
    }

    if (joined) {
        InterlockedExchange(&s_musicStop, 0);
        EnterCriticalSection(&s_dsCS);
        if (s_music) { s_music->Stop(); s_music->Release(); s_music = NULL; }
        LeaveCriticalSection(&s_dsCS);
        if (s_musicData) { free(s_musicData); s_musicData = NULL; }
    }
    /* else: thread never returned from a wedged Lock -- leak rather than free under
       a live thread; StartMusic drains it before reusing the globals, and
       s_musicStop stays set so the zombie exits the instant its Lock unblocks. */

    s_musicSize = 0; s_musicPos = 0; s_carryBytes = 0;
}

void Audio_Shutdown(void)
{
    Audio_StopMusic();
    if (s_csReady) {
        DeleteCriticalSection(&s_musicCS);
        DeleteCriticalSection(&s_dsCS);
        s_csReady = 0;
    }
    if (s_ds) { s_ds->Release(); s_ds = NULL; }
}