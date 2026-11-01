#ifndef LFN_STORAGE_RETENTION_H
#define LFN_STORAGE_RETENTION_H
#include "storage_file_catalog.h"
typedef enum { RETAIN_PSD,RETAIN_PCM,RETAIN_SUMMARY,RETAIN_NOTES,RETAIN_METADATA,RETAIN_FAMILIES } StorageRetentionFamily;
typedef struct {uint64_t total_bytes,reserve_free_bytes,limit[RETAIN_FAMILIES],target_age_seconds[RETAIN_FAMILIES];} StorageRetentionPolicy;
typedef struct {
 uint64_t session_id,file_id,catalog_id,ledger_id,bytes,order_key,age_seconds;
 StorageRetentionFamily family;unsigned references;bool age_known,active,journal,closed,seal_verified;
 char name[STORAGE_CATALOG_NAME];
} StorageRetentionFile;
typedef enum { RETENTION_KEEP,RETENTION_DELETE_ONE,RETENTION_NO_SPACE,RETENTION_INVALID } StorageRetentionAction;
typedef struct {
 StorageRetentionPolicy policy;StorageRetentionFamily incoming_family;uint64_t incoming_bytes,free_bytes,total,by_family[RETAIN_FAMILIES],count;
 StorageRetentionFile oldest[RETAIN_FAMILIES],expired;bool have[RETAIN_FAMILIES],have_expired,invalid;
} StorageRetentionScan;
/* Defaults from design:8GiB total,512MiB spare, per-family2/2/1GiB,
 * 256/64MiB.7day PSD and30day summaries are targets, not guaranteed retention.
 * No card capacity/format inference. Physical free bytes come from backend. */
StorageRetentionPolicy storage_retention_default(void);
bool storage_retention_begin(StorageRetentionScan *,const StorageRetentionPolicy *,StorageRetentionFamily,uint64_t incoming_bytes,uint64_t physical_free_bytes);
/* Feed each managed file exactly once under sole-owner inventory snapshot.
 * Streaming scan: constant RAM regardless of file count; caller's filesystem
 * enumerator supplies unique IDs and charged sizes including local definitions.
 * Historical seal alone is NOT seal_verified. Active/journal/open/pinned files
 * count toward quotas but are never candidates. No IO or deletion here. */
bool storage_retention_feed(StorageRetentionScan *,const StorageRetentionFile *);
StorageRetentionAction storage_retention_finish(const StorageRetentionScan *,StorageRetentionFile *victim);
/* After each actual deletion, query real free space and rescan. Do not assume
 * logical file bytes equal released FAT clusters, or retry stale inventory. */
#endif
