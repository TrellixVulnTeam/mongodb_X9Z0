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

#include "mongo/transport/service_executor_adaptive.h"

#include <random>

#include "mongo/db/server_parameters.h"
#include "mongo/transport/service_entry_point_utils.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/log.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/stringutils.h"

#include <asio.hpp>

namespace mongo {
namespace transport {
namespace {
// The executor will always keep this many number of threads around. If the value is -1,
// (the default) then it will be set to number of cores / 2.
//ʵʱ���ߵ���db.adminCommand( { setParameter: 1, adaptiveServiceExecutorReservedThreads: 200} ) 

//����Ĭ�϶�Ҫ��ô����߳�����
MONGO_EXPORT_SERVER_PARAMETER(adaptiveServiceExecutorReservedThreads, int, 1); //ddd change debug

// Each worker thread will allow ASIO to run for this many milliseconds before checking
// whether it should exit
//�����̴߳�ȫ�ֶ����л�ȡ����ִ�У����������û����������Ҫ�ȴ��������þ������Ƶȴ�ʱ������ֵ
MONGO_EXPORT_SERVER_PARAMETER(adaptiveServiceExecutorRunTimeMillis, int, 5000);

// The above parameter will be offset of some random value between -runTimeJitters/
// +runTimeJitters so that not all threads are starting/stopping execution at the same time
//�������Ϊ0����������ӴӶ��л�ȡ����ȴ�ʱ������Ҫ���һ�������
MONGO_EXPORT_SERVER_PARAMETER(adaptiveServiceExecutorRunTimeJitterMillis, int, 500);

// This is the maximum amount of time the controller thread will sleep before doing any
// stuck detection
//��֤control�߳�һ��whileѭ������(ѭ���������ж��Ƿ���Ҫ�����̳߳����̣߳���������̳߳�ѹ�����������߳�)��ʱ��Ϊ�����õ�ֵ
MONGO_EXPORT_SERVER_PARAMETER(adaptiveServiceExecutorStuckThreadTimeoutMillis, int, 250);

// The maximum allowed latency between when a task is scheduled and a thread is started to
// service it.
//���control�߳�һ��ѭ����ʱ�䲻��adaptiveServiceExecutorStuckThreadTimeoutMillis����do {} while()��ֱ����֤����whileѭ���ﵽ��Ҫ��ʱ��ֵ�� {}�о��Ǽ򵥵�sleep��sleep��ֵ���Ǳ��������ֵ��
MONGO_EXPORT_SERVER_PARAMETER(adaptiveServiceExecutorMaxQueueLatencyMicros, int, 500);

// Threads will exit themselves if they spent less than this percentage of the time they ran
// doing actual work.
//���һ���̴߳��������ʱ��/��ʱ��(��������ʱ��)+����ʱ��  Ҳ��������̱߳ȽϿ���
//ִ�� IO ������ʱ��С�� adaptiveServiceExecutorIdlePctThreshold ����ʱ������Զ������߳�
//�߳̿��аٷְ�    Thread was only executing tasks 60% over the last 5096ms. Exiting thread.
//db.adminCommand( { setParameter: 1, adaptiveServiceExecutorIdlePctThreshold: 30} ) 

MONGO_EXPORT_SERVER_PARAMETER(adaptiveServiceExecutorIdlePctThreshold, int, 60);

// Tasks scheduled with MayRecurse may be called recursively if the recursion depth is below this
// value.
//_processMessage�ݹ���ñ���:
//ʵ����һ���̴߳�boost-asio���ȫ�ֶ��л�ȡ����ִ�е�ʱ�������Ҫ���������ӵ����ݣ�����һ������
//����ͨ��_processMessageת������˽���SinkWait״̬��ʱ�򣬼�����ȡ�������ӵ����ݣ�ͬʱ����_sourceCallback
//����Process״̬��������ServiceExecutorAdaptive::schedule�м���"�ݹ�"����process�׶�_processMessage����

//����ݹ���õ�������ֵ 
MONGO_EXPORT_SERVER_PARAMETER(adaptiveServiceExecutorRecursionLimit, int, 8);

//db.serverStatus().network.serviceExecutorTaskStats��ȡ
constexpr auto kTotalQueued = "totalQueued"_sd;
constexpr auto kTotalExecuted = "totalExecuted"_sd;
constexpr auto kTasksQueued = "tasksQueued"_sd;
constexpr auto kDeferredTasksQueued = "deferredTasksQueued"_sd;
//���̳߳���ִ�е�ʱ�䣬��λ΢�롣ע�����ʱ��������worker���̵߳Ļ�����Ϣ��������ʷworker��ͳ��ʱ�䡣
constexpr auto kTotalTimeExecutingUs = "totalTimeExecutingMicros"_sd;
constexpr auto kTotalTimeRunningUs = "totalTimeRunningMicros"_sd;
constexpr auto kTotalTimeQueuedUs = "totalTimeQueuedMicros"_sd;
// ����ʹ��(��IO����)��worker�߳���
constexpr auto kThreadsInUse = "threadsInUse"_sd;
constexpr auto kThreadsRunning = "threadsRunning"_sd;
constexpr auto kThreadsPending = "threadsPending"_sd;
constexpr auto kExecutorLabel = "executor"_sd;
constexpr auto kExecutorName = "adaptive"_sd;

//ticksת��Ϊ��usΪ��λ��ֵ
int64_t ticksToMicros(TickSource::Tick ticks, TickSource* tickSource) {
    invariant(tickSource->getTicksPerSecond() >= 1000000);
	//һ��us�������ٸ�ticks,Ĭ��1
    static const auto ticksPerMicro = tickSource->getTicksPerSecond() / 1000000;
    return ticks / ticksPerMicro;
}

struct ServerParameterOptions : public ServiceExecutorAdaptive::Options {
    int reservedThreads() const final {
		//���û������adaptiveServiceExecutorReservedThreads
        int value = adaptiveServiceExecutorReservedThreads.load();
        if (value == -1) { 
            ProcessInfo pi;
			//Ĭ���߳�������CPU������/2
            value = pi.getNumAvailableCores().value_or(pi.getNumCores()) / 2;
            value = std::max(value, 2);
            adaptiveServiceExecutorReservedThreads.store(value);
            log() << "No thread count configured for executor. Using number of cores / 2: "
                  << value;
        }
        return value;
    }

