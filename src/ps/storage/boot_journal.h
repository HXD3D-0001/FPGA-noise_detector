#ifndef LFN_BOOT_JOURNAL_H
#define LFN_BOOT_JOURNAL_H
#include "storage_writer.h"
enum { BOOT_JOURNAL_BYTES=256 };
typedef enum { BOOT_SIZE_0,BOOT_READ_0,BOOT_SIZE_1,BOOT_READ_1,BOOT_CHOOSE,BOOT_WRITE,BOOT_SYNC,BOOT_READY,BOOT_FAULT } BootJournalState;
typedef struct {
 StorageBackend copies[2];char device_id[64];uint64_t sequence[2],session_id;
 bool empty[2],valid[2],provision;unsigned target,completed;BootJournalState state;StorageError error;
 uint8_t frame[BOOT_JOURNAL_BYTES];
} BootJournal;
/* TWO distinct exclusively owned files. backend.write must support offset0
 * overwrite, not append-only. Provision permits only BOTH physically empty
 * copies, never two corrupt copies. No formatting, truncate or recovery append.
 * Device identity is explicitly supplied, not guessed from board parameters. */
bool boot_journal_init(BootJournal *,const StorageBackend copies[2],const char *device,size_t length,bool provision_empty);
/* One size/read/write/sync per call,256B max; only READY exposes a new boot
 * session. Greatest valid counter+1 is synced to BOTH copies, older/invalid first, before READY.
 * A failure never grants a persistent session; RAM monitoring may use caller's
 * separately labelled temporary identity. Media/FAT crash tolerance unproven. */
void boot_journal_step(BootJournal *);
bool boot_journal_session(const BootJournal *,uint64_t *);
#endif
