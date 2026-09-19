#pragma once
// eos_file.h -- slim, heap-free file access for the Eos loader.
//
// Just what flashing/theme/audio needs: mount HDD partitions, enumerate + read,
// and expose the EOS FAT32 card through a tiny virtual "SD:\\..." path. No MU,
// no tree copy/delete -- those live in the DarkDash file manager.
#pragma once
#include <xtl.h>

#define EOS_FILE_NAME_MAX     64
#define EOS_FILE_PATH_MAX     256
#define EOS_FILE_MAX_ENTRIES  128

struct EosFileEntry {
    char name[EOS_FILE_NAME_MAX];
    int  is_dir;
};

// Bind HDD partitions to drive letters (idempotent; safe to call repeatedly).
void File_MountDrives(void);

// Mountable drive roots that actually resolve, e.g. "C:" "E:" "F:" ... Absent
// partitions (F:/G: on a stock drive) are skipped. Returns count in out[].
int  File_ListDrives(EosFileEntry* out, int maxEntries);

// Directory entries (dotfiles skipped). path like "E:\\eos" or
// "SD:\\Eos\\Themes". Returns count.
int  File_ListDir(const char* path, EosFileEntry* out, int maxEntries);

int  File_IsDir(const char* path);
int  File_Exists(const char* path);

// Read a whole file into buf (up to cap bytes). Returns bytes read, or -1 on
// error -- including a file larger than cap (caller decides the cap).
int  File_ReadInto(const char* path, unsigned char* buf, int cap);