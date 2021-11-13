/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/concurrency/lock_state.h"

#include <vector>

#include "mongo/db/namespace_string.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/compiler.h"
#include "mongo/stdx/new.h"
#include "mongo/util/background.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

/**
 * Partitioned global lock statistics, so we don't hit the same bucket.
 */ 

//ע��db.serverStatus().globalLock   db.serverStatus().locks   db.runCommand({lockInfo: 1}) ����������

/*  LockStats<>::_report �л�ȡ�����Ϣ
featdoc:PRIMARY> 
featdoc:PRIMARY> db.serverStatus().locks
{
        "Global" : {
                "acquireCount" : {
                        "r" : NumberLong(1447),
                        "w" : NumberLong(40),
                        "W" : NumberLong(9)
                },
                "acquireWaitCount" : {
                        "w" : NumberLong(1),
                        "W" : NumberLong(2)
                },
                "timeAcquiringMicros" : {
                        "w" : NumberLong(8569),
                        "W" : NumberLong(268)
                }
        },
        "Database" : {
                "acquireCount" : {
                        "r" : NumberLong(689),
                        "w" : NumberLong(18),
                        "R" : NumberLong(7),
                        "W" : NumberLong(16)
                }
        },
        "Collection" : {
                "acquireCount" : {
                        "r" : NumberLong(358),
                        "w" : NumberLong(8)
                }
        },
        "oplog" : {
                "acquireCount" : {
                        "r" : NumberLong(331),
                        "w" : NumberLong(12)
                }
        }
}
featdoc:PRIMARY> db.serverStatus().globalLock
{
        "totalTime" : NumberLong(170653000),
        "currentQueue" : {
                "total" : 0,
                "readers" : 0,
                "writers" : 0
        },
        "activeClients" : {
                "total" : 29,
                "readers" : 0,
                "writers" : 0
        }
}
featdoc:PRIMARY> 
featdoc:PRIMARY> 
*/

//ע��db.serverStatus().globalLock   db.serverStatus().locks   db.runCommand({lockInfo: 1}) ����������

//���������Ӧ�̵߳���ͳ����LockerImpl._stats�д洢��ȫ����ͳ����ȫ�ֱ���globalStats�д洢

//PartitionedInstanceWideLockStats globalStats;    ȫ����ͳ��
//LockStats<>::_report(db.serverStatus().locks�鿴)�л�ȡ�����Ϣ,���������ܵ�����ص�ͳ��
class PartitionedInstanceWideLockStats {
    MONGO_DISALLOW_COPYING(PartitionedInstanceWideLockStats);

public:
    PartitionedInstanceWideLockStats() {}

	//LockerImpl<>::lockBegin->PartitionedInstanceWideLockStats::recordAcquisition->LockStats::recordAcquisition
	//LockerImpl<IsForMMAPV1>::lockBegin��ִ��
    void recordAcquisition(LockerId id, ResourceId resId, LockMode mode) {
        _get(id).recordAcquisition(resId, mode);
    }

	
	//LockerImpl<>::lockBegin->PartitionedInstanceWideLockStats::recordWait->LockStats::recordWait
	//LockerImpl<>::lockBegin��ִ��
    void recordWait(LockerId id, ResourceId resId, LockMode mode) {
        _get(id).recordWait(resId, mode);
    }

	//LockerImpl<>::lockComplete->PartitionedInstanceWideLockStats::recordWaitTime->LockStats::recordWaitTime
    void recordWaitTime(LockerId id, ResourceId resId, LockMode mode, uint64_t waitMicros) {
        _get(id).recordWaitTime(resId, mode, waitMicros);
    }

	//LockerImpl<>::lockComplete->PartitionedInstanceWideLockStats::recordDeadlock->LockStats::recordDeadlock
    void recordDeadlock(ResourceId resId, LockMode mode) {
        _get(resId).recordDeadlock(resId, mode);
    }

	//ȫ�ּ�ش�ӡdb.serverstats().locks  reportGlobalLockingStats->PartitionedInstanceWideLockStats::report
	//�û������̣߳�����дһ�����ݶ�Ӧ�̣߳����¼����־��OpDebug::report�д�ӡ������־��
	void report(SingleThreadedLockStats* outStats) const {
        for (int i = 0; i < NumPartitions; i++) {
			//LockStats::append
            outStats->append(_partitions[i].stats);
        }
    }

    void reset() {
        for (int i = 0; i < NumPartitions; i++) {
            _partitions[i].stats.reset();
        }
    }

private:
    // This alignment is a best effort approach to ensure that each partition falls on a
    // separate page/cache line in order to avoid false sharing.
    //c++ alignas�ı�һ���������͵Ķ�������
    struct alignas(stdx::hardware_destructive_interference_size) AlignedLockStats {
        AtomicLockStats stats;
    };

	//AlignedLockStats _partitions[NumPartitions];
    enum { NumPartitions = 8 };

	//ǰ���recordAcquisition�Ƚӿڻ��õ�����
    AtomicLockStats& _get(LockerId id) {
        return _partitions[id % NumPartitions].stats;
    }

	//Ҳ����AtomicLockStats�ṹ 
    AlignedLockStats _partitions[NumPartitions];
};

// Global lock manager instance.
LockManager globalLockManager;

// Global lock. Every server operation, which uses the Locker must acquire this lock at least
// once. See comments in the header file (begin/endTransaction) for more information.
const ResourceId resourceIdGlobal = ResourceId(RESOURCE_GLOBAL, ResourceId::SINGLETON_GLOBAL);

// Flush lock. This is only used for the MMAP V1 storage engine and synchronizes journal writes
// to the shared view and remaps. See the comments in the header for information on how MMAP V1
// concurrency control works.
const ResourceId resourceIdMMAPV1Flush =
    ResourceId(RESOURCE_MMAPV1_FLUSH, ResourceId::SINGLETON_MMAPV1_FLUSH);

// How often (in millis) to check for deadlock if a lock has not been granted for some time
//������ô��ʱ�䶼û�л�ȡ���������ж�Ϊ����
const Milliseconds DeadlockTimeout = Milliseconds(500);

// Dispenses unique LockerId identifiers
AtomicUInt64 idCounter(0);

// Partitioned global lock statistics, so we don't hit the same bucket

//���������Ӧ�̵߳���ͳ����LockerImpl._stats�д洢��ȫ����ͳ����ȫ�ֱ���globalStats�д洢
PartitionedInstanceWideLockStats globalStats; //ȫ��lockͳ��


