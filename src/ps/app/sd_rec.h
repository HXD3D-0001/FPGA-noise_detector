#ifndef LFN_SD_REC_H
#define LFN_SD_REC_H
#include <stdbool.h>
#include <stdint.h>
/* One-shot REAL-CARD record persistence probe for exactly ONE new ".LFR" file.
 * Builds SESSION/CONFIG/CAPTURE definitions, one ENERGY and one PSD record
 * through the storage task (SD mode), waits for all five records SYNCED, then
 * closes so the task performs f_sync+f_close+unmount. It never overwrites,
 * formats, renames or deletes an existing file. The created file is kept. */
bool lfn_sd_rec_run(void (*emit)(const char *line));
/* Continuous-write variant used for power-loss injection: creates one new .LFR
 * file with the same definition set, then keeps submitting PSD records. Prints
 * the session id and file name needed for the later existing-file recovery. */
bool lfn_sd_load_run(unsigned count, void (*emit)(const char *line));
/* Open the named EXISTING .LFR file, scan/repair its tail, report recovered
 * counters, then close cleanly. Never creates, overwrites or deletes files. */
bool lfn_sd_open_run(uint64_t session, const char *relative_path, void (*emit)(const char *line));
/* Dev/probe: create one NEW 8.3 directory, start real-card multi-file owner
 * (provision only, no cold start/rotation) under it, and report readiness. */
bool lfn_sd_mf_run(const char *dir, void (*emit)(const char *line));
bool lfn_sd_ls_run(const char *dir, void (*emit)(const char *line));
/* Read-only sealed-file replay: scans the valid prefix of each MON/RAW data
 * file in one directory and reports framing/sequence counts. No write/repair. */
bool lfn_sd_replay_run(uint64_t session, const char *dir, void (*emit)(const char *line));
/* Crash probe: cold start + continuous PSD writes until the board is reset. */
bool lfn_sd_mw_run(const char *dir, void (*emit)(const char *line));
bool lfn_sd_purge_dir(const char *dir, void (*emit)(const char *line));
bool lfn_sd_purge_all(void (*emit)(const char *line));
#endif