// Copyright 2022 SpeeDB Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "db/db_impl/db_spdb_impl_write.h"

#include "db/db_impl/db_impl.h"
#include "db/write_batch_internal.h"
#include "logging/logging.h"
#include "monitoring/instrumented_mutex.h"
#include "rocksdb/statistics.h"
#include "rocksdb/status.h"
#include "rocksdb/system_clock.h"
#include "util/mutexlock.h"

namespace ROCKSDB_NAMESPACE {

// add_buffer_mutex_ is held
void SpdbWriteImpl::WritesBatchList::Add(WriteBatch* batch,
                                         const WriteOptions& write_options,
                                         bool* leader_batch) {
  const size_t seq_inc = batch->Count();
  max_seq_ = WriteBatchInternal::Sequence(batch) + seq_inc - 1;

  if (!write_options.disableWAL) {
    wal_writes_.push_back(batch);
  }
  if (write_options.sync) {
    need_sync_ = true;
  }
  if (empty_) {
    // first wal batch . should take the buffer_write_rw_lock_ as write
    *leader_batch = true;
    buffer_write_rw_lock_.WriteLock();
    empty_ = false;
  }
  write_ref_rwlock_.ReadLock();
}

void SpdbWriteImpl::WritesBatchList::WriteBatchComplete(bool leader_batch,
                                                        DBImpl* db) {
  // Batch was added to the memtable, we can release the memtable_ref.
  write_ref_rwlock_.ReadUnlock();
  if (leader_batch) {
    {
      // make sure all batches wrote to memtable (if needed) to be able progress
      // the version
      WriteLock wl(&write_ref_rwlock_);
    }
    db->SetLastSequence(max_seq_);
    // wal write has been completed wal waiters will be released
    buffer_write_rw_lock_.WriteUnlock();
  } else {
    // wait wal write completed
    ReadLock rl(&buffer_write_rw_lock_);
  }
}

void SpdbWriteImpl::WritesBatchList::WaitForPendingWrites() {
  // make sure all batches wrote to memtable (ifneeded) to be able progress the
  // version
  WriteLock wl(&write_ref_rwlock_);
}

void SpdbWriteImpl::WriteBatchComplete(void* list, bool leader_batch) {
  if (leader_batch) {
    SwitchAndWriteBatchGroup();
  } else {
    WritesBatchList* write_batch_list = static_cast<WritesBatchList*>(list);
    write_batch_list->WriteBatchComplete(false, db_);
  }
}

/*void SpdbWriteImpl::SpdbFlushWriteThread() {
  for (;;) {
    {
      std::unique_lock<std::mutex> lck(flush_thread_mutex_);
      flush_thread_cv_.wait(lck);
      if (flush_thread_terminate_.load()) {
        break;
      }
    }
    // make sure no on the fly writes
    flush_rwlock_.WriteLock();
    db_->RegisterFlushOrTrim();
    action_needed_.store(false);
    flush_rwlock_.WriteUnlock();
  }
}*/

SpdbWriteImpl::SpdbWriteImpl(DBImpl* db) : db_(db) {}

SpdbWriteImpl::~SpdbWriteImpl() { Shutdown(); }

void SpdbWriteImpl::Shutdown() { WriteLock wl(&flush_rwlock_); }

bool DBImpl::CheckIfActionNeeded() {
  InstrumentedMutexLock l(&mutex_);

  if (!single_column_family_mode_ && total_log_size_ > GetMaxTotalWalSize()) {
    return true;
  }

  if (write_buffer_manager_->ShouldFlush()) {
    return true;
  }

  if (!flush_scheduler_.Empty()) {
    return true;
  }

  if (!trim_history_scheduler_.Empty()) {
    return true;
  }
  return false;
}

Status DBImpl::RegisterFlushOrTrim() {
  Status status;
  WriteContext write_context;
  InstrumentedMutexLock l(&mutex_);

  if (UNLIKELY(status.ok() && !single_column_family_mode_ &&
               total_log_size_ > GetMaxTotalWalSize())) {
    status = SwitchWAL(&write_context);
  }

  if (UNLIKELY(status.ok() && write_buffer_manager_->ShouldFlush())) {
    status = HandleWriteBufferManagerFlush(&write_context);
  }

  if (UNLIKELY(status.ok() && !flush_scheduler_.Empty())) {
    status = ScheduleFlushes(&write_context);
  }

  if (UNLIKELY(status.ok() && !trim_history_scheduler_.Empty())) {
    status = TrimMemtableHistory(&write_context);
  }
  return status;
}

void* SpdbWriteImpl::Add(WriteBatch* batch, const WriteOptions& write_options,
                         bool* leader_batch) {
  MutexLock l(&add_buffer_mutex_);
  WritesBatchList& pending_list = GetActiveList();
  const uint64_t sequence =
      db_->FetchAddLastAllocatedSequence(batch->Count()) + 1;
  WriteBatchInternal::SetSequence(batch, sequence);
  pending_list.Add(batch, write_options, leader_batch);
  return &pending_list;
}

void* SpdbWriteImpl::AddMerge(WriteBatch* batch,
                              const WriteOptions& write_options,
                              bool* leader_batch) {
  // thie will be released AFTER ths batch will be written to memtable!
  add_buffer_mutex_.Lock();
  const uint64_t sequence =
      db_->FetchAddLastAllocatedSequence(batch->Count()) + 1;
  WriteBatchInternal::SetSequence(batch, sequence);
  // need to wait all prev batches completed to write to memetable and avoid
  // new batches to write to memetable before this one
  for (uint32_t i = 0; i < kWalWritesContainers; i++) {
    wb_lists_[i].WaitForPendingWrites();
  }

  WritesBatchList& pending_list = GetActiveList();
  pending_list.Add(batch, write_options, leader_batch);
  return &pending_list;
}
// release the add merge lock
void SpdbWriteImpl::CompleteMerge() { add_buffer_mutex_.Unlock(); }

void SpdbWriteImpl::Lock(bool is_read) {
  if (is_read) {
    flush_rwlock_.ReadLock();
  } else {
    flush_rwlock_.WriteLock();
  }
}

void SpdbWriteImpl::Unlock(bool is_read) {
  if (is_read) {
    flush_rwlock_.ReadUnlock();
  } else {
    flush_rwlock_.WriteUnlock();
  }
}

SpdbWriteImpl::WritesBatchList* SpdbWriteImpl::SwitchBatchGroup() {
  MutexLock l(&add_buffer_mutex_);
  WritesBatchList* batch_group = &wb_lists_[active_buffer_index_];
  active_buffer_index_ = (active_buffer_index_ + 1) % wb_lists_.size();
  return batch_group;
}

void SpdbWriteImpl::SwitchAndWriteBatchGroup() {
  // take the wal write rw lock from protecting another batch group wal write
  bool action_needed = false;
  ;
  WritesBatchList* batch_group = nullptr;
  if (db_->CheckIfActionNeeded()) {
    // need to take the spdb write to write
    action_needed = true;
    Unlock(true);
    Lock(false);
    db_->RegisterFlushOrTrim();
    // take the wal write rw lock from protecting another batch group wal write
    wal_write_mutex_.Lock();
    batch_group = SwitchBatchGroup();
    Unlock(false);
    Lock(true);

  } else {
    wal_write_mutex_.Lock();
    batch_group = SwitchBatchGroup();
  }
  if (!batch_group->wal_writes_.empty()) {
    auto const& immutable_db_options = db_->immutable_db_options();
    StopWatch write_sw(immutable_db_options.clock, immutable_db_options.stats,
                       DB_WAL_WRITE_TIME);

    const WriteBatch* to_be_cached_state = nullptr;
    IOStatus io_s;
    if (batch_group->wal_writes_.size() == 1 &&
        batch_group->wal_writes_.front()
            ->GetWalTerminationPoint()
            .is_cleared()) {
      WriteBatch* wal_batch = batch_group->wal_writes_.front();

      if (WriteBatchInternal::IsLatestPersistentState(wal_batch)) {
        to_be_cached_state = wal_batch;
      }
      io_s = db_->SpdbWriteToWAL(wal_batch, 1, to_be_cached_state);
    } else {
      uint64_t progress_batch_seq;
      size_t wal_writes = 0;
      WriteBatch* merged_batch = &tmp_batch_;
      for (const WriteBatch* batch : batch_group->wal_writes_) {
        if (wal_writes != 0 &&
            (progress_batch_seq != WriteBatchInternal::Sequence(batch))) {
          // this can happened if we have a batch group that consists no wal
          // writes... need to divide the wal writes when the seq is broken
          io_s =
              db_->SpdbWriteToWAL(merged_batch, wal_writes, to_be_cached_state);
          // reset counter and state
          tmp_batch_.Clear();
          wal_writes = 0;
          to_be_cached_state = nullptr;
          if (!io_s.ok()) {
            // TBD what todo with error
            break;
          }
        }
        if (wal_writes == 0) {
          // first batch seq to use when we will replay the wal after recovery
          WriteBatchInternal::SetSequence(merged_batch,
                                          WriteBatchInternal::Sequence(batch));
        }
        // to be able knowing the batch are in seq order
        progress_batch_seq =
            WriteBatchInternal::Sequence(batch) + batch->Count();
        Status s = WriteBatchInternal::Append(merged_batch, batch, true);
        // Always returns Status::OK.()
        if (!s.ok()) {
          assert(false);
        }
        if (WriteBatchInternal::IsLatestPersistentState(batch)) {
          // We only need to cache the last of such write batch
          to_be_cached_state = batch;
        }
        ++wal_writes;
      }
      if (wal_writes) {
        io_s =
            db_->SpdbWriteToWAL(merged_batch, wal_writes, to_be_cached_state);
        tmp_batch_.Clear();
      }
    }
    if (!io_s.ok()) {
      // TBD what todo with error
      ROCKS_LOG_ERROR(db_->immutable_db_options().info_log,
                      "Error write to wal!!! %s", io_s.ToString().c_str());
    } else {
      if (batch_group->need_sync_) {
        db_->SpdbSyncWAL();
      }
    }
  }

  batch_group->WriteBatchComplete(true, db_);
  batch_group->Clear();
  wal_write_mutex_.Unlock();
}

Status DBImpl::SpdbWrite(const WriteOptions& write_options, WriteBatch* batch,
                         bool disable_memtable, bool txn_write) {
  assert(batch != nullptr && WriteBatchInternal::Count(batch) > 0);
  StopWatch write_sw(immutable_db_options_.clock, immutable_db_options_.stats,
                     DB_WRITE);
  if (txn_write) {
    ROCKS_LOG_INFO(immutable_db_options().info_log,
                   "SPDBD TXN Write!!! wal_disabled=%d disable_memtable=%d ",
                   write_options.disableWAL, disable_memtable);
  }

  if (error_handler_.IsDBStopped()) {
    return error_handler_.GetBGError();
  }

  last_batch_group_size_ = WriteBatchInternal::ByteSize(batch);
  spdb_write_->Lock(true);

  if (write_options.disableWAL) {
    has_unpersisted_data_.store(true, std::memory_order_relaxed);
  }

  Status status;
  bool leader_batch = false;
  void* list;
  if (batch->HasMerge()) {
    // need to wait all prev batches completed to write to memetable and avoid
    // new batches to write to memetable before this one
    list = spdb_write_->AddMerge(batch, write_options, &leader_batch);
  } else {
    list = spdb_write_->Add(batch, write_options, &leader_batch);
  }

  if (!disable_memtable) {
    bool concurrent_memtable_writes = !batch->HasMerge();
    status = WriteBatchInternal::InsertInto(
        batch, column_family_memtables_.get(), &flush_scheduler_,
        &trim_history_scheduler_, write_options.ignore_missing_column_families,
        0 /*recovery_log_number*/, this, concurrent_memtable_writes, nullptr,
        nullptr, seq_per_batch_, batch_per_txn_);
  }

  if (batch->HasMerge()) {
    spdb_write_->CompleteMerge();
  }

  // handle !status.ok()
  spdb_write_->WriteBatchComplete(list, leader_batch);
  spdb_write_->Unlock(true);

  return status;
}

void DBImpl::SuspendSpdbWrites() {
  if (spdb_write_) {
    spdb_write_->Lock(false);
  }
}
void DBImpl::ResumeSpdbWrites() {
  if (spdb_write_) {
    // must release the db mutex lock before unlock spdb flush lock
    // to prevent deadlock!!! the db mutex will be acquired after the unlock
    mutex_.Unlock();
    spdb_write_->Unlock(false);
    // Lock again the db mutex as it was before we enterd this function
    mutex_.Lock();
  }
}

IOStatus DBImpl::SpdbSyncWAL() {
  IOStatus io_s;
  ROCKS_LOG_INFO(immutable_db_options_.info_log, "Sync writes!");

  StopWatch sw(immutable_db_options_.clock, stats_, WAL_FILE_SYNC_MICROS);
  // Wait until the parallel syncs are finished. Any sync process has to sync
  // the front log too so it is enough to check the status of front()
  // We do a while loop since log_sync_cv_ is signalled when any sync is
  // finished
  // Note: there does not seem to be a reason to wait for parallel sync at
  // this early step but it is not important since parallel sync (SyncWAL) and
  // need_log_sync are usually not used together.
  {
    InstrumentedMutexLock l(&mutex_);
    while (logs_.front().getting_synced) {
      log_sync_cv_.Wait();
    }
    for (auto& log : logs_) {
      assert(!log.getting_synced);
      // This is just to prevent the logs to be synced by a parallel SyncWAL
      // call. We will do the actual syncing later after we will write to the
      // WAL.
      // Note: there does not seem to be a reason to set this early before we
      // actually write to the WAL
      log.getting_synced = true;
    }
  }

  {
    InstrumentedMutexLock l(&log_write_mutex_);
    /*log::Writer* log_writer = logs_.back().writer;
    io_s = log_writer->file()->Sync(immutable_db_options_.use_fsync);*/
    for (auto& log : logs_) {
      io_s = log.writer->file()->Sync(immutable_db_options_.use_fsync);
      if (!io_s.ok()) {
        break;
      }
    }
  }
  if (io_s.ok() && !log_dir_synced_) {
    io_s = directories_.GetWalDir()->FsyncWithDirOptions(
        IOOptions(), nullptr,
        DirFsyncOptions(DirFsyncOptions::FsyncReason::kNewFileSynced));
  }
  {
    InstrumentedMutexLock l(&mutex_);
    if (io_s.ok()) {
      MarkLogsSynced(logfile_number_, !log_dir_synced_);
    } else {
      MarkLogsNotSynced(logfile_number_);
    }
  }
  return io_s;
}
IOStatus DBImpl::SpdbWriteToWAL(WriteBatch* merged_batch, size_t write_with_wal,
                                const WriteBatch* to_be_cached_state) {
  assert(merged_batch != nullptr || write_with_wal == 0);
  IOStatus io_s;

  const Slice log_entry = WriteBatchInternal::Contents(merged_batch);
  const uint64_t log_entry_size = log_entry.size();

  {
    InstrumentedMutexLock l(&log_write_mutex_);
    log::Writer* log_writer = logs_.back().writer;
    io_s = log_writer->AddRecord(log_entry);
  }

  total_log_size_ += log_entry_size;
  // TODO(myabandeh): it might be unsafe to access alive_log_files_.back()
  // here since alive_log_files_ might be modified concurrently
  alive_log_files_.back().AddSize(log_entry_size);
  log_empty_ = false;

  if (to_be_cached_state != nullptr) {
    cached_recoverable_state_ = *to_be_cached_state;
    cached_recoverable_state_empty_ = false;
  }

  if (io_s.ok()) {
    InternalStats* stats = default_cf_internal_stats_;

    stats->AddDBStats(InternalStats::kIntStatsWalFileBytes, log_entry_size);
    RecordTick(stats_, WAL_FILE_BYTES, log_entry_size);
    stats->AddDBStats(InternalStats::kIntStatsWriteWithWal, write_with_wal);
    RecordTick(stats_, WRITE_WITH_WAL, write_with_wal);
  }

  return io_s;
}

}  // namespace ROCKSDB_NAMESPACE
