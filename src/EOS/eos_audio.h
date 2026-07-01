#ifndef EOS_AUDIO_H
#define EOS_AUDIO_H
/*---------------------------------------------------------------------------
    eos_audio -- background music for the Eos loader.

    minimp3 decode -> DirectSound, streamed through a 256KB ring buffer and a
    worker thread. Ported from DarkDash's dd_audio, music path only: no SFX,
    no reactivity/levels, no shuffle. One user-selected track, optionally looped.

    Usage:
        Audio_Init();                         // once, after boot
        Audio_SetMusicPath(Config_GetBgmPath());
        if (Config_GetBgmOn()) Audio_StartMusic(1);
        ... each frame: Audio_Update();
        Audio_Shutdown();                     // at exit
---------------------------------------------------------------------------*/
#include <xtl.h>

#ifdef __cplusplus
extern "C" {
#endif

    int  Audio_Init(void);                 /* create the DirectSound device; 1 on success */
    void Audio_Shutdown(void);

    /* Service the Xbox DirectSound mixer. MUST be called once per frame or the
       play cursor stalls and nothing is audible (DirectSoundDoWork). */
    void Audio_Update(void);

    void Audio_SetMusicPath(const char* fullPath);  /* full path to the .mp3 (applies on next Start) */
    void Audio_StartMusic(int loop);                /* stream the set path (loop != 0 to loop) */
    void Audio_StopMusic(void);
    int  Audio_MusicPlaying(void);                  /* 1 while a stream is active */

    void Audio_SetMusicVolume(int pct);             /* 0..100 percent; live if playing */
    int  Audio_GetMusicVolume(void);

#ifdef __cplusplus
}
#endif
#endif /* EOS_AUDIO_H */