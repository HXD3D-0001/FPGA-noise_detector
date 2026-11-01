#ifndef LFN_STORAGE_HISTORY_H
#define LFN_STORAGE_HISTORY_H
#include "storage_metadata.h"
typedef enum { HISTORY_SIZE,HISTORY_HEADER,HISTORY_BODY,HISTORY_RECORD,HISTORY_DONE,HISTORY_TAIL,HISTORY_IO,HISTORY_CANCELLED,HISTORY_CAPACITY } StorageHistoryState;
typedef struct {
 StorageBackend backend;uint64_t session_id,file_id,limit,offset,prefix,last_sequence,records;
 uint32_t crc,expected_crc;bool exact_size,check_crc;StorageHistoryState state;StorageError error;
 size_t used,total;RecordView view;StorageMetadataRegistry registry;MetadataJson scratch;uint8_t record[RECORD_MAX_BYTES];
} StorageHistoryReader;
/* Caller owns stable byte range; live limit must be writer/family durable
 * prefix. A historical catalog claim may set exact/check_crc, but remains
 * historical evidence, never a recovered writable seed or old receipt.
 * ~600KiB persistent object, not a capture stack. Read-only, one IO<=4KiB. */
bool storage_history_init(StorageHistoryReader *,const StorageBackend *,uint64_t session,uint64_t file,uint64_t limit,bool exact_size,bool check_crc,uint32_t expected_crc);
void storage_history_step(StorageHistoryReader *);
/* RECORD view is borrowed ONLY by sole storage owner. Network must receive
 * private <=4KiB copies through query mailbox. Advance after final chunk ACK. */
bool storage_history_advance(StorageHistoryReader *);
void storage_history_cancel(StorageHistoryReader *);
#endif
