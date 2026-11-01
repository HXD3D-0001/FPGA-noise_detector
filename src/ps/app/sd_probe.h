#ifndef LFN_SD_PROBE_H
#define LFN_SD_PROBE_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
/* Probe-only SDX1 raw export. All responses are bytes: 48-byte LE header,
 * then <=4096 payload bytes. Header: magic[4],version:u8,type:u8,status:u16,
 * nonce:u64,seq:u32,offset:u64,total:u64,length:u32,file_crc32c:u32,
 * crc32c(header[0:44]+payload):u32. Types OPEN=1,DATA=2,END=3,CANCEL=4,
 * ERROR=5. Status: 0=ok,1=syntax,2=busy/stale,3=mount,4=open,5=read/size,
 * 6=offset/incomplete,7=retry limit,8=cleanup failed,9=idle timeout.
 * CRC32C uses init/final xor FFFFFFFF.
 * Decimal commands: SDXOPEN nonce path; SDXREAD nonce offset; SDXEND nonce;
 * SDXCANCEL. Single owner, read-existing only, last block retry <=3 times.
 * Call poll at least once/second (now_ms wraps naturally); idle limit 15s.
 * Busy remains true after cleanup failure, until a successful cancel.
 * command returns true if consumed; while busy all other commands reject. */
typedef bool (*LfnSdExportSend)(const uint8_t *,size_t);
bool lfn_sd_export_command(void *owner_task,LfnSdExportSend,const char *line,uint32_t now_ms);
bool lfn_sd_export_busy(void);
void lfn_sd_export_poll(LfnSdExportSend,uint32_t now_ms);
/* Probe image only, one storage owner: mount, read the entire framed file,
 * check CRC/session/strict sequence, count target capture/config data and
 * sample extents, close and unmount. RAW is PCM with CONFIG.mode != 0.
 * No write/repair and no full metadata-schema or acoustic validation. */
bool lfn_sd_verify_run(void *owner_task, void (*emit)(const char *line),
 uint64_t session, uint64_t capture, uint32_t config, const char *relative_path);
/* One-shot READ-ONLY SD0 probe. It must run in one task context that becomes
 * the sole FatFs owner: mount logical drive "0:", report filesystem type,
 * total/free space and up to 32 root entries, then unmount. It never opens a
 * file for writing, creates, renames, formats or deletes anything; no boot
 * media is touched. Output lines are delivered through emit() (no trailing
 * newline assumptions) and the final marker is "LFN SD DONE". */
bool lfn_sd_probe_run(void *owner_task, void (*emit)(const char *line));
/* AUTHORIZED WRITE PROBE: create exactly ONE new 8.3 file (CREATE_NEW after a
 * missing-name check), write 4096 deterministic bytes, sync+close, unmount,
 * remount and read back. Never overwrites, formats, renames or deletes any
 * existing file; the created file is kept for offline inspection. */
bool lfn_sd_write_probe_run(void *owner_task, void (*emit)(const char *line));
/* AUTHORIZED CLEANUP: delete ONLY this fixed list of experiment files created
 * by earlier probes (exact 8.3 names). Pre-existing user files are never
 * touched; missing targets are skipped; before/after listings are reported. */
bool lfn_sd_cleanup_run(void *owner_task, void (*emit)(const char *line));
/* Probe-only directory helpers: create ONE new 8.3 directory (existing is
 * reported, never deleted), and read-only list one subdirectory. */
bool lfn_sd_mkdir_probe_run(void *owner_task, void (*emit)(const char *line), const char *dir);
bool lfn_sd_list_probe_run(void *owner_task, void (*emit)(const char *line), const char *dir);
/* Authorised cleanup: remove ONLY files inside a named development directory
 * (prefixes LFNM2/MW/MF/SR) then the empty directory itself. Never touches any
 * other root entry, never recursive, and refuses subdirectories. */
bool lfn_sd_purge_run(void *owner_task, void (*emit)(const char *line), const char *dir);
bool lfn_sd_purge_all_run(void *owner_task, void (*emit)(const char *line));
/* Volume owner check used by other probe modules in the same task. */
bool (*lfn_sd_probe_owner_ok(void *owner_task))(void *);
#endif
