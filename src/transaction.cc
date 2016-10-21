/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2010 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "libforestdb/forestdb.h"
#include "fdb_engine.h"
#include "fdb_internal.h"
#include "file_handle.h"
#include "internal_types.h"
#include "filemgr.h"
#include "common.h"
#include "list.h"
#include "wal.h"
#include "disk_write_queue.h"
#include "memleak.h"

// Global static variables
static std::atomic<uint64_t> transaction_id(0); // unique & monotonically increasing

LIBFDB_API
fdb_status fdb_begin_transaction(fdb_file_handle *fhandle,
                                 fdb_isolation_level_t isolation_level)
{
    FdbEngine *fdb_engine = FdbEngine::getInstance();
    if (fdb_engine) {
        return fdb_engine->beginTransaction(fhandle, isolation_level);
    }
    return FDB_RESULT_ENGINE_NOT_INSTANTIATED;
}

LIBFDB_API
fdb_status fdb_abort_transaction(fdb_file_handle *fhandle)
{
    FdbEngine *fdb_engine = FdbEngine::getInstance();
    if (fdb_engine) {
        return fdb_engine->abortTransaction(fhandle);
    }
    return FDB_RESULT_ENGINE_NOT_INSTANTIATED;
}

FdbTransaction::FdbTransaction(fdb_isolation_level_t _isolation,
                               FdbKvsHandle *_handle,
                               uint64_t _prev_hdr_bid,
                               uint64_t _prev_revnum,
                               struct wal_txn_wrapper *_wrapper) :
                                   isolation(_isolation),
                                   handle(_handle),
                                   txn_id(transaction_id++),
                                   prev_hdr_bid(_prev_hdr_bid),
                                   prev_revnum(_prev_revnum),
                                   txn_items(nullptr),
                                   wrapper(_wrapper) {
    spin_init(&txn_lock);
    wrapper->txn = this; // set up the back pointer
}

FdbTransaction::~FdbTransaction() {
    if (txn_items) {
        delete txn_items;
    }
    spin_destroy(&txn_lock);
}

size_t FdbTransaction::getItemCount() {
    size_t ret = 0;
    spin_lock(&txn_lock);
    if (txn_items) {
        ret = txn_items->getItemCount();
    }
    spin_unlock(&txn_lock);
    return ret;
}

TxnItemList *FdbTransaction::getItemList() {
    TxnItemList *ret = nullptr;
    spin_lock(&txn_lock);
    ret = txn_items;
    spin_unlock(&txn_lock);
    return ret;
}

void FdbTransaction::resetTxnItems() {
    spin_lock(&txn_lock);
    delete txn_items;
    txn_items = nullptr;
    spin_unlock(&txn_lock);
}

void FdbTransaction::resetTxnItem(wal_item *item) {
    spin_lock(&txn_lock);
    txn_items->items.at(item->dwq_index) = nullptr;
    item->dwq_index = uint64_t(-1);
    spin_unlock(&txn_lock);
}

uint64_t FdbTransaction::addTxnItem(wal_item *item, wal_item *old_item) {
    uint64_t idx;
    spin_lock(&txn_lock);
    if (!txn_items) { // Initialize list on demand when first item is inserted.
        txn_items = new TxnItemList(this);
    }
    idx = txn_items->addItem(item, old_item);
    spin_unlock(&txn_lock);
    return idx;
}

fdb_status FdbEngine::abortTransaction(FdbFileHandle *fhandle)
{
    if (!fhandle || !fhandle->getRootHandle()) {
        return FDB_RESULT_INVALID_HANDLE;
    }

    FdbKvsHandle *handle = fhandle->getRootHandle();
    file_status_t fstatus;
    FileMgr *file;
    fdb_status status = FDB_RESULT_SUCCESS;

    if (handle->txn == NULL) {
        // there is no transaction started
        return FDB_RESULT_TRANSACTION_FAIL;
    }
    if (handle->kvs) {
        if (handle->kvs->getKvsType() == KVS_SUB) {
            // deny transaction on sub handle
            return FDB_RESULT_INVALID_HANDLE;
        }
    }

    if (!BEGIN_HANDLE_BUSY(handle)) {
        return FDB_RESULT_HANDLE_BUSY;
    }

    do { // repeat until file status is not REMOVED_PENDING
        status = fdb_check_file_reopen(handle, NULL);
        if (status != FDB_RESULT_SUCCESS) {
            END_HANDLE_BUSY(handle);
            return status;
        }

        file = handle->file;
        file->mutexLock();
        fdb_sync_db_header(handle);

        fstatus = file->getFileStatus();
        if (fstatus == FILE_REMOVED_PENDING) {
            // we must not abort transaction on this file
            // file status was changed by other thread .. start over
            file->mutexUnlock();
        }
    } while (fstatus == FILE_REMOVED_PENDING);

    file->getWal()->discardTxnEntries_Wal(handle->txn);
    file->getWal()->removeTransaction_Wal(handle->txn);

    free(handle->txn->wrapper);
    delete handle->txn;
    handle->txn = NULL;

    file->mutexUnlock();

    END_HANDLE_BUSY(handle);
    return status;
}

LIBFDB_API
fdb_status fdb_end_transaction(fdb_file_handle *fhandle,
                               fdb_commit_opt_t opt)
{
    FdbEngine *fdb_engine = FdbEngine::getInstance();
    if (fdb_engine) {
        return fdb_engine->endTransaction(fhandle, opt);
    }
    return FDB_RESULT_ENGINE_NOT_INSTANTIATED;
}

fdb_status FdbEngine::beginTransaction(FdbFileHandle *fhandle,
                                       fdb_isolation_level_t isolation_level)
{
    if (!fhandle || !fhandle->getRootHandle()) {
        return FDB_RESULT_INVALID_HANDLE;
    }

    file_status_t fstatus;
    FdbKvsHandle *handle = fhandle->getRootHandle();
    FileMgr *file;
    fdb_status status = FDB_RESULT_SUCCESS;

    if (handle->txn) {
        // transaction already exists
        return FDB_RESULT_TRANSACTION_FAIL;
    }
    if (handle->kvs) {
        if (handle->kvs->getKvsType() == KVS_SUB) {
            // deny transaction on sub handle
            return FDB_RESULT_INVALID_HANDLE;
        }
    }

    if (!BEGIN_HANDLE_BUSY(handle)) {
        return FDB_RESULT_HANDLE_BUSY;
    }

    do { // repeat until file status is not REMOVED_PENDING
        status = fdb_check_file_reopen(handle, NULL);
        if (status != FDB_RESULT_SUCCESS) {
            END_HANDLE_BUSY(handle);
            return status;
        }

        file = handle->file;
        file->mutexLock();
        fdb_sync_db_header(handle);

        if (file->isRollbackOn()) {
            // deny beginning transaction during rollback
            file->mutexUnlock();
            END_HANDLE_BUSY(handle);
            return FDB_RESULT_FAIL_BY_ROLLBACK;
        }

        fstatus = file->getFileStatus();
        if (fstatus == FILE_REMOVED_PENDING) {
            // we must not create transaction on this file
            // file status was changed by other thread .. start over
            file->mutexUnlock();
        }
    } while (fstatus == FILE_REMOVED_PENDING);

    struct wal_txn_wrapper *txn_wrapper = (struct wal_txn_wrapper *)
                                  malloc(sizeof(struct wal_txn_wrapper));
    uint64_t txn_hdr_bid;
    if (file->getFileStatus() != FILE_COMPACT_OLD) {
        // keep previous header's BID
        txn_hdr_bid = handle->last_hdr_bid;
    } else {
        // if file status is COMPACT_OLD,
        // then this transaction will work on new file, and
        // there is no previous header until the compaction is done.
        txn_hdr_bid = BLK_NOT_FOUND;
    }
    handle->txn = new FdbTransaction(isolation_level, handle, txn_hdr_bid,
                                     handle->cur_header_revnum,
                                     txn_wrapper);
    file->getWal()->addTransaction_Wal(handle->txn);

    file->mutexUnlock();

    END_HANDLE_BUSY(handle);
    return status;
}

fdb_status FdbEngine::endTransaction(FdbFileHandle *fhandle,
                                     fdb_commit_opt_t opt)
{
    if (!fhandle || !fhandle->getRootHandle()) {
        return FDB_RESULT_INVALID_HANDLE;
    }

    file_status_t fstatus;
    FdbKvsHandle *handle = fhandle->getRootHandle();
    FileMgr *file;

    if (handle->txn == NULL) {
        // there is no transaction started
        return FDB_RESULT_TRANSACTION_FAIL;
    }
    if (handle->kvs) {
        if (handle->kvs->getKvsType() == KVS_SUB) {
            // deny transaction on sub handle
            return FDB_RESULT_INVALID_HANDLE;
        }
    }

    fdb_status fs = FDB_RESULT_SUCCESS;
    if (handle->txn->getItemCount()) {
        bool sync = !(handle->config.durability_opt & FDB_DRB_ASYNC);
        fs = commitWithKVHandle(handle, opt, sync);
    }

    if (fs == FDB_RESULT_SUCCESS) {

        do { // repeat until file status is not REMOVED_PENDING
            fs = fdb_check_file_reopen(handle, NULL);
            if (fs != FDB_RESULT_SUCCESS) {
                return fs;
            }

            file = handle->file;
            file->mutexLock();
            fdb_sync_db_header(handle);

            fstatus = file->getFileStatus();
            if (fstatus == FILE_REMOVED_PENDING) {
                // we must not commit transaction on this file
                // file status was changed by other thread .. start over
                file->mutexUnlock();
            }
        } while (fstatus == FILE_REMOVED_PENDING);

        file->getWal()->removeTransaction_Wal(handle->txn);
        free(handle->txn->wrapper);
        delete handle->txn;
        handle->txn = NULL;

        file->mutexUnlock();
    }

    return fs;
}
