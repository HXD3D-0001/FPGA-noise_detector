#ifndef LFN_STORAGE_PUMP_H
#define LFN_STORAGE_PUMP_H
#include "storage_service.h"
enum { STORAGE_PUMP_SLOTS=4,STORAGE_PUMP_RESERVED=1 };
typedef enum { PUMP_METADATA=1,PUMP_ENERGY,PUMP_PSD,PUMP_PCM,PUMP_INGRESS_LOSS,PUMP_SUMMARY } StoragePumpKind;
typedef struct {uint64_t generation;uint32_t slot;} StoragePumpTicket;
typedef struct {
 bool done,aborted,have_remainder;StorageSubmit last_submit;unsigned pieces;
 uint64_t first_sequence,last_sequence;StoragePcmRemainder remainder;
} StoragePumpResult;
typedef union {uint8_t bytes[LFN_MAX_PACKET];LfnServiceMessage energy;LfnSpectrum spectrum;StorageIngressLoss loss;MinuteSummary summary;} StoragePumpPayload;
typedef struct {
 uint64_t generation,order,session;unsigned state;bool abort_requested;
 StoragePumpKind kind;RecordIdentity identity;size_t bytes;
 StoragePumpPayload payload;StoragePumpResult result;
} StoragePumpSlot;
typedef struct {
 uint64_t accepted,rejected,completed,steps;unsigned occupied;
 bool closed,drained,fatal,admin_pending;StorageResult admin_result;
 uint64_t synced,failed,unsaved_overflow;unsigned unsaved_ranges,inflight_ranges;
 StorageWriterState writer_state;StorageError writer_error;
} StoragePumpHealth;
typedef struct {
 StorageService *service;StorageQueueLocks locks;StoragePumpSlot slots[STORAGE_PUMP_SLOTS];
 uint64_t next_order;StoragePumpHealth health;unsigned admin;bool admin_active;
 /* Sole consumer state; no producer reads these fields. */
 unsigned active;bool staged,publish_pending,admin_publish;StorageResult admin_completion;StoragePumpResult completion;
} StoragePump;
/* Four retained command slots; first slot reserved from ordinary data. All
 * producer APIs copy values under bounded try-lock and never touch service/IO.
 * One dedicated consumer calls step. NULL locks only for serialized Host. */
bool storage_pump_init(StoragePump *,StorageService *,const StorageQueueLocks *);
/* Reopens a drained closed EVENT slot without resetting generation/order. */
StorageResult storage_pump_rebind_empty(StoragePump *,StorageService *);
StorageResult storage_pump_metadata(StoragePump *,const RecordIdentity *,const uint8_t *,size_t,StoragePumpTicket *);
StorageResult storage_pump_energy(StoragePump *,const LfnServiceMessage *,StoragePumpTicket *);
StorageResult storage_pump_ingress_loss(StoragePump *,const StorageIngressLoss *,StoragePumpTicket *);
StorageResult storage_pump_summary(StoragePump *,const MinuteSummary *,StoragePumpTicket *);
StorageResult storage_pump_psd(StoragePump *,const LfnSpectrum *,StoragePumpTicket *);
StorageResult storage_pump_pcm(StoragePump *,uint64_t,const uint8_t *,size_t,StoragePumpTicket *);
/* done/pieces mean queue admission, NEVER persistence. Inspect service's exact
 * receipt lifecycle separately. Terminal slots retained until explicit ACK. */
StorageResult storage_pump_poll(StoragePump *,StoragePumpTicket,StoragePumpResult *);
StorageResult storage_pump_ack(StoragePump *,StoragePumpTicket);
StorageResult storage_pump_abort(StoragePump *,StoragePumpTicket);
StorageResult storage_pump_health(StoragePump *,StoragePumpHealth *);
/* Caller quiesces producers first. Closing freezes admissions, drains existing
 * commands, then seals service; no hardware STOP or implicit repair. */
StorageResult storage_pump_close(StoragePump *);
StorageResult storage_pump_recover(StoragePump *,bool repair_tail);
/* <=one backend IO and <=one command attempt per call, IO outside mailbox lock.
 * Busy/rejected PCM retries preserve offsets; abort returns remainder facts. */
void storage_pump_step(StoragePump *,uint64_t now_us);
unsigned storage_pump_debug_stage(void);
#endif