/**
 * Whether the particular lock's release should be held until the end of the operation. We
 * delay release of exclusive locks (locks that are for write operations) in order to ensure
 * that the data they protect is committed successfully.
 */
//LockerImpl<>::unlock�е���
bool shouldDelayUnlock(ResourceId resId, LockMode mode) {
    // Global and flush lock are not used to protect transactional resources and as such, they
    // need to be acquired and released when requested.
    //ȫ������ˢ���������ڱ���������Դ�������Ҫ������ʱ��ȡ���ͷ���Щ��Դ��
    switch (resId.getType()) {
        case RESOURCE_GLOBAL:
        case RESOURCE_MMAPV1_FLUSH:
        case RESOURCE_MUTEX:
            return false;

        case RESOURCE_COLLECTION:
        case RESOURCE_DATABASE:
        case RESOURCE_METADATA:
            break;

        default:
            MONGO_UNREACHABLE;
    }

    switch (mode) {
		//д����Ӧ���ͷţ���������ȷ��д������ȷ�ύ
        case MODE_X:
        case MODE_IX:
            return true;

        case MODE_IS:
        case MODE_S:
            return false;

        default:
            MONGO_UNREACHABLE;
    }
}

}  // namespace

//д��   //LockerImpl�ṹ��OperationContext._locker��Ա, wiredtiger��ӦDefaultLockerImpl
template <bool IsForMMAPV1>
//��ǰresourceIdGlobal�ӵ���MODE_X���͵���
bool LockerImpl<IsForMMAPV1>::isW() const {
    return getLockMode(resourceIdGlobal) == MODE_X;
}

//����
template <bool IsForMMAPV1>
//��ǰresourceIdGlobal�ӵ���MODE_S���͵���
bool LockerImpl<IsForMMAPV1>::isR() const {
    return getLockMode(resourceIdGlobal) == MODE_S;
}

template <bool IsForMMAPV1>
bool LockerImpl<IsForMMAPV1>::isLocked() const {
    return getLockMode(resourceIdGlobal) != MODE_NONE;
}

template <bool IsForMMAPV1>
//��ǰresourceIdGlobal�ӵ���MODE_IX���͵���  д������
bool LockerImpl<IsForMMAPV1>::isWriteLocked() const {
    return isLockHeldForMode(resourceIdGlobal, MODE_IX);
}

template <bool IsForMMAPV1>
bool LockerImpl<IsForMMAPV1>::isReadLocked() const {
//��ǰresourceIdGlobal�ӵ���MODE_IS���͵���   ��������
    return isLockHeldForMode(resourceIdGlobal, MODE_IS);
}

template <bool IsForMMAPV1>
void LockerImpl<IsForMMAPV1>::dump() const {
    StringBuilder ss;
    ss << "Locker id " << _id << " status: ";

    _lock.lock();
    LockRequestsMap::ConstIterator it = _requests.begin();
    while (!it.finished()) {
        ss << it.key().toString() << " " << lockRequestStatusName(it->status) << " in "
           << modeName(it->mode) << "; ";
        it.next();
    }
    _lock.unlock();

    log() << ss.str();
}


//
// CondVarLockGrantNotification
//

CondVarLockGrantNotification::CondVarLockGrantNotification() {
    clear();
}

void CondVarLockGrantNotification::clear() {
    _result = LOCK_INVALID;
}

//���������ȴ����ѻ��߳�ʱ�Զ����ѷ���     LockerImpl<>::lockComplete
LockResult CondVarLockGrantNotification::wait(Milliseconds timeout) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    return _cond.wait_for(
               lock, timeout.toSystemDuration(), [this] { return _result != LOCK_INVALID; })
        ? _result
        : LOCK_TIMEOUT;
}

//LockManager::_onLockModeChanged->CondVarLockGrantNotification::notify���ѵȴ��߳�
void CondVarLockGrantNotification::notify(ResourceId resId, LockResult result) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    invariant(_result == LOCK_INVALID);
    _result = result;

	//�����������������еĵȴ�(wait)�̡߳�
    _cond.notify_all();
}


//ServiceContextMongoD::_newOpCtx�й���LockerImplʹ�ã�ÿ�������Ӧһ��DefaultLockerImpl(Ҳ����һ��LockerImpl)
//ע��ȫ������������ʵ����ȫ�ֱ���ticketHolders[]����ά������ÿ�������locker����ǰ��Ҫ�ж�ȫ�����Ƿ���Ҫ�ȴ�



//ע��db.serverStatus().globalLock   db.serverStatus().locks   db.runCommand({lockInfo: 1}) ����������


//ÿһ��mode��Ӧһ��TicketHolder������linux��sem�ź���ʵ��
//ͨ��db.serverStatus().globalLock��ȡ
namespace { //��ֵ��setGlobalThrottling //WiredTigerKVEngine::WiredTigerKVEngine->Locker::setGlobalThrottling
TicketHolder* ticketHolders[LockModesCount] = {}; 
}  // namespace


//
// Locker
//
/**
 * Lock modes.
 *
 * Compatibility Matrix  �����Թ�ϵ +���ݹ���        +�Ǽ��ݵ�   
 *                                          Granted mode
 *   ---------------.--------------------------------------------------------.
 *   Requested Mode | MODE_NONE  MODE_IS   MODE_IX  MODE_S   MODE_X  |
 *     MODE_IS      |      +        +         +        +        -    |
 *     MODE_IX      |      +        +         +        -        -    |
 *     MODE_S       |      +        +         -        +        -    |
 *     MODE_X       |      +        -         -        -        -    |  ����MODE_X���󣬶�д��������
 * �ٷ��ĵ�https://docs.mongodb.com/manual/faq/concurrency/
 */

//TicketHolder openWriteTransaction(128); 
//TicketHolder openReadTransaction(128);

//WiredTigerKVEngine::WiredTigerKVEngine->Locker::setGlobalThrottling
/* static */  //��д��Ĭ����128���ź���ʵ�֣���ȡ���ź���-1���ͷ����ź�����1
void Locker::setGlobalThrottling(class TicketHolder* reading, class TicketHolder* writing) {
    ticketHolders[MODE_S] = reading;
    ticketHolders[MODE_IS] = reading;
    ticketHolders[MODE_IX] = writing;

	//ticketHolders[MODE_X]Ϊʲôû��ֵ�أ������︳ֵ��   ��_lockGlobalBegin����Ķ�
}

template <bool IsForMMAPV1>
LockerImpl<IsForMMAPV1>::LockerImpl()
    : _id(idCounter.addAndFetch(1)), _wuowNestingLevel(0), _threadId(stdx::this_thread::get_id()) {}

template <bool IsForMMAPV1>
stdx::thread::id LockerImpl<IsForMMAPV1>::getThreadId() const {
    return _threadId;
}

template <bool IsForMMAPV1>
LockerImpl<IsForMMAPV1>::~LockerImpl() {
    // Cannot delete the Locker while there are still outstanding requests, because the
    // LockManager may attempt to access deleted memory. Besides it is probably incorrect
    // to delete with unaccounted locks anyways.
    invariant(!inAWriteUnitOfWork());
    invariant(_resourcesToUnlockAtEndOfUnitOfWork.empty());
    invariant(_requests.empty());
    invariant(_modeForTicket == MODE_NONE);

    // Reset the locking statistics so the object can be reused
    _stats.reset();
}

//��ȡ Client ״̬ʱ���Ѿ���ȡ��wiredtiger ticket �� Reader/Writer ����ڵ�����Ҳ����Ϊ�� Queued ״̬�����֮ǰ�����ˡ�
//http://www.mongoing.com/archives/4768
//db.serverStatus().globalLock��ȡ��
//ע��db.serverStatus().globalLock   db.serverStatus().locks   db.runCommand({lockInfo: 1}) ����������

////GlobalLockServerStatusSection::generateSection�е���
template <bool IsForMMAPV1>
Locker::ClientState LockerImpl<IsForMMAPV1>::getClientState() const {
    auto state = _clientState.load();
    if (state == kActiveReader && hasLockPending())
        state = kQueuedReader;
    if (state == kActiveWriter && hasLockPending())
        state = kQueuedWriter;

    return state;
}

//LockerImpl<>::restoreLockState
//ȫ������ȡʵ���ϲ����ߵ��������Lock::GlobalLock::_enqueue->lock_state.h�е�lockGlobalBegin->LockerImpl<>::_lockGlobalBegin


//ȫ�����������̣�Lock::GlobalLock::GlobalLock->Lock::GlobalLock::_enqueue->LockerImpl::lockGlobal
template <bool IsForMMAPV1>
LockResult LockerImpl<IsForMMAPV1>::lockGlobal(LockMode mode) {
    LockResult result = _lockGlobalBegin(mode, Milliseconds::max());

    if (result == LOCK_WAITING) { //һֱ�ȴ�����ȡ�������߳�ʱ�ŷ���
        result = lockGlobalComplete(Milliseconds::max());
    }

    if (result == LOCK_OK) {
        lockMMAPV1Flush();
    }

    return result;
}
/*
db/concurrency/lock_state.cpp:const ResourceId resourceIdGlobal = ResourceId(RESOURCE_GLOBAL, ResourceId::SINGLETON_GLOBAL);
db/concurrency/lock_state.cpp:const ResourceId resourceIdLocalDB = ResourceId(RESOURCE_DATABASE, StringData("local"));
db/concurrency/lock_state.cpp:const ResourceId resourceIdOplog = ResourceId(RESOURCE_COLLECTION, StringData("local.oplog.rs"));
db/concurrency/lock_state.cpp:const ResourceId resourceIdAdminDB = ResourceId(RESOURCE_DATABASE, StringData("admin"));
*/

//LockerImpl<IsForMMAPV1>::_lockGlobalBegin��Lock::GlobalLock::_unlock��Ӧ��һ��������һ������

//serverStatus.globalLock ���� mongostat ��qr|qw ar|awָ�꣩�ܲ鿴mongod globalLock�ĸ���ָ�������

//ȫ������������LockerImpl<>::_lockGlobalBegin   
//RESOURCE_DATABASE RESOURCE_COLLECTION��Ӧ���������̼�LockResult LockerImpl<>::lock

//ȫ�����������̣�Lock::GlobalLock::GlobalLock->Lock::GlobalLock::_enqueue->LockerImpl::lockGlobal

//LockerImpl<>::lockGlobal  Lock::GlobalLock::_enqueue�е���  
template <bool IsForMMAPV1>  //�ȴ���ȡȫ�����ɹ���Lock::GlobalLock::GlobalLock->Lock::GlobalLock::waitForLock->LockerImpl<>::lockGlobalComplete
LockResult LockerImpl<IsForMMAPV1>::_lockGlobalBegin(LockMode mode, Milliseconds timeout) {
    dassert(isLocked() == (_modeForTicket != MODE_NONE));

	//ȫ�������в����������������ʵ���������
    if (_modeForTicket == MODE_NONE) {
		//�ж��Ƿ�������߶�������
        const bool reader = isSharedLockMode(mode);
		//��mode��Ӧ��ticketHolders������ʵ���������
        auto holder = ticketHolders[mode]; 
		//��ѭ���е���
        if (holder) { //���modeΪMODE_X�� ����ticketHolders[MODE_X]ΪNULL����setGlobalThrottling
		//����������S  IS  IX������ÿ��������Ҫ��ȫ��128�ź����������ƣ�Ҳ�������ֻ��128���߳�ͬʱ����
		//�⼸�����͵���
		
            _clientState.store(reader ? kQueuedReader : kQueuedWriter); 
			//�ȴ����ڼ�ΪQueued״̬����ȡ�������ΪActive״̬����ȡ��ʱ��Ϊinactive
            if (timeout == Milliseconds::max()) {
				//TicketHolder::waitForTicketһֱ�����ź���������
                holder->waitForTicket();  


			//���ȴ���ʱ�䣬�������ʱ��ֱ�ӽ���inactive
            } else if (!holder->waitForTicketUntil(Date_t::now() + timeout)) {
            	//û��ȡ������Ҳ�����ź��������ˣ�״̬��Ϊinactive
                _clientState.store(kInactive); 
				//��ȡ����ʱ
                return LOCK_TIMEOUT;
            }
        }

		//��ȡ������״̬��Ϊactive, ͨ��db.serverStatus().globalLock��ȡ
		//ע��db.serverStatus().globalLock   db.serverStatus().locks   db.runCommand({lockInfo: 1}) ����������
        _clientState.store(reader ? kActiveReader : kActiveWriter);
        _modeForTicket = mode;
    }

	//���������������
	//ע������ȫ���õ�ͬһ����ԴresourceIdGlobal
    const LockResult result = lockBegin(resourceIdGlobal, mode);
    if (result == LOCK_OK)
        return LOCK_OK;

    // Currently, deadlock detection does not happen inline with lock acquisition so the only
    // unsuccessful result that the lock manager would return is LOCK_WAITING.
    invariant(result == LOCK_WAITING);

    return result;
}

//LockerImpl<>::lockGlobal    Lock::GlobalLock::GlobalLock->Lock::GlobalLock::waitForLock
template <bool IsForMMAPV1>
LockResult LockerImpl<IsForMMAPV1>::lockGlobalComplete(Milliseconds timeout) {
    return lockComplete(resourceIdGlobal, getLockMode(resourceIdGlobal), timeout, false);
}

template <bool IsForMMAPV1>
void LockerImpl<IsForMMAPV1>::lockMMAPV1Flush() {
    if (!IsForMMAPV1)
        return;

    // The flush lock always has a reference count of 1, because it is dropped at the end of
    // each write unit of work in order to allow the flush thread to run. See the comments in
    // the header for information on how the MMAP V1 journaling system works.
    LockRequest* globalLockRequest = _requests.find(resourceIdGlobal).objAddr();
    if (globalLockRequest->recursiveCount == 1) {
        invariant(LOCK_OK == lock(resourceIdMMAPV1Flush, _getModeForMMAPV1FlushLock()));
    }

    dassert(getLockMode(resourceIdMMAPV1Flush) == _getModeForMMAPV1FlushLock());
}

template <bool IsForMMAPV1>
void LockerImpl<IsForMMAPV1>::downgradeGlobalXtoSForMMAPV1() {
    invariant(!inAWriteUnitOfWork());

    LockRequest* globalLockRequest = _requests.find(resourceIdGlobal).objAddr();
    invariant(globalLockRequest->mode == MODE_X);
    invariant(globalLockRequest->recursiveCount == 1);
    invariant(_modeForTicket == MODE_X);
    // Note that this locker will not actually have a ticket (as MODE_X has no TicketHolder) or
    // acquire one now, but at most a single thread can be in this downgraded MODE_S situation,
    // so it's OK.

    // Making this call here will record lock downgrades as acquisitions, which is acceptable
    globalStats.recordAcquisition(_id, resourceIdGlobal, MODE_S);
    _stats.recordAcquisition(resourceIdGlobal, MODE_S);

    globalLockManager.downgrade(globalLockRequest, MODE_S);

    if (IsForMMAPV1) {
        invariant(unlock(resourceIdMMAPV1Flush));
    }
}

//LockerImpl<IsForMMAPV1>::_lockGlobalBegin��Lock::GlobalLock::_unlock��Ӧ��һ��������һ������
//Lock::GlobalLock::_unlock
template <bool IsForMMAPV1>
bool LockerImpl<IsForMMAPV1>::unlockGlobal() {
    if (!unlock(resourceIdGlobal)) {
        return false;
    }

    invariant(!inAWriteUnitOfWork());

    LockRequestsMap::Iterator it = _requests.begin();
    while (!it.finished()) {
        // If we're here we should only have one reference to any lock. It is a programming
        // error for any lock used with multi-granularity locking to have more references than
        // the global lock, because every scope starts by calling lockGlobal.
        if (it.key().getType() == RESOURCE_GLOBAL || it.key().getType() == RESOURCE_MUTEX) {
            it.next();
        } else {
            invariant(_unlockImpl(&it));
        }
    }

    return true;
}

//��ϲο�insertDocuments  makeCollection    �����װ
//WriteUnitOfWork�е���
template <bool IsForMMAPV1>
void LockerImpl<IsForMMAPV1>::beginWriteUnitOfWork() {
    // Sanity check that write transactions under MMAP V1 have acquired the flush lock, so we
    // don't allow partial changes to be written.
    dassert(!IsForMMAPV1 || isLockHeldForMode(resourceIdMMAPV1Flush, MODE_IX));

    _wuowNestingLevel++;
}

//��ϲο�insertDocuments  makeCollection    �����װ
//~WriteUnitOfWork()  WriteUnitOfWork::commit����
template <bool IsForMMAPV1>
void LockerImpl<IsForMMAPV1>::endWriteUnitOfWork() {
    invariant(_wuowNestingLevel > 0);

    if (--_wuowNestingLevel > 0) {
        // Don't do anything unless leaving outermost WUOW.
        return;
    }

	//������ӳ��ͷŽӿ��Ķ�LockerImpl<>::unlock
    while (!_resourcesToUnlockAtEndOfUnitOfWork.empty()) {
        unlock(_resourcesToUnlockAtEndOfUnitOfWork.front());
        _resourcesToUnlockAtEndOfUnitOfWork.pop();
    }

    // For MMAP V1, we need to yield the flush lock so that the flush thread can run
    if (IsForMMAPV1) {
        invariant(unlock(resourceIdMMAPV1Flush));
        invariant(LOCK_OK == lock(resourceIdMMAPV1Flush, _getModeForMMAPV1FlushLock()));
    }
}

//ȫ������������LockerImpl<IsForMMAPV1>::_lockGlobalBegin   
//RESOURCE_DATABASE RESOURCE_COLLECTION��Ӧ���������̼�LockResult LockerImpl<>::lock

//LockerImpl<>::lock��LockerImpl<>::unlock��Ӧ

//Lock::ResourceLock::lock
//Lock::DBLock::DBLock  Lock::DBLock::relockWithMode
//Lock::CollectionLock::CollectionLock
//Lock::OplogIntentWriteLock::OplogIntentWriteLock

//LockerImpl<>::lockComplete
//LockerImpl<>::restoreLockState�п�������ݹ����
template <bool IsForMMAPV1>
LockResult LockerImpl<IsForMMAPV1>::lock(ResourceId resId,
                                         LockMode mode,
                                         //Locker����Ĭ�ϸ�ֵΪMilliseconds::max()������ʱ
                                         Milliseconds timeout,
                                         bool checkDeadlock) {
    const LockResult result = lockBegin(resId, mode);

    // Fast, uncontended path
    if (result == LOCK_OK)
        return LOCK_OK;

    // Currently, deadlock detection does not happen inline with lock acquisition so the only
    // unsuccessful result that the lock manager would return is LOCK_WAITING.
    invariant(result == LOCK_WAITING);

	//һֱ�ȴ���ȡ�������߳�ʱ�ŷ���
    return lockComplete(resId, mode, timeout, checkDeadlock);
}

template <bool IsForMMAPV1>
void LockerImpl<IsForMMAPV1>::downgrade(ResourceId resId, LockMode newMode) {
    LockRequestsMap::Iterator it = _requests.find(resId);
    globalLockManager.downgrade(it.objAddr(), newMode);
}

//LockerImpl<>::lock��LockerImpl<>::unlock��Ӧ
template <bool IsForMMAPV1>
//ע��LockerImpl<>::unlock �� LockerImpl<>::_unlockImpl������
//	LockerImpl<>::unlock���_unlockImpl���������ͷ�ǰ���������жϣ��ж��ͷ���Ҫ�ӳٵȵ�����
//	�ύ���ͷţ�ȷ�������ύ��ɺ��ͷ������Ӷ���һ����������
bool LockerImpl<IsForMMAPV1>::unlock(ResourceId resId) {
    LockRequestsMap::Iterator it = _requests.find(resId);
	//Lock_state.h��_wuowNestingLevel>0˵�����������У���ʱ����Ҫ��һ��ȷ���Ƿ���Ҫ�ӳٽ���
	//�����ǰ�������������в�����Դ����Ϊ���������������Ҷ�Ӧ��������ΪX����IX��
	//����insert���� insertDocuments������д��������Դ��Ϣ��unlock��Ҫ�ӳٵ�LockerImpl<>::endWriteUnitOfWork()
	// �����ύ���������Դunlock�ͷ�
	if (inAWriteUnitOfWork() && shouldDelayUnlock(it.key(), (it->mode))) {
		//��Ҫ�ӳ�unlock������ӵ�_resourcesToUnlockAtEndOfUnitOfWork����
        _resourcesToUnlockAtEndOfUnitOfWork.push(it.key());
        return false;
    }

	
    return _unlockImpl(&it);
}

template <bool IsForMMAPV1>
LockMode LockerImpl<IsForMMAPV1>::getLockMode(ResourceId resId) const {
    scoped_spinlock scopedLock(_lock);

    const LockRequestsMap::ConstIterator it = _requests.find(resId);
    if (!it)
        return MODE_NONE;

    return it->mode;
}

template <bool IsForMMAPV1>

//Ҳ����LockConflictsTable��getLockMode(resId)�Ƿ����mode
//ǰ��resId.mode�Ƿ��������mode
bool LockerImpl<IsForMMAPV1>::isLockHeldForMode(ResourceId resId, LockMode mode) const {
    return isModeCovered(mode, getLockMode(resId));
}

template <bool IsForMMAPV1>
bool LockerImpl<IsForMMAPV1>::isDbLockedForMode(StringData dbName, LockMode mode) const {
    invariant(nsIsDbOnly(dbName));

    if (isW())
        return true;
    if (isR() && isSharedLockMode(mode))
        return true;

    const ResourceId resIdDb(RESOURCE_DATABASE, dbName);
    return isLockHeldForMode(resIdDb, mode);
}

template <bool IsForMMAPV1>
bool LockerImpl<IsForMMAPV1>::isCollectionLockedForMode(StringData ns, LockMode mode) const {
    invariant(nsIsFull(ns));

    if (isW())
        return true;
    if (isR() && isSharedLockMode(mode))
        return true;

    const NamespaceString nss(ns);
    const ResourceId resIdDb(RESOURCE_DATABASE, nss.db());

    LockMode dbMode = getLockMode(resIdDb);
    if (!shouldConflictWithSecondaryBatchApplication())
        return true;

    switch (dbMode) {
        case MODE_NONE:
            return false;
        case MODE_X:
            return true;
        case MODE_S:
            return isSharedLockMode(mode);
        case MODE_IX:
        case MODE_IS: {
            const ResourceId resIdColl(RESOURCE_COLLECTION, ns);
            return isLockHeldForMode(resIdColl, mode);
        } break;
        case LockModesCount:
            break;
    }

    invariant(false);
    return false;
}

template <bool IsForMMAPV1>
//����־��ӡ��ʱ�����LockerImpl<>::getLockerInfo��Ȼ����øýӿ�
ResourceId LockerImpl<IsForMMAPV1>::getWaitingResource() const {
    scoped_spinlock scopedLock(_lock);

    LockRequestsMap::ConstIterator it = _requests.begin();
    while (!it.finished()) {
        if (it->status == LockRequest::STATUS_WAITING ||
            it->status == LockRequest::STATUS_CONVERTING) {
            return it.key();
        }

        it.next();
    }

    return ResourceId();
}

//����־��¼�ο�ServiceEntryPointMongod::handleRequest   OpDebug::report��ʹ�ø�lockerInfo��ӡ����־
template <bool IsForMMAPV1>
void LockerImpl<IsForMMAPV1>::getLockerInfo(LockerInfo* lockerInfo) const {
    invariant(lockerInfo);

	//����ֵ
    // Zero-out the contents
    lockerInfo->locks.clear();
    lockerInfo->waitingResource = ResourceId();
	//LockStatCounters::reset
    lockerInfo->stats.reset();

	//��ȡ��ǰ��ʵʱͳ����Ϣ
    _lock.lock();
    LockRequestsMap::ConstIterator it = _requests.begin();
    while (!it.finished()) {
        OneLock info;
        info.resourceId = it.key();
        info.mode = it->mode;

        lockerInfo->locks.push_back(info);
        it.next();
    }
    _lock.unlock();

    std::sort(lockerInfo->locks.begin(), lockerInfo->locks.end());

	// OpDebug::report��ʹ�ø�lockerInfo��ӡ����־
    lockerInfo->waitingResource = getWaitingResource();
	//LockStats::append
    lockerInfo->stats.append(_stats);
}

template <bool IsForMMAPV1>
//QueryYield::yieldAllLocks  Lock::TempRelease::TempRelease����
bool LockerImpl<IsForMMAPV1>::saveLockStateAndUnlock(Locker::LockSnapshot* stateOut) {
    // We shouldn't be saving and restoring lock state from inside a WriteUnitOfWork.
    invariant(!inAWriteUnitOfWork());

    // Clear out whatever is in stateOut.
    stateOut->locks.clear();
    stateOut->globalMode = MODE_NONE;

    // First, we look at the global lock.  There is special handling for this (as the flush
    // lock goes along with it) so we store it separately from the more pedestrian locks.
    LockRequestsMap::Iterator globalRequest = _requests.find(resourceIdGlobal);
    if (!globalRequest) {
        // If there's no global lock there isn't really anything to do. Check that.
        for (auto it = _requests.begin(); !it.finished(); it.next()) {
            invariant(it.key().getType() == RESOURCE_MUTEX);
        }
        return false;
    }

    // If the global lock has been acquired more than once, we're probably somewhere in a
    // DBDirectClient call.  It's not safe to release and reacquire locks -- the context using
    // the DBDirectClient is probably not prepared for lock release.
    if (globalRequest->recursiveCount > 1) {
        return false;
    }

    // The global lock must have been acquired just once
    stateOut->globalMode = globalRequest->mode;
    invariant(unlock(resourceIdGlobal));

    // Next, the non-global locks.
    for (LockRequestsMap::Iterator it = _requests.begin(); !it.finished(); it.next()) {
        const ResourceId resId = it.key();
        const ResourceType resType = resId.getType();
        if (resType == RESOURCE_MUTEX)
            continue;

        // We should never have to save and restore metadata locks.
        invariant((IsForMMAPV1 && (resourceIdMMAPV1Flush == resId)) ||
                  RESOURCE_DATABASE == resId.getType() || RESOURCE_COLLECTION == resId.getType() ||
                  (RESOURCE_GLOBAL == resId.getType() && isSharedLockMode(it->mode)));

        // And, stuff the info into the out parameter.
        OneLock info;
        info.resourceId = resId;
        info.mode = it->mode;

        stateOut->locks.push_back(info);

        invariant(unlock(resId));
    }
    invariant(!isLocked());

    // Sort locks by ResourceId. They'll later be acquired in this canonical locking order.
    std::sort(stateOut->locks.begin(), stateOut->locks.end());

    return true;
}

//Lock::TempRelease::~TempRelease    WiredTigerRecordStore::yieldAndAwaitOplogDeletionRequest
//QueryYield::yieldAllLocks   handleBatchHelper 
template <bool IsForMMAPV1>
void LockerImpl<IsForMMAPV1>::restoreLockState(const Locker::LockSnapshot& state) {
    // We shouldn't be saving and restoring lock state from inside a WriteUnitOfWork.
    invariant(!inAWriteUnitOfWork());
    invariant(_modeForTicket == MODE_NONE);

    std::vector<OneLock>::const_iterator it = state.locks.begin();
    // If we locked the PBWM, it must be locked before the resourceIdGlobal resource.
    if (it != state.locks.end() && it->resourceId == resourceIdParallelBatchWriterMode) {
        invariant(LOCK_OK == lock(it->resourceId, it->mode));
        it++;
    }

    invariant(LOCK_OK == lockGlobal(state.globalMode));
    for (; it != state.locks.end(); it++) {
        // This is a sanity check that lockGlobal restored the MMAP V1 flush lock in the
        // expected mode.
        if (IsForMMAPV1 && (it->resourceId == resourceIdMMAPV1Flush)) {
            invariant(it->mode == _getModeForMMAPV1FlushLock());
        } else {
            invariant(LOCK_OK == lock(it->resourceId, it->mode));
        }
    }
    invariant(_modeForTicket != MODE_NONE);
}

//ȫ������������LockerImpl<>::_lockGlobalBegin   
//RESOURCE_DATABASE RESOURCE_COLLECTION��Ӧ���������̼�LockResult LockerImpl<>::lock

//ȫ����_lockGlobalBegin     ���� ���� LockResult LockerImpl<>::lock ����ִ�иú���
template <bool IsForMMAPV1>  ////wiredtiger�洢����LockerImpl��ӦDefaultLockerImpl
LockResult LockerImpl<IsForMMAPV1>::lockBegin(ResourceId resId, LockMode mode) {
    dassert(!getWaitingResource().isValid());

    LockRequest* request;
    bool isNew = true; //˵��request�����¹���ģ�false˵����֮ǰ_requests�Ѿ����ڵ�

	//log() << "ddd test lockBegin, lockinfo: " << resId.toString();
	
//#include "mongo/util/stacktrace.h"
	//mallocFreeOStream << "lock test :" << ".\n";
    //printStackTrace(mallocFreeOStream);


    LockRequestsMap::Iterator it = _requests.find(resId); 
    if (!it) { //���resId����_requests map���У�������һ��LockRequest��ӽ�ȥ
        scoped_spinlock scopedLock(_lock);
		//ÿ��resId����Դ��Ӧ����һ��LockRequest����ӵ�_requests��
        LockRequestsMap::Iterator itNew = _requests.insert(resId);
		
		//��ʼ��һ��struct LockRequest�ṹ	 һ��ResourceId��Ӧһ��LockRequest�࣬LockRequest���и�����ṹ����������locker��������
        itNew->initNew(this, &_notify); //LockRequest::initNew
		
		//��ȡLockRequest �����LockRequest::initNew����
        request = itNew.objAddr();//FastMapNoAlloc::IteratorImpl::objAddr
    } else { //resId�Ѿ�������_requests�����ȡ��resId��Ӧ��LockRequest
    	//һ���ʾ��ȡ����ʱ��Ȼ��ݹ����»�ȡ�����LockerImpl<>::lockComplete�Ķ�
    	//ֻ��MMAP����Ż��ߵ��������?
        request = it.objAddr();
        isNew = false;
    }

    // Making this call here will record lock re-acquisitions and conversions as well.
    //_idΪ��ʶ��LockerImpl���ΨһID
    //_id��ʶΪLockerImpl���࣬���¼��mode������ResourceId resId, LockMode mode��Ӧ�ļ�������
	//ȫ�ּ���
	//PartitionedInstanceWideLockStats::recordAcquisition;
	globalStats.recordAcquisition(_id, resId, mode); 
	//LockStats::recordAcquisition;  
    _stats.recordAcquisition(resId, mode); //�Ա�LockerImpl._stats��ͳ��

    // Give priority to the full modes for global, parallel batch writer mode,
    // and flush lock so we don't stall global operations such as shutdown or flush.
    const ResourceType resType = resId.getType();
	//ȫ����Դ���Ͳ���������Ϊ��������(MODE_S  MODE_X)��compatibleFirstΪtrue
    if (resType == RESOURCE_GLOBAL || (IsForMMAPV1 && resId == resourceIdMMAPV1Flush)) {
        if (mode == MODE_S || mode == MODE_X) {
            request->enqueueAtFront = true;
            request->compatibleFirst = true;
        }
    } else if (resType != RESOURCE_MUTEX) { //���� �������ͣ�����������
        // This is all sanity checks that the global and flush locks are always be acquired
        // before any other lock has been acquired and they must be in sync with the nesting.
        DEV { //DEBUG���ش򿪲Ż����
            const LockRequestsMap::Iterator itGlobal = _requests.find(resourceIdGlobal);
            invariant(itGlobal->recursiveCount > 0);
            invariant(itGlobal->mode != MODE_NONE);

            // Check the MMAP V1 flush lock is held in the appropriate mode
            invariant(!IsForMMAPV1 ||
                      isLockHeldForMode(resourceIdMMAPV1Flush, _getModeForMMAPV1FlushLock()));
        };
    }

    // The notification object must be cleared before we invoke the lock manager, because
    // otherwise we might reset state if the lock becomes granted very fast.
    _notify.clear();

	//��һ���µ���Դ����������LockManager::lock�������_requests���е�����LockManager::convert
    LockResult result = isNew ? globalLockManager.lock(resId, request, mode)  //LockManager::lock
                              : globalLockManager.convert(resId, request, mode); //LockManager::convert

    if (result == LOCK_WAITING) {
		//PartitionedInstanceWideLockStats::recordWait;
        globalStats.recordWait(_id, resId, mode);
		//LockStats::recordWait;
        _stats.recordWait(resId, mode);
    }

    return result;
}

//LockerImpl<>::lockGlobalComplete
//LockerImpl<>::lock

//ѭ���ȴ�lock�����ֱ��LOCK_OK����LOCK_DEADLOCK���߳�ʱ����ʱʱ��timeout ms��ʱ
//LockerImpl<>::restoreLockState�п�������ݹ����
template <bool IsForMMAPV1>
LockResult LockerImpl<IsForMMAPV1>::lockComplete(ResourceId resId,
                                                 LockMode mode,
                                                 Milliseconds timeout,
                                                 bool checkDeadlock) {
    // Under MMAP V1 engine a deadlock can occur if a thread goes to sleep waiting on
    // DB lock, while holding the flush lock, so it has to be released. This is only
    // correct to do if not in a write unit of work.
    //yieldFlushLockֻ���MMAP����
    const bool yieldFlushLock = IsForMMAPV1 && !inAWriteUnitOfWork() &&
        resId.getType() != RESOURCE_GLOBAL && resId.getType() != RESOURCE_MUTEX &&
        resId != resourceIdMMAPV1Flush;
    if (yieldFlushLock) {
        invariant(unlock(resourceIdMMAPV1Flush));
    }

    LockResult result;

    // Don't go sleeping without bound in order to be able to report long waits or wake up for
    // deadlock detection.
    Milliseconds waitTime = std::min(timeout, DeadlockTimeout);
    const uint64_t startOfTotalWaitTime = curTimeMicros64();
    uint64_t startOfCurrentWaitTime = startOfTotalWaitTime;

    while (true) {
        // It is OK if this call wakes up spuriously, because we re-evaluate the remaining
        // wait time anyways.
        //�ȴ��������������ѣ�LockManager::_onLockModeChanged->CondVarLockGrantNotification::notify���ѵȴ��߳�
        result = _notify.wait(waitTime); //CondVarLockGrantNotification::wait

        // Account for the time spent waiting on the notification object
        const uint64_t curTimeMicros = curTimeMicros64();
        const uint64_t elapsedTimeMicros = curTimeMicros - startOfCurrentWaitTime;
        startOfCurrentWaitTime = curTimeMicros;

		//�ȴ�����ʱ���¼����
        globalStats.recordWaitTime(_id, resId, mode, elapsedTimeMicros);
        _stats.recordWaitTime(resId, mode, elapsedTimeMicros);

        if (result == LOCK_OK)
            break;

        if (checkDeadlock) {
            DeadlockDetector wfg(globalLockManager, this);
            if (wfg.check().hasCycle()) {
                warning() << "Deadlock found: " << wfg.toString();

                globalStats.recordDeadlock(resId, mode);
                _stats.recordDeadlock(resId, mode);

                result = LOCK_DEADLOCK;
                break;
            }
        }

        // If infinite timeout was requested, just keep waiting
        if (timeout == Milliseconds::max()) {
            continue;
        }

        const auto totalBlockTime = duration_cast<Milliseconds>(
            Microseconds(int64_t(curTimeMicros - startOfTotalWaitTime)));
        waitTime = (totalBlockTime < timeout) ? std::min(timeout - totalBlockTime, DeadlockTimeout)
                                              : Milliseconds(0);

		//��ȡ����ʱ
        if (waitTime == Milliseconds(0)) {
            break;
        }
    }

    // Cleanup the state, since this is an unused lock now
    if (result != LOCK_OK) {
        LockRequestsMap::Iterator it = _requests.find(resId);
		//�ͷ���������������������
        _unlockImpl(&it);
    }

	//ֻ���MMAP����
    if (yieldFlushLock) {
        // We cannot obey the timeout here, because it is not correct to return from the lock
        // request with the flush lock released.
        //ע�������������������Ƶݹ����
        invariant(LOCK_OK == lock(resourceIdMMAPV1Flush, _getModeForMMAPV1FlushLock()));
    }

    return result;
}

template <bool IsForMMAPV1>
//ע��LockerImpl<>::unlock �� LockerImpl<>::_unlockImpl������
//  LockerImpl<>::unlock���_unlockImpl���������ͷ�ǰ���������жϣ��ж��ͷ���Ҫ�ӳٵȵ�����
//  �ύ���ͷţ�ȷ�������ύ��ɺ��ͷ������Ӷ���һ����������
bool LockerImpl<IsForMMAPV1>::_unlockImpl(LockRequestsMap::Iterator* it) {
	//LockManager::unlock�ͷŸ���Դ��Ϣ��Ӧ��LockRequest
    if (globalLockManager.unlock(it->objAddr())) {
		//�����ȫ����Դ��Ϣ��������Ҫ��ȫ�ֲ�����Ծ��ͳ��
        if (it->key() == resourceIdGlobal) {
            invariant(_modeForTicket != MODE_NONE);
            auto holder = ticketHolders[_modeForTicket];
            _modeForTicket = MODE_NONE;
            if (holder) {
                holder->release();
            }
            _clientState.store(kInactive);
        }

        scoped_spinlock scopedLock(_lock);
        it->remove();

        return true;
    }

    return false;
}

template <bool IsForMMAPV1>
LockMode LockerImpl<IsForMMAPV1>::_getModeForMMAPV1FlushLock() const {
    invariant(IsForMMAPV1);

    LockMode mode = getLockMode(resourceIdGlobal);
    switch (mode) {
        case MODE_X:
        case MODE_IX:
            return MODE_IX;
        case MODE_S:
        case MODE_IS:
            return MODE_IS;
        default:
            invariant(false);
            return MODE_NONE;
    }
}

template <bool IsForMMAPV1>
bool LockerImpl<IsForMMAPV1>::isGlobalLockedRecursively() {
	//��ȡresourceIdGlobal��Ӧ��LockRequest
    auto globalLockRequest = _requests.find(resourceIdGlobal);

	//���LockerImpl<>::lockComplete�Ķ�
	//�ҵ���Ӧ��LockRequest������recursiveCount���ü�������1��˵�����ڵݹ��������̣�����lock�ȴ���ʱ
    return !globalLockRequest.finished() && globalLockRequest->recursiveCount > 1;
}

//
// Auto classes
//

AutoYieldFlushLockForMMAPV1Commit::AutoYieldFlushLockForMMAPV1Commit(Locker* locker)
    : _locker(static_cast<MMAPV1LockerImpl*>(locker)) {
    // Explicit yielding of the flush lock should happen only at global synchronization points
    // such as database drop. There should not be any active writes at these points.
    invariant(!_locker->inAWriteUnitOfWork());

    if (isMMAPV1()) {
        invariant(_locker->unlock(resourceIdMMAPV1Flush));
    }
}

AutoYieldFlushLockForMMAPV1Commit::~AutoYieldFlushLockForMMAPV1Commit() {
    if (isMMAPV1()) {
        invariant(LOCK_OK ==
                  _locker->lock(resourceIdMMAPV1Flush, _locker->_getModeForMMAPV1FlushLock()));
    }
}


AutoAcquireFlushLockForMMAPV1Commit::AutoAcquireFlushLockForMMAPV1Commit(Locker* locker)
    : _locker(locker), _released(false) {
    // The journal thread acquiring the journal lock in S-mode opens opportunity for deadlock
    // involving operations which do not acquire and release the Oplog collection's X lock
    // inside a WUOW (see SERVER-17416 for the sequence of events), therefore acquire it with
    // check for deadlock and back-off if one is encountered.
    //
    // This exposes theoretical chance that we might starve the journaling system, but given
    // that these deadlocks happen extremely rarely and are usually due to incorrect locking
    // policy, and we have the deadlock counters as part of the locking statistics, this is a
    // reasonable handling.
    //
    // In the worst case, if we are to starve the journaling system, the server will shut down
    // due to too much uncommitted in-memory journal, but won't have corruption.

    while (true) {
        LockResult result = _locker->lock(resourceIdMMAPV1Flush, MODE_S, Milliseconds::max(), true);
        if (result == LOCK_OK) {
            break;
        }

        invariant(result == LOCK_DEADLOCK);

        warning() << "Delayed journaling in order to avoid deadlock during MMAP V1 journal "
                  << "lock acquisition. See the previous messages for information on the "
                  << "involved threads.";
    }
}

void AutoAcquireFlushLockForMMAPV1Commit::upgradeFlushLockToExclusive() {
    // This should not be able to deadlock, since we already hold the S journal lock, which
    // means all writers are kicked out. Readers always yield the journal lock if they block
    // waiting on any other lock.
    invariant(LOCK_OK == _locker->lock(resourceIdMMAPV1Flush, MODE_X, Milliseconds::max(), false));

    // Lock bumps the recursive count. Drop it back down so that the destructor doesn't
    // complain.
    invariant(!_locker->unlock(resourceIdMMAPV1Flush));
}

void AutoAcquireFlushLockForMMAPV1Commit::release() {
    if (!_released) {
        invariant(_locker->unlock(resourceIdMMAPV1Flush));
        _released = true;
    }
}

AutoAcquireFlushLockForMMAPV1Commit::~AutoAcquireFlushLockForMMAPV1Commit() {
    release();
}


namespace {
/**
 *  Periodically purges unused lock buckets. The first time the lock is used again after
 *  cleanup it needs to be allocated, and similarly, every first use by a client for an intent
 *  mode may need to create a partitioned lock head. Cleanup is done roughtly once a minute.
 */
class UnusedLockCleaner : PeriodicTask {
public:
    std::string taskName() const {
        return "UnusedLockCleaner";
    }

    void taskDoWork() {
        LOG(2) << "cleaning up unused lock buckets of the global lock manager";
        getGlobalLockManager()->cleanupUnusedLocks();
    }
} unusedLockCleaner;
}  // namespace


//
// Standalone functions
//

LockManager* getGlobalLockManager() {
    return &globalLockManager;
}

//db.serverstats().locks
//LockStatsServerStatusSection�е���
void reportGlobalLockingStats(SingleThreadedLockStats* outStats) {
	//PartitionedInstanceWideLockStats::report
    globalStats.report(outStats);
}

void resetGlobalLockStats() {
    globalStats.reset();
}


// Ensures that there are two instances compiled for LockerImpl for the two values of the
// template argument.
template class LockerImpl<true>;
template class LockerImpl<false>;

// Definition for the hardcoded localdb and oplog collection info
const ResourceId resourceIdLocalDB = ResourceId(RESOURCE_DATABASE, StringData("local"));
const ResourceId resourceIdOplog = ResourceId(RESOURCE_COLLECTION, StringData("local.oplog.rs"));
const ResourceId resourceIdAdminDB = ResourceId(RESOURCE_DATABASE, StringData("admin"));

/*
db/concurrency/d_concurrency.cpp:      _pbwm(opCtx->lockState(), resourceIdParallelBatchWriterMode),
db/concurrency/d_concurrency.cpp:    : _pbwm(lockState, resourceIdParallelBatchWriterMode, MODE_X),
*/ //��ͬ����أ��ο�Lock::ParallelBatchWriterMode::ParallelBatchWriterMode
const ResourceId resourceIdParallelBatchWriterMode =
    ResourceId(RESOURCE_GLOBAL, ResourceId::SINGLETON_PARALLEL_BATCH_WRITER_MODE);

}  // namespace mongo
