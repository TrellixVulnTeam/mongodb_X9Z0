/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kExecutor;

#include "mongo/platform/basic.h"

#include "mongo/transport/service_executor_synchronous.h"

#include "mongo/db/server_parameters.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/service_entry_point_utils.h"
#include "mongo/util/log.h"
#include "mongo/util/net/thread_idle_callback.h"
#include "mongo/util/processinfo.h"

namespace mongo {
namespace transport {
namespace {

// Tasks scheduled with MayRecurse may be called recursively if the recursion depth is below this
// value.
MONGO_EXPORT_SERVER_PARAMETER(synchronousServiceExecutorRecursionLimit, int, 8);

//��ǰ�߳�����Ҳ���ǵ�ǰconn�߳�����
constexpr auto kThreadsRunning = "threadsRunning"_sd;
constexpr auto kExecutorLabel = "executor"_sd;
constexpr auto kExecutorName = "passthrough"_sd;
}  // namespace

//�̼߳���ı�����ֻ��Ա�����session��Ӧ���߳�
thread_local std::deque<ServiceExecutor::Task> ServiceExecutorSynchronous::_localWorkQueue = {}; //�������
thread_local int ServiceExecutorSynchronous::_localRecursionDepth = 0;
thread_local int64_t ServiceExecutorSynchronous::_localThreadIdleCounter = 0;

ServiceExecutorSynchronous::ServiceExecutorSynchronous(ServiceContext* ctx) {}

//��ȡCPU����
Status ServiceExecutorSynchronous::start() {
    _numHardwareCores = [] {
        ProcessInfo p;
        if (auto availCores = p.getNumAvailableCores()) {
            return static_cast<size_t>(*availCores);
        }
        return static_cast<size_t>(p.getNumCores());
    }();

    _stillRunning.store(true);

    return Status::OK();
}

//ʵ���ϲ��Է���db.shutdown��ʱ��û�н���ú���
Status ServiceExecutorSynchronous::shutdown(Milliseconds timeout) {
    LOG(3) << "Shutting down passthrough executor";
	log() << "Shutting down passthrough executor";

    _stillRunning.store(false);

    stdx::unique_lock<stdx::mutex> lock(_shutdownMutex);
    bool result = _shutdownCondition.wait_for(lock, timeout.toSystemDuration(), [this]() {
        return _numRunningWorkerThreads.load() == 0;
    });

    return result
        ? Status::OK()
        : Status(ErrorCodes::Error::ExceededTimeLimit,
                 "passthrough executor couldn't shutdown all worker threads within time limit.");
}

//ServiceStateMachine::_scheduleNextWithGuard �����µ�conn�߳�
Status ServiceExecutorSynchronous::schedule(Task task, ScheduleFlags flags) {
    if (!_stillRunning.load()) {
        return Status{ErrorCodes::ShutdownInProgress, "Executor is not running"};
    }

	//���˵�һ�ν���ú������ߺ���Ĵ����߳����̣�����������������ǽ����ifѭ������Ϊ״̬����ʼ�ջ�����������
	//�������
    if (!_localWorkQueue.empty()) {
        /*
         * In perf testing we found that yielding after running a each request produced
         * at 5% performance boost in microbenchmarks if the number of worker threads
         * was greater than the number of available cores.
         */
         //��perf�����У����Ƿ��֣���������̵߳��������ڿ����ں˵���������ô��΢��׼�����У�����ÿ����
         //����������������Ϊ5%��
         //kMayYieldBeforeSchedule��ǵ����ؿͻ���Ӧ��ɹ��󣬿�ʼ������һ����������ʱ������øñ��
        if (flags & ScheduleFlags::kMayYieldBeforeSchedule) {
			//Ҳ������������Ӷ�Ӧ���߳��������������0xf����������Ҫ��Ϣһ���
            if ((_localThreadIdleCounter++ & 0xf) == 0) {
				//������Ϣ������ٴ�������ӵ���һ���û�����
				//ʵ�����ǵ���TCMalloc MarkThreadTemporarilyIdleʵ��
                markThreadIdle();
            }
            //������������CPU��������ÿ������һ�����󣬾�yieldһ��		 
            if (_numRunningWorkerThreads.loadRelaxed() > _numHardwareCores) {
                stdx::this_thread::yield();//�̱߳��β�����CPU���ȣ�Ҳ���Ƿ����Ų�
            }
        }
		//log() << "ddd test Starting ServiceExecutorSynchronous::schedule 11";

        // Execute task directly (recurse) if allowed by the caller as it produced better
        // performance in testing. Try to limit the amount of recursion so we don't blow up the
        // stack, even though this shouldn't happen with this executor that uses blocking network
        // I/O.
        /*
		�������������ֱ��ִ������(�ݹ�)����Ϊ���ڲ����в����˸��õ����ܡ��������Ƶݹ����������������
		�Ͳ����ƻ���ջ����ʹ����ʹ����������I/O��ִ������˵�����ǲ�Ӧ�÷����ġ�
		*/
		//���߳����ȴ����Ӧ���ӵ�

		//���ﱣ֤��ͬһ�������readTask��dealTaskһ�εݹ����ִ�У�����ͨ��_localWorkQueue��ӵķ�ʽִ��
		//ֻ����readTask��ȡ������mongodb�������ݺ󣬿�ʼ��һ��dealTask������ȵ�ʱ��Ż�����kMayRecurse���
		if ((flags & ScheduleFlags::kMayRecurse) &&  //��kMayRecurse��ʶ����ֱ�ӵݹ�ִ��
            (_localRecursionDepth < synchronousServiceExecutorRecursionLimit.loadRelaxed())) {
            ++_localRecursionDepth;
			if (_localRecursionDepth > 2)
				log() << "ddd test Starting digui ##  1111111111111 ServiceExecutorSynchronous::schedule, depth:" << _localRecursionDepth;
            task();
        } else {
            //readTask����������
        	if (_localRecursionDepth > 2)
        		log() << "ddd test Starting no digui ## 222222222222 ServiceExecutorSynchronous::schedule, depth:" << _localRecursionDepth;
            _localWorkQueue.emplace_back(std::move(task)); //���
        }
        return Status::OK();
    }

    // First call to schedule() for this connection, spawn a worker thread that will push jobs
    // into the thread local job queue.
    log() << "Starting new executor thread in passthrough mode";

	//����conn�̣߳��߳���conn-xx��ִ�ж�Ӧ��task
    Status status = launchServiceWorkerThread([ this, task = std::move(task) ] {
		//���func���̻߳ص�����
	
        int ret = _numRunningWorkerThreads.addAndFetch(1);

		//task��Ӧ ServiceStateMachine::_runNextInGuard
        _localWorkQueue.emplace_back(std::move(task));
		//ÿ�������Ӷ����ڸ�while��ѭ����������IO�����DB storage����
        while (!_localWorkQueue.empty() && _stillRunning.loadRelaxed()) {
			if (_localRecursionDepth > 2)
				log() << "ddd test Starting while deal ## 333333333333 ServiceExecutorSynchronous::schedule, depth:" << _localRecursionDepth;
            _localRecursionDepth = 1;
			//log() << "Starting new executor thread in passthrough mode ddd tesst 11 size:" << _localWorkQueue.size() << "  _numRunningWorkerThreads:" << ret;
			//�����л�ȡһ��task����ִ��, taskִ�й����л�����SSM״̬������һֱѭ�������Ǹ��̶߳�Ӧ�Ŀͻ��˹ر����ӲŻ��ߵ������_localWorkQueue.pop_front();
			//��Ӧ:ServiceStateMachine::_runNextInGuard  ���̸߳�����������ӵ��������ݰ���������
            _localWorkQueue.front()(); 
			
            _localWorkQueue.pop_front();  //ȥ����taskɾ��
            
        }
		ret = _numRunningWorkerThreads.subtractAndFetch(1);
		//log() << "Starting new executor thread in passthrough mode ddd tesst 22 size:" << _localWorkQueue.size() << "	_numRunningWorkerThreads:" << ret;
		if (ret == 0) { //�����һ�����ӶϿ���ʱ����ߵ���if
        //if (_numRunningWorkerThreads.subtractAndFetch(1) == 0) { //
        	//˵���Ѿ�û�п��������ˣ�shutdown���������˳���
        	//mongo shell��shutdown��ʱ��ֻ�е�û���κ����Ӵ��ڵ�ʱ������������˳�
            _shutdownCondition.notify_all();
			//log() << "Starting new executor thread in passthrough mode ddd tesst 44";
        }

		//�ͻ��˶�Ӧ���ӶϿ���ʱ���ߵ�����
		LOG(3) << "Starting new executor thread in passthrough mode ddd tesst end ";
		//log() << "Starting new executor thread in passthrough mode ddd tesst end ";
    });

    return status;
}

/*
mongos> db.serverStatus().network
{
        "bytesIn" : NumberLong("32650556117"),
        "bytesOut" : NumberLong("596811224034"),
        "physicalBytesIn" : NumberLong("32650556117"),
        "physicalBytesOut" : NumberLong("596811224034"),
        "numRequests" : NumberLong(238541401),
        "compression" : {
                "snappy" : {
                        "compressor" : {
                                "bytesIn" : NumberLong("11389624237"),
                                "bytesOut" : NumberLong("10122531881")
                        },
                        "decompressor" : {
                                "bytesIn" : NumberLong("54878702006"),
                                "bytesOut" : NumberLong("341091660385")
                        }
                }
        },
        "serviceExecutorTaskStats" : {
                "executor" : "passthrough",
                "threadsRunning" : 102
        }
}
*/ 
void ServiceExecutorSynchronous::appendStats(BSONObjBuilder* bob) const {
    BSONObjBuilder section(bob->subobjStart("serviceExecutorTaskStats"));
    section << kExecutorLabel << kExecutorName << kThreadsRunning
            << static_cast<int>(_numRunningWorkerThreads.loadRelaxed());
}

}  // namespace transport
}  // namespace mongo