    Milliseconds workerThreadRunTime() const final {
        return Milliseconds{adaptiveServiceExecutorRunTimeMillis.load()};
    }


	//�������Ϊ��0����������ӴӶ��л�ȡ����ȴ�ʱ������Ҫ���һ�������
    int runTimeJitter() const final {
        return adaptiveServiceExecutorRunTimeJitterMillis.load();
    }
	
	//controller�����߳����˯��ʱ��
    Milliseconds stuckThreadTimeout() const final {
        return Milliseconds{adaptiveServiceExecutorStuckThreadTimeoutMillis.load()};
    }
	
	//worker-control�̵߳ȴ�work pending�̵߳�ʱ��  �ο�ServiceExecutorAdaptive::_controllerThreadRoutine
    Microseconds maxQueueLatency() const final {
        return Microseconds{adaptiveServiceExecutorMaxQueueLatencyMicros.load()};
    }

	//�߳̿��аٷְ٣�����worker�߳��Ƿ��˳�����controller�߳������µ�worker�߳�
	//�ο�ServiceExecutorAdaptive::_controllerThreadRoutine
    int idlePctThreshold() const final {
        return adaptiveServiceExecutorIdlePctThreshold.load();
    }

	//����ݹ���õ�������ֵ
    int recursionLimit() const final {
        return adaptiveServiceExecutorRecursionLimit.load();
    }
};

}  // namespace

thread_local ServiceExecutorAdaptive::ThreadState* ServiceExecutorAdaptive::_localThreadState =
    nullptr;

//ServiceExecutorAdaptive�๹�캯�� 
//TransportLayerManager::createWithConfig��ֵ����
ServiceExecutorAdaptive::ServiceExecutorAdaptive(ServiceContext* ctx,
                                                 std::shared_ptr<asio::io_context> ioCtx)
    : ServiceExecutorAdaptive(ctx, std::move(ioCtx), stdx::make_unique<ServerParameterOptions>()) {}

//�����TransportLayerManager::createWithConfig����
ServiceExecutorAdaptive::ServiceExecutorAdaptive(ServiceContext* ctx,
                                                 std::shared_ptr<asio::io_context> ioCtx,
                                                 std::unique_ptr<Options> config)
    : _ioContext(std::move(ioCtx)),
      _config(std::move(config)),
      _tickSource(ctx->getTickSource()),
      _lastScheduleTimer(_tickSource) {}

ServiceExecutorAdaptive::~ServiceExecutorAdaptive() {
    invariant(!_isRunning.load());
}

//runMongosServer�е���
Status ServiceExecutorAdaptive::start() {
    invariant(!_isRunning.load());
    _isRunning.store(true);
	//�����̳߳�ʼ���������̻߳ص�ServiceExecutorAdaptive::_controllerThreadRoutine
    _controllerThread = stdx::thread(&ServiceExecutorAdaptive::_controllerThreadRoutine, this);
    for (auto i = 0; i < _config->reservedThreads(); i++) {
        _startWorkerThread(); //����ʱ��Ĭ������CPU������/2��worker�߳�
    }

    return Status::OK();
}

Status ServiceExecutorAdaptive::shutdown(Milliseconds timeout) {
    if (!_isRunning.load())
        return Status::OK();

    _isRunning.store(false);

    _scheduleCondition.notify_one();
    _controllerThread.join();

    stdx::unique_lock<stdx::mutex> lk(_threadsMutex);
    _ioContext->stop();
    bool result =
        _deathCondition.wait_for(lk, timeout.toSystemDuration(), [&] { return _threads.empty(); });

    return result
        ? Status::OK()
        : Status(ErrorCodes::Error::ExceededTimeLimit,
                 "adaptive executor couldn't shutdown all worker threads within time limit.");
}

//ServiceStateMachine::_scheduleNextWithGuard
//ServiceStateMachine::_scheduleNextWithGuard  
//adaptiveģʽ���ַ����ӵ��̸߳�_ioContext

//ServiceExecutorAdaptive::schedule��������ӣ�ServiceExecutorAdaptive::_workerThreadRoutine(worker�߳�)����ִ��
Status ServiceExecutorAdaptive::schedule(ServiceExecutorAdaptive::Task task, ScheduleFlags flags) {
	//��ȡ��ǰʱ��
	auto scheduleTime = _tickSource->getTicks();

	//kTasksQueued: ��ǰ��ӻ�ûִ�е�task��
    //_deferredTasksQueued: ��ǰ��ӻ�ûִ�е�deferredTask��
	//defered task����ͨtask�ֿ���¼   _totalQueued=_deferredTasksQueued+_tasksQueued
    auto pendingCounterPtr = (flags & kDeferredTask) ? &_deferredTasksQueued : &_tasksQueued;
    pendingCounterPtr->addAndFetch(1); 

    if (!_isRunning.load()) {
        return {ErrorCodes::ShutdownInProgress, "Executor is not running"};
    }

	//�������task()ִ�к�-task()ִ��ǰ��ʱ�����CPU����������ʱ��
    auto wrappedTask = [ this, task = std::move(task), scheduleTime, pendingCounterPtr ] {
		//worker�̻߳ص���ִ�и�wrappedTask��
        pendingCounterPtr->subtractAndFetch(1);
        auto start = _tickSource->getTicks();
		//�����񱻵�����ӣ���������ִ����ι��̵�ʱ�䣬Ҳ���ǵȴ������ȵ�ʱ��
        _totalSpentQueued.addAndFetch(start - scheduleTime); //�����񱻵�����ӣ���������ִ����ι��̵�ʱ��

		//recursionDepth=0˵����ʼ������ȴ��������п����ǵݹ�ִ��
        if (_localThreadState->recursionDepth++ == 0) {
			//��¼wrappedTask��worker�̵߳���ִ�е���ʼʱ��
            _localThreadState->executing.markRunning();
			//��ǰ����ִ��wrappedTask���̼߳�1
            _threadsInUse.addAndFetch(1);
        }

		//ServiceExecutorAdaptive::_workerThreadRoutineִ��wrappedTask������guard�����func 
        const auto guard = MakeGuard([this, start] { //�ĺ�����task()���к�ִ��
        	//ÿִ��һ��������ɣ���ݹ�����Լ�
            if (--_localThreadState->recursionDepth == 0) {
				//wrappedTask����ִ�����ĵ���ʱ��   _localThreadState->executing.markStopped()���������taskִ�е�ʱ��
                _localThreadState->executingCurRun += _localThreadState->executing.markStopped();
				//�����task()ִ���������ִ��task���߳�-1
				_threadsInUse.subtractAndFetch(1);
            }
			//��ִ�е���������taskÿִ��һ������һ��
            _totalExecuted.addAndFetch(1);
        });

        task();
    };

    // Dispatching a task on the io_context will run the task immediately, and may run it
    // on the current thread (if the current thread is running the io_context right now).
    //
    // Posting a task on the io_context will run the task without recursion.
    //
    // If the task is allowed to recurse and we are not over the depth limit, dispatch it so it
    // can be called immediately and recursively.
/*
//accept��Ӧ��״̬�������������
//TransportLayerASIO::_acceptConnection->basic_socket_acceptor::async_accept
//->start_accept_op->epoll_reactor::post_immediate_completion

//��ͨread write��Ӧ��״̬�������������
//mongodb��ServiceExecutorAdaptive::schedule����->io_context::post(ASIO_MOVE_ARG(CompletionHandler) handler)
//->scheduler::post_immediate_completion
//mongodb��ServiceExecutorAdaptive::schedule����->io_context::dispatch(ASIO_MOVE_ARG(CompletionHandler) handler)
//->scheduler::do_dispatch

//��ͨ��дread write��Ӧ��״̬�������������
//ServiceExecutorAdaptive::_workerThreadRoutine->io_context::run_for->scheduler::wait_one
//->scheduler::do_wait_one����
//mongodb��ServiceExecutorAdaptive::_workerThreadRoutine->io_context::run_one_for
//->io_context::run_one_until->schedule::wait_one
		|
		|1.�Ƚ���״̬���������(Ҳ����mongodb��TransportLayerASIO._workerIOContext  TransportLayerASIO._acceptorIOContext��ص�����)
		|2.��ִ�в���1��Ӧ����������������յ���TransportLayerASIO::_acceptConnection��TransportLayerASIO::ASIOSourceTicket::fillImpl��
		|  TransportLayerASIO::ASIOSinkTicket::fillImpl���������Ӵ������ݶ�д�¼�epollע�ἰ��ȡ��һ���������ĵûص�(�����ͷ����)
		|
	    \|/
//accept��Ӧ��������epoll�¼�ע������:reactive_socket_service_base::start_accept_op->reactive_socket_service_base::start_op
//������epoll�¼�ע������:reactive_descriptor_service::async_read_some->reactive_descriptor_service::start_op->epoll_reactor::start_op
//д����epoll�¼�ע������:reactive_descriptor_service::async_write_some->reactive_descriptor_service::start_op->epoll_reactor::start_op
*/

	//reactive_socket_service_base::start_accept_op 
	//mongodb accept�첽������������: 
	//TransportLayerASIO::_acceptConnection->basic_socket_acceptor::async_accept->reactive_socket_service::async_accept(���ﹹ��reactive_socket_accept_op_base��������epoll��ȡ�����ӵ�handler�ص�Ҳ�������do_complete��ִ��)
	//->reactive_socket_service_base::start_accept_op->reactive_socket_service_base::start_op�н���acceptע��
	
	//mongodb�첽��ȡ����:
	 //mongodbͨ��TransportLayerASIO::ASIOSession::opportunisticRead->asio::async_read->start_read_buffer_sequence_op->read_op::operator
	 //->basic_stream_socket::async_read_some->reactive_socket_service_base::async_receive(���ﹹ��reactive_socket_recv_op��������epoll�����ݼ����ȡ��һ������mongo���ĵ�handler�ص�Ҳ�������do_complete��ִ��)
	 //->reactive_socket_service_base::start_op�н���EPOLL�¼�ע��
	//mongodbͬ����ȡ����:
	 //mongodb��opportunisticRead->asio:read->detail::read_buffer_sequence->basic_stream_socket::read_some->basic_stream_socket::read_some
	 //reactive_socket_service_base::receive->socket_ops::sync_recv(����ֱ�Ӷ�ȡ����)
	
	//write�����첽��������: 
	 //mongodb��ͨ��opportunisticWrite->asio::async_write->start_write_buffer_sequence_op->detail::write_op() 
	 //->basic_stream_socket::async_write_some->reactive_socket_service_base::async_send(���ﹹ��reactive_socket_send_op_base��������epollд���ݼ����ȡ��һ������mongo���ĵ�handler�ص�Ҳ�������do_complete��ִ��)
	 //->reactive_socket_service_base::start_op->reactive_socket_service_base::start_op�н���EPOLL�¼�ע��
	//writeͬ��������������:
	 //ͬ��д����asio::write->write_buffer_sequence->basic_stream_socket::write_some->reactive_socket_service_base::send->socket_ops::sync_send(������������ͬ������)
	

	//�����е�wrappedTask������ServiceExecutorAdaptive::_workerThreadRoutine������

	//kMayRecurse��ʶ�����񣬻���еݹ����
    if ((flags & kMayRecurse) && //�ݹ���ã��������ɱ��̴߳���
    //�ݹ���ñ���: (��ȡһ����������+���������ڶ�������ݹ����) ���̻߳���һ���߳̿��Դ��������ӵ�������Ϊ�������ݸ��ͻ��˿������첽�ģ����Դ���ͬʱ������������������
    	//�ݹ���Ȼ�û�ﵽ���ޣ������ɱ��̼߳�������ִ��wrappedTask����
        (_localThreadState->recursionDepth + 1 < _config->recursionLimit())) {
        //���߳�����ֱ��ִ��wrappedTask���񣬲�����ӵ�boost-asioȫ�ֶ��еȴ�����ִ��
        //io_context::dispatch   io_context::dispatch 
        if(_localThreadState->recursionDepth > 2) //ʵ������Զ�������2���Ӵ����������source�׶κ����process�׶ε��ʺ����˸ñ�ʶ��process����һ�׶������Ѿ�û�иñ�ʶ��
			log() << "ddd test .... _localThreadState->recursionDepth:" << _localThreadState->recursionDepth;
        _ioContext->dispatch(std::move(wrappedTask));  
    } else { //���   io_context::post
    	//��ӵ�schedule��ȫ�ֶ��У��ȴ������̵߳���
        _ioContext->post(std::move(wrappedTask));
    }

    _lastScheduleTimer.reset();
	//�ܵ����������,Ҳ���ǵ���ServiceStateMachine::_scheduleNextWithGuard->ServiceExecutorAdaptive::schedule�Ĵ���
    _totalQueued.addAndFetch(1); 

    // Deferred tasks never count against the thread starvation avoidance. For other tasks, we
    // notify the controller thread that a task has been scheduled and we should monitor thread
    // starvation.

	//kDeferredTask������Ч������
	//�����е����������ڿ����߳�����˵��workerѹ��������Ҫ�����µ�worker�߳�
    if (_isStarved() && !(flags & kDeferredTask)) {//kDeferredTask������Ч������
    	//����������֪ͨcontroler�߳�,֪ͨ��ServiceExecutorAdaptive::schedule���ȴ���_controllerThreadRoutine
        _scheduleCondition.notify_one();
    }

    return Status::OK();
}

//�ж϶����е��������Ϳ����߳�����С����������task����
bool ServiceExecutorAdaptive::_isStarved() const {
    // If threads are still starting, then assume we won't be starved pretty soon, return false
    //����������������̣߳���ֱ�ӷ���
    if (_threadsPending.load() > 0)
        return false;

    auto tasksQueued = _tasksQueued.load();
    // If there are no pending tasks, then we definitely aren't starved
    if (tasksQueued == 0)
        return false;

    // The available threads is the number that are running - the number that are currently
    // executing
    auto available = _threadsRunning.load() - _threadsInUse.load();

	//�����е����������ڿ����߳�����˵��workerѹ��������Ҫ�����µ�worker�߳�
    return (tasksQueued > available);
}

//worker-x�߳�Ĭ����CPU/2����������controller�̻߳���ݸ�����_controllerThreadRoutine�ж�̬����worker�߳���
//���controller�̷߳��ָ��ظߣ���ôworker�߳���Ҳ�������ӣ����������ȥ�ˣ�worker�̸߳�����������������Ƿ��˳����������߳�

//worker-controller thread   ServiceExecutorAdaptive::start�д���  woker�����߳�
void ServiceExecutorAdaptive::_controllerThreadRoutine() {
    setThreadName("worker-controller"_sd);  
    // The scheduleCondition needs a lock to wait on.
    stdx::mutex fakeMutex;
    stdx::unique_lock<stdx::mutex> fakeLk(fakeMutex);

    TickTimer sinceLastControlRound(_tickSource);
    TickSource::Tick lastSpentExecuting = _getThreadTimerTotal(ThreadTimer::Executing);
    TickSource::Tick lastSpentRunning = _getThreadTimerTotal(ThreadTimer::Running);

    while (_isRunning.load()) {
        // Make sure that the timer gets reset whenever this loop completes
        //һ��while������ʱ��ִ�ж�Ӧfunc ,Ҳ���ǽ�����ʱ�����Ϊ��ʼʱ��
        const auto timerResetGuard =
            MakeGuard([&sinceLastControlRound] { sinceLastControlRound.reset(); });

		//�ȴ������߳�֪ͨ,���ȴ�stuckThreadTimeout
        _scheduleCondition.wait_for(fakeLk, _config->stuckThreadTimeout().toSystemDuration());

        // If the executor has stopped, then stop the controller altogether
        if (!_isRunning.load())
            break;

        double utilizationPct;
        {
			//��ȡ�����߳�ִ���������ʱ��
            auto spentExecuting = _getThreadTimerTotal(ThreadTimer::Executing);
			//��ȡ�����߳�������������ʱ��(��������ʱ��+ִ������ʱ��+�����̵߳�ʱ��)
            auto spentRunning = _getThreadTimerTotal(ThreadTimer::Running);

			//Ҳ����while��ִ��һ�����������spentExecuting��ֵ��
			//Ҳ����spentExecuting����whileһ��ѭ����Executing time��ʼֵ, lastSpentExecuting����һ��ѭ����Ӧ�Ľ���timeֵ
            auto diffExecuting = spentExecuting - lastSpentExecuting;
			////Ҳ����spentRunning����whileһ��ѭ����running time��ʼֵ, lastSpentRunning����һ��ѭ����Ӧ�Ľ���timeֵ
            auto diffRunning = spentRunning - lastSpentRunning;

            // If no threads have run yet, then don't update anything
            if (spentRunning == 0 || diffRunning == 0)
                utilizationPct = 0.0;
            else {
                lastSpentExecuting = spentExecuting;
                lastSpentRunning = spentRunning;

				//һ��whileѭ�������������߳�ִ�������ʱ����߳�������ʱ��ı�ֵ
                utilizationPct = diffExecuting / static_cast<double>(diffRunning);
                utilizationPct *= 100;
            }
        }

        // If the wait timed out then either the executor is idle or stuck
        //Ҳ���Ǳ�while()ִ��һ�ε�ʱ���ֵ��Ҳ�����ϴ��ߵ������ʱ��ͱ����ߵ������ʱ���ֵ���ڸ÷�ֵ
        //Ҳ���ǿ����߳�̫��û���ж��̳߳��Ƿ�����
        if (sinceLastControlRound.sinceStart() >= _config->stuckThreadTimeout()) {
            // Each call to schedule updates the last schedule ticks so we know the last time a
            // task was scheduled
            Milliseconds sinceLastSchedule = _lastScheduleTimer.sinceStart();

            // If the number of tasks executing is the number of threads running (that is all
            // threads are currently busy), and the last time a task was able to be scheduled was
            // longer than our wait timeout, then we can assume all threads are stuck.
            //
            // In that case we should start the reserve number of threads so fully unblock the
            // thread pool.
            //use�е��߳���=�̳߳����ܵ��߳�����˵���̳߳����߳�̫æ��
            if ((_threadsInUse.load() == _threadsRunning.load()) &&
                (sinceLastSchedule >= _config->stuckThreadTimeout())) {
                log() << "Detected blocked worker threads, "
                      << "starting new reserve threads to unblock service executor";
				//һ������������ô���̣߳�����ʵ�����и����⣬�����������adaptiveServiceExecutorReservedThreads
				//�ǳ����������һ���Դ����ǳ�����̣߳����ܷ������Ϊϵͳƿ��
				//����mongodb�ٷ����������һ����������
                for (int i = 0; i < _config->reservedThreads(); i++) {
                    _startWorkerThread();
                }
            }
            continue;
        }

		//��ǰ��worker�߳���
        auto threadsRunning = _threadsRunning.load();
		//��֤�̳߳���worker�߳������ٶ�ҪreservedThreads��
        if (threadsRunning < _config->reservedThreads()) {
            log() << "Starting " << _config->reservedThreads() - threadsRunning
                  << " to replenish reserved worker threads";
            while (_threadsRunning.load() < _config->reservedThreads()) {
                _startWorkerThread();
            }
        }

        // If the utilization percentage is lower than our idle threshold, then the threads we
        // already have aren't saturated and we shouldn't consider adding new threads at this
        // time.

		//worker�̷߳ǿ���ռ��С�ڸ÷�ֵ��˵��ѹ�����󣬲���Ҫ����worker�߳���
        if (utilizationPct < _config->idlePctThreshold()) {
            continue;
        }
		//�ߵ����˵������worker����ѹ�����Ǻܴ��

        // While there are threads pending sleep for 50 microseconds (this is our thread latency
        // perf budget).
        //
        // If waiting for pending threads takes longer than the stuckThreadTimeout, then the
        // pending threads may be stuck and we should loop back around.
        //����������ѭ��stuckThreadTimeout���룬ֱ�����ǵȴ�worker�̴߳�����������������task
        //��Ϊ��������ڴ�����worker�̣߳����ǵȴ�һС�ᣬ���ȴ�stuckThreadTimeout ms

		//��֤һ��whileѭ��ʱ��ΪstuckThreadTimeout
        do {
            stdx::this_thread::sleep_for(_config->maxQueueLatency().toSystemDuration());
        } while ((_threadsPending.load() > 0) &&
                 (sinceLastControlRound.sinceStart() < _config->stuckThreadTimeout()));


        // If the number of pending tasks is greater than the number of running threads minus the
        // number of tasks executing (the number of free threads), then start a new worker to
        // avoid starvation.
        //������������������ÿ����߳�����˵��ѹ���е�󣬸��̳߳�����һ���µ��߳�
        if (_isStarved()) {
            log() << "Starting worker thread to avoid starvation.";
            _startWorkerThread();
        }
    }
}

//�����µ�worker-n�߳�ServiceExecutorAdaptive::_startWorkerThread->_workerThreadRoutine   conn�̴߳�����ServiceStateMachine::create 
//ServiceExecutorAdaptive::start  _controllerThreadRoutine  �е���
void ServiceExecutorAdaptive::_startWorkerThread() {
    stdx::unique_lock<stdx::mutex> lk(_threadsMutex);
	//warning() << "ddd test   _startWorkerThread:  num1:" << _threads.size();
	//��stdx::list list��׷��һ��ThreadState
    auto num = _threads.size();
    auto it = _threads.emplace(_threads.begin(), _tickSource); 
	warning() << "ddd test   _startWorkerThread: 22222 num2:" << _threads.size();

	//��û��ִ�й�task������߳���
    _threadsPending.addAndFetch(1);
	//worker�߳�����
    _threadsRunning.addAndFetch(1);

    lk.unlock();

    const auto launchResult = //�̻߳ص�����Ϊ_workerThreadRoutine
        launchServiceWorkerThread([this, num, it] { _workerThreadRoutine(num, it); });

    if (!launchResult.isOK()) {
        warning() << "Failed to launch new worker thread: " << launchResult;
        lk.lock();
        _threadsPending.subtractAndFetch(1);
        _threadsRunning.subtractAndFetch(1);
        _threads.erase(it);
    }
}

//����һ�����������λms
Milliseconds ServiceExecutorAdaptive::_getThreadJitter() const {
    static stdx::mutex jitterMutex;
	//
    static std::default_random_engine randomEngine = [] {
        std::random_device seed;
        return std::default_random_engine(seed());
    }();

	//��������������һ�������
    auto jitterParam = _config->runTimeJitter();
    if (jitterParam == 0)
        return Milliseconds{0};

    std::uniform_int_distribution<> jitterDist(-jitterParam, jitterParam);

    stdx::lock_guard<stdx::mutex> lk(jitterMutex);
    auto jitter = jitterDist(randomEngine);
    if (jitter > _config->workerThreadRunTime().count())
        jitter = 0;

    return Milliseconds{jitter};
}

//���̳߳���ִ�е�ʱ�䡣ע�����ʱ��������worker���̵߳Ļ�����Ϣ��������ʷworker��ͳ��ʱ�䡣
//��ȡָ��which���͵Ĺ����߳��������ʱ�䣬
//����Running�����߳�������ʱ��(�ȴ�����+������) 
//Executingֻ����ִ��task�����ʱ��
TickSource::Tick ServiceExecutorAdaptive::_getThreadTimerTotal(ThreadTimer which) const {
	//��ȡһ��ʱ�����tick
	TickSource::Tick accumulator;
	//�Ȱ������ĵ��̵߳�����ͳ�Ƴ���
    switch (which) { 
		//��ȡ���������Ѿ��������߳�ִ���������ʱ��(ֻ����ִ�������ʱ��)
        case ThreadTimer::Running:
            accumulator = _pastThreadsSpentRunning.load();
            break;
		//��ȡ���������Ѿ��������߳�������������ʱ��(��������ʱ��+ִ������ʱ��)
        case ThreadTimer::Executing: 
            accumulator = _pastThreadsSpentExecuting.load();
            break;
    }

	//Ȼ���ٰ�ͳ�Ƶ�ǰ�������е�worker�̵߳Ĳ�ͬ���͵�ͳ��ʱ��ͳ�Ƴ���
    stdx::lock_guard<stdx::mutex> lk(_threadsMutex);
    for (auto& thread : _threads) { 
        switch (which) {
			//��ȡ��ǰ�̳߳������й����߳�ִ������ʱ��
            case ThreadTimer::Running:
                accumulator += thread.running.totalTime();
                break;
			//��ȡ��ǰ�̳߳������й����߳�������������ʱ��(��������ʱ��+ִ������ʱ��)
            case ThreadTimer::Executing: 
                accumulator += thread.executing.totalTime();
                break;
        }
    }

	//���ص�ʱ���������������ٵ��̺߳͵�ǰ�������е��̵߳����ͳ��
    return accumulator;
}

//ServiceExecutorAdaptive::_startWorkerThread
//worker-x�߳�Ĭ����CPU/2����������controller�̻߳���ݸ�����_controllerThreadRoutine�ж�̬����worker�߳���
//���controller�̷߳��ָ��ظߣ���ôworker�߳���Ҳ�������ӣ����������ȥ�ˣ�worker�̸߳�����������������Ƿ��˳����������߳�

//ServiceExecutorAdaptive::schedule���������(listener�߳�)��ServiceExecutorAdaptive::_workerThreadRoutine(worker�߳�)����ִ��
void ServiceExecutorAdaptive::_workerThreadRoutine(
    int threadId, ServiceExecutorAdaptive::ThreadList::iterator state) {

    _localThreadState = &(*state);
    {
		//worker-N�߳���
        std::string threadName = str::stream() << "worker-" << threadId;
        setThreadName(threadName);
    }

    log() << "Started new database worker thread " << threadId;

    // Whether a thread is "pending" reflects whether its had a chance to do any useful work.
    // When a thread is pending, it will only try to run one task through ASIO, and report back
    // as soon as possible so that the thread controller knows not to keep starting threads while
    // the threads it's already created are finishing starting up.
    //���̵߳�һ��ִ��while�е������ʱ��Ϊture�����涼��false
    bool stillPending = true; //��ʾ���߳��Ǹմ��������ģ���û��ִ���κ�һ��task

	//MakeGuard�������һ����ʱ�����������˳����ִ��func��Ҳ���Ǳ��߳��Զ��˳����ٵ�ʱ��ִ��
    const auto guard = MakeGuard([this, &stillPending, state] {
    	//()�е�func�����ĵ�ʱ���ִ�У�Ҳ���Ǹú����˳���ʱ����Ҫ����guard�������������е��ø�func
        if (stillPending)
            _threadsPending.subtractAndFetch(1);
		//�߳��˳��󣬵�ǰ�������е��߳�����1
        _threadsRunning.subtractAndFetch(1);
		//��¼����˳����߳���������ִ���������ʱ��
        _pastThreadsSpentRunning.addAndFetch(state->running.totalTime());
		//��¼����˳����߳������������е���ʱ��(�����ȴ�IO������IO�����ʱ��)
        _pastThreadsSpentExecuting.addAndFetch(state->executing.totalTime());

        {
            stdx::lock_guard<stdx::mutex> lk(_threadsMutex);
            _threads.erase(state);
        }
        _deathCondition.notify_one();
    });

	//�Ƿ�����һ�������
    auto jitter = _getThreadJitter();

    while (_isRunning.load()) {
        // We don't want all the threads to start/stop running at exactly the same time, so the
        // jitter setParameter adds/removes a random small amount of time to the runtime.
        Milliseconds runTime = _config->workerThreadRunTime() + jitter;
        dassert(runTime.count() > 0);

        // Reset ticksSpentExecuting timer
        //����ѭ��ִ��task��ʱ�䣬����������IO�ȴ�ʱ��
        state->executingCurRun = 0;

        try {
            asio::io_context::work work(*_ioContext);
            // If we're still "pending" only try to run one task, that way the controller will
            // know that it's okay to start adding threads to avoid starvation again.
            state->running.markRunning(); //��¼��ʼʱ�䣬Ҳ��������ִ�п�ʼʱ��
            //����������ServiceExecutorAdaptive::schedule��Ӧ��task,�߳���Ҳ����Ϊconn�߳�
            //�ڸ��߳��첽���������У�ͨ��ServiceStateMachine::_sinkCallback  ServiceStateMachine::_sourceCallback��worker�̸߳�Ϊconn�߳�

			//ִ��ServiceExecutorAdaptive::schedule�ж�Ӧ��task
			if (stillPending) {
				//��һ��ʱ���ڴ����¼�ѭ�����������������ͬʱû������������ǲ������ֱ��io_context���� stop() ����ֹͣ �� ��ʱ Ϊֹ
				//ִ��һ������ͻ᷵��
				_ioContext->run_one_for(runTime.toSystemDuration());
            } else {  // Otherwise, just run for the full run period
            	//_ioContext��Ӧ����������ִ����ɺ�Ż᷵��
                _ioContext->run_for(runTime.toSystemDuration()); //io_context::run_for
            }
			
            // _ioContext->run_one() will return when all the scheduled handlers are completed, and
            // you must call restart() to call run_one() again or else it will return immediately.
            // In the case where the server has just started and there has been no work yet, this
            // means this loop will spin until the first client connect. Thsi call to restart avoids
            // that.
            if (_ioContext->stopped()) //��֤��worker�̼߳����������紦�����򱾴��첽�������������̻߳��˳�
                _ioContext->restart();
            // If an exceptione escaped from ASIO, then break from this thread and start a new one.
        } catch (std::exception& e) {
        	//����IO����������쳣�ˣ�����߳��˳����٣�ͬʱ�򴴽�һ�����߳�
            log() << "Exception escaped worker thread: " << e.what()
                  << " Starting new worker thread.";
            _startWorkerThread();
            break;
        } catch (...) {
            log() << "Unknown exception escaped worker thread. Starting new worker thread.";
            _startWorkerThread();
            break;
        }
		//���̱߳���ѭ�����ĵ�ʱ�䣬����IO�ȴ���ִ�ж�Ӧ�����¼���Ӧtask��ʱ��
        auto spentRunning = state->running.markStopped();

        // If we're still pending, let the controller know and go back around for another go
        //
        // Otherwise we can think about exiting if the last call to run_for() wasn't very
        // productive
        
        if (stillPending) { //���̵߳�һ��ִ��while�е������ʱ��Ϊture�����涼��false
            _threadsPending.subtractAndFetch(1);
            stillPending = false;
        } else if (_threadsRunning.load() > _config->reservedThreads()) { //
        	//���һ�ε���run_for()��Ч�ʲ��Ǻܸߣ����ǿ��Կ����˳�
            // If we spent less than our idle threshold actually running tasks then exit the thread.
            // This time measurement doesn't include time spent running network callbacks, so the
            // threshold is lower than you'd expect.
            dassert(spentRunning < std::numeric_limits<double>::max());

            // First get the ratio of ticks spent executing to ticks spent running. We expect this
            // to be <= 1.0
            //������ѭ�����߳��������������ʱ���뱾��ѭ����ʱ��(��ʱ�����IO�ȴ���IO������ʱ��)
            double executingToRunning = state->executingCurRun / static_cast<double>(spentRunning);

            // Multiply that by 100 to get the percentage of time spent executing tasks. We expect
            // this to be <= 100.
            executingToRunning *= 100;
            dassert(executingToRunning <= 100);

            int pctExecuting = static_cast<int>(executingToRunning);
			//�̺߳ܶ࣬������ָ�����ã��������������������worker�̻߳��˳����̱߳ȽϿ��У��˳�
			//����߳�������������ִ��ʱ��ռ��С�ڸ�ֵ����˵�����̱߳ȽϿ��У������˳���
            if (pctExecuting < _config->idlePctThreshold()) {
                log() << "Thread was only executing tasks " << pctExecuting << "% over the last "
                      << runTime << ". Exiting thread.";
                break;
            }
        }
    }
}

//db.serverStatus().network.serviceExecutorTaskStats  //db.serverStatus().network��ȡ
void ServiceExecutorAdaptive::appendStats(BSONObjBuilder* bob) const {
	//����ĵ�һ������,�ڶ����Ƕ�Ӧ��ֵ����"totalTimeRunningMicros" : NumberLong("69222621699"),
    BSONObjBuilder section(bob->subobjStart("serviceExecutorTaskStats"));
	//kTotalQueued:�ܵ����������,Ҳ���ǵ���ServiceStateMachine::_scheduleNextWithGuard->ServiceExecutorAdaptive::schedule�Ĵ���
    //kExecutorName��adaptive
    //kTotalExecuted: ��ִ�е�������
    //kTasksQueued: ��ǰ��ӻ�ûִ�е�task��
    //_deferredTasksQueued: ��ǰ��ӻ�ûִ�е�deferredTask��
    //kThreadsInUse: ��ǰ����ִ��task���߳�
    //kTotalQueued=kDeferredTasksQueued(deferred task)+kTasksQueued(��ͨtask)
    //kThreadsPending����ǰ�մ������������������߳�������Ҳ���Ǵ���������û��ִ��task���߳���
    //kThreadsRunning�����Ѿ�ִ�й�task���߳�������Ҳ������Щ�̲߳��Ǹոմ���������
	//kTotalTimeRunningUs:��¼����˳����߳���������ִ���������ʱ��
	//kTotalTimeExecutingUs����¼����˳����߳������������е���ʱ��(�����ȴ�IO������IO�����ʱ��)
	//kTotalTimeQueuedUs: �����񱻵�����ӣ���������ִ����ι��̵�ʱ�䣬Ҳ���ǵȴ������ȵ�ʱ��

    section << kExecutorLabel << kExecutorName                                             //
            << kTotalQueued << _totalQueued.load()                                         //
            << kTotalExecuted << _totalExecuted.load()                                     //
            << kTasksQueued << _tasksQueued.load()                                         //
            << kDeferredTasksQueued << _deferredTasksQueued.load()                         //
            << kThreadsInUse << _threadsInUse.load()                                       //
            << kTotalTimeRunningUs                                                         //
            << ticksToMicros(_getThreadTimerTotal(ThreadTimer::Running), _tickSource)      //
            << kTotalTimeExecutingUs                                                       //
            << ticksToMicros(_getThreadTimerTotal(ThreadTimer::Executing), _tickSource)    //
            << kTotalTimeQueuedUs << ticksToMicros(_totalSpentQueued.load(), _tickSource)  //
            << kThreadsRunning << _threadsRunning.load()                                   //
            << kThreadsPending << _threadsPending.load();
    section.doneFast();
}

}  // namespace transport
}  // namespace mongo
