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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/transport/service_state_machine.h"

#include "mongo/config.h"
#include "mongo/db/client.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/stats/counters.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/session.h"
#include "mongo/transport/ticket.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/thread_idle_callback.h"
#include "mongo/util/quick_exit.h"

namespace mongo {
namespace {
//使用Exhaust类型的cursor，这样可以让mongo一批一批的返回查询结果，并且在client请求之前把数据stream过来。
// Set up proper headers for formatting an exhaust request, if we need to
bool setExhaustMessage(Message* m, const DbResponse& dbresponse) {
    MsgData::View header = dbresponse.response.header();
    QueryResult::View qr = header.view2ptr();
    long long cursorid = qr.getCursorId();

    if (!cursorid) {
        return false;
    }

    invariant(dbresponse.exhaustNS.size() && dbresponse.exhaustNS[0]);

    auto ns = dbresponse.exhaustNS;  // m->reset() will free this so we must cache a copy

    m->reset();

    // Rebuild out the response.
    BufBuilder b(512);
    b.appendNum(static_cast<int>(0) /* size set later in setLen() */);
    b.appendNum(header.getId());               // message id
    b.appendNum(header.getResponseToMsgId());  // in response to
    b.appendNum(static_cast<int>(dbGetMore));  // opCode is OP_GET_MORE
    b.appendNum(static_cast<int>(0));          // Must be ZERO (reserved)
    b.appendStr(ns);                           // Namespace
    b.appendNum(static_cast<int>(0));          // ntoreturn
    b.appendNum(cursorid);                     // cursor id from the OP_REPLY

    MsgData::View(b.buf()).setLen(b.len());
    m->setData(b.release());

    return true;
}

}  // namespace

using transport::ServiceExecutor;
using transport::TransportLayer;

/*
 * This class wraps up the logic for swapping/unswapping the Client during runNext().
 *
 * In debug builds this also ensures that only one thread is working on the SSM at once.
 */ 
/*
Breakpoint 1, mongo::ServiceStateMachine::ThreadGuard::ThreadGuard (this=0x7f60ef378e90, ssm=<optimized out>) at src/mongo/transport/service_state_machine.cpp:128
128                             log() << "ddd test ......2.....ServiceStateMachine::ThreadGuard:" << _ssm->_threadName;
(gdb) bt
#0  mongo::ServiceStateMachine::ThreadGuard::ThreadGuard (this=0x7f60ef378e90, ssm=<optimized out>) at src/mongo/transport/service_state_machine.cpp:128
#1  0x00007f60fd72f791 in mongo::ServiceStateMachine::start (this=0x7f60ff8d3a90, ownershipModel=mongo::ServiceStateMachine::kOwned) at src/mongo/transport/service_state_machine.cpp:534
#2  0x00007f60fd72d2b1 in mongo::ServiceEntryPointImpl::startSession (this=<optimized out>, session=...) at src/mongo/transport/service_entry_point_impl.cpp:169
#3  0x00007f60fde02987 in operator() (peerSocket=..., ec=..., __closure=0x7f60ef379280) at src/mongo/transport/transport_layer_asio.cpp:334
#4  operator() (this=0x7f60ef379280) at src/third_party/asio-master/asio/include/asio/detail/bind_handler.hpp:308
#5  asio_handler_invoke<asio::detail::move_binder2<mongo::transport::TransportLayerASIO::_acceptConnection(mongo::transport::TransportLayerASIO::GenericAcceptor&)::<lambda(const std::error_code&, mongo::transport::GenericSocket)>, std::error_code, asio::basic_stream_socket<asio::generic::stream_protocol> > > (function=...) at src/third_party/asio-master/asio/include/asio/handler_invoke_hook.hpp:68
#6  invoke<asio::detail::move_binder2<mongo::transport::TransportLayerASIO::_acceptConnection(mongo::transport::TransportLayerASIO::GenericAcceptor&)::<lambda(const std::error_code&, mongo::transport::GenericSocket)>, std::error_code, asio::basic_stream_socket<asio::generic::stream_protocol> >, mongo::transport::TransportLayerASIO::_acceptConnection(mongo::transport::TransportLayerASIO::GenericAcceptor&)::<lambda(const std::error_code&, mongo::transport::GenericSocket)> > (context=..., function=...) at src/third_party/asio-master/asio/include/asio/detail/handler_invoke_helpers.hpp:37
#7  complete<asio::detail::move_binder2<mongo::transport::TransportLayerASIO::_acceptConnection(mongo::transport::TransportLayerASIO::GenericAcceptor&)::<lambda(const std::error_code&, mongo::transport::GenericSocket)>, std::error_code, asio::basic_stream_socket<asio::generic::stream_protocol> > > (this=<synthetic pointer>, handler=..., function=...) at src/third_party/asio-master/asio/include/asio/detail/handler_work.hpp:81
#8  asio::detail::reactive_socket_move_accept_op<asio::generic::stream_protocol, mongo::transport::TransportLayerASIO::_acceptConnection(mongo::transport::TransportLayerASIO::GenericAcceptor&)::<lambda(const std::error_code&, mongo::transport::GenericSocket)> >::do_complete(void *, asio::detail::operation *, const asio::error_code &, std::size_t) (owner=<optimized out>, base=<optimized out>)
    at src/third_party/asio-master/asio/include/asio/detail/reactive_socket_accept_op.hpp:201
#9  0x00007f60fde0ccd9 in complete (bytes_transferred=<optimized out>, ec=..., owner=0x7f60ff58b100, this=<optimized out>) at src/third_party/asio-master/asio/include/asio/detail/scheduler_operation.hpp:39
#10 asio::detail::scheduler::do_run_one (this=this@entry=0x7f60ff58b100, lock=..., this_thread=..., ec=...) at src/third_party/asio-master/asio/include/asio/detail/impl/scheduler.ipp:400
#11 0x00007f60fde0cf21 in asio::detail::scheduler::run (this=0x7f60ff58b100, ec=...) at src/third_party/asio-master/asio/include/asio/detail/impl/scheduler.ipp:153
#12 0x00007f60fde1716e in asio::io_context::run (this=0x7f60ff7a8a90) at src/third_party/asio-master/asio/include/asio/impl/io_context.ipp:61
#13 0x00007f60fde0071e in mongo::transport::TransportLayerASIO::<lambda()>::operator()(void) const (__closure=0x7f60ff6ac2e8) at src/mongo/transport/transport_layer_asio.cpp:249
#14 0x00007f60fc7428f0 in std::execute_native_thread_routine (__p=<optimized out>) at ../../../.././libstdc++-v3/src/c++11/thread.cc:84
#15 0x00007f60fbf5ee25 in start_thread () from /lib64/libpthread.so.0
#16 0x00007f60fbc8c34d in clone () from /lib64/libc.so.6
(gdb) c
*/
//ThreadGuard 相关的定义会调用
class ServiceStateMachine::ThreadGuard {
    ThreadGuard(ThreadGuard&) = delete;
    ThreadGuard& operator=(ThreadGuard&) = delete;

public:
	// create a ThreadGuard which will take ownership of the SSM in this thread.
	//标记ssm所有权属于本线程
    explicit ThreadGuard(ServiceStateMachine* ssm) : _ssm{ssm} {
		//如果ServiceStateMachine._owned=kUnowned,则ServiceStateMachine._owned赋值为kOwned
		//如果本SSM对应所有权为kUnowned，则进入这里后表示本SSM归属于本线程了，因此所有权有了
        auto owned = _ssm->_owned.compareAndSwap(Ownership::kUnowned, Ownership::kOwned);
        if (owned == Ownership::kStatic) { 
			//sync线程模式,不需要更改线程名，SSM所有权归宿本线程
            dassert(haveClient());
            dassert(Client::getCurrent() == _ssm->_dbClientPtr);
            _haveTakenOwnership = true;
            return;
        }

		//adaptive 动态线程模式走下面的模式

#ifdef MONGO_CONFIG_DEBUG_BUILD
        invariant(owned == Ownership::kUnowned);
//In debug builds this also ensures that only one thread is working on the SSM at once.
        
        _ssm->_owningThread.store(stdx::this_thread::get_id());
#endif

        // Set up the thread name
        //改线程名前的线程名称临时保存起来，为什么命名为oldThreadName，是因为可能即将改名了
        auto oldThreadName = getThreadName(); 
		//当前线程名和之前ssm保存的线程名不一样
        if (oldThreadName != _ssm->_threadName) {
			//记录下当前线程名，即将该命了，所以是old
            _ssm->_oldThreadName = getThreadName().toString();
			//log() << "ddd test ...........ServiceStateMachine::ThreadGuard:" << _ssm->_oldThreadName;
			//把运行本ssm状态机的线程名改为之前保存的线程名
			setThreadName(_ssm->_threadName); //把当前线程改名为_threadName
			//sleep(60);
			//log() << "ddd test ......2.....ServiceStateMachine::ThreadGuard:" << _ssm->_threadName;
        }

        // Swap the current Client so calls to cc() work as expected
        //设置本线程对应client信息,一个链接对应一个client,标识本client当前归属于本线程处理
        Client::setCurrent(std::move(_ssm->_dbClient));
		//本状态机ssm所有权有了，归属于运行本ssm的线程
        _haveTakenOwnership = true;
    }

    // Constructing from a moved ThreadGuard invalidates the other thread guard.
    //构造初始化，状态机及所有权赋值
    ThreadGuard(ThreadGuard&& other)
        : _ssm(other._ssm), _haveTakenOwnership(other._haveTakenOwnership) {
        //原来的other所有权失效
        other._haveTakenOwnership = false;
    }

	//重新赋值
    ThreadGuard& operator=(ThreadGuard&& other) {
        if (this != &other) {
            _ssm = other._ssm;
            _haveTakenOwnership = other._haveTakenOwnership;
			//原来的other所有权失效
            other._haveTakenOwnership = false;
        }
		//返回
        return *this;
    };

    ThreadGuard() = delete;

    ~ThreadGuard() {
		//ssm所有权已确定，则析构的时候，调用release处理，恢复线程原有线程名
        if (_haveTakenOwnership)
            release();
    }

	//获取所有权
    explicit operator bool() const {
#ifdef MONGO_CONFIG_DEBUG_BUILD
        if (_haveTakenOwnership) {
            invariant(_ssm->_owned.load() != Ownership::kUnowned);
            invariant(_ssm->_owningThread.load() == stdx::this_thread::get_id());
            return true;
        } else {
            return false;
        }
#else
        return _haveTakenOwnership;
#endif
    }

	//ServiceStateMachine::_scheduleNextWithGuard
	//设置谁static类型
    void markStaticOwnership() {
        dassert(static_cast<bool>(*this));
        _ssm->_owned.store(Ownership::kStatic);
    }

	//恢复原有线程名，同时把client信息从调度线程归还给状态机
	//boost-asio库中的队列任务调度和底层数据收发流程都切入到worker-n线程
    void release() {
        auto owned = _ssm->_owned.load();

#ifdef MONGO_CONFIG_DEBUG_BUILD
        dassert(_haveTakenOwnership);
        dassert(owned != Ownership::kUnowned);
        dassert(_ssm->_owningThread.load() == stdx::this_thread::get_id());
#endif
		//adaptive异步线程池模式满足if条件，表示SSM固定归属于某个线程
        if (owned != Ownership::kStatic) {
			//本线程拥有currentClient信息，于是把它归还给SSM状态机
            if (haveClient()) {
                _ssm->_dbClient = Client::releaseCurrent();
            }

			//恢复到以前的线程名
            if (!_ssm->_oldThreadName.empty()) {
				//恢复到老线程名
                setThreadName(_ssm->_oldThreadName); 
            }
        }

        // If the session has ended, then it's unsafe to do anything but call the cleanup hook.
        //状态机状态进入end，则调用对应回收hook处理
        if (_ssm->state() == State::Ended) {
            // The cleanup hook gets moved out of _ssm->_cleanupHook so that it can only be called
            // once.  //链接关闭的回收处理 ServiceStateMachine::setCleanupHook
            auto cleanupHook = std::move(_ssm->_cleanupHook);
            if (cleanupHook)
                cleanupHook();

            // It's very important that the Guard returns here and that the SSM's state does not
            // get modified in any way after the cleanup hook is called.
            return;
        }

		//该ssm状态机是否归属于某个线程
        _haveTakenOwnership = false;
        // If owned != Ownership::kOwned here then it can only equal Ownership::kStatic and we
        // should just return
        //归属状态变为未知
        if (owned == Ownership::kOwned) {
            _ssm->_owned.store(Ownership::kUnowned);
        }
    }

private:
	//SSM归属于本线程
    ServiceStateMachine* _ssm;
	//默认false，标识该状态机ssm不归属于任何线程
    bool _haveTakenOwnership = false;
};

//创建新的worker-n线程ServiceExecutorAdaptive::_startWorkerThread->_workerThreadRoutine   conn线程创建见ServiceStateMachine::create 

//TransportLayerASIO::_acceptConnection->ServiceEntryPointImpl::startSession->ServiceStateMachine::create 

//ServiceEntryPointImpl::startSession中调用，session默认对应ASIOSession
std::shared_ptr<ServiceStateMachine> ServiceStateMachine::create(ServiceContext* svcContext,
                                                                 transport::SessionHandle session,
                                                                 transport::Mode transportMode) {
    return std::make_shared<ServiceStateMachine>(svcContext, std::move(session), transportMode);
}

//ServiceStateMachine::create调用   这里面设置线程名
ServiceStateMachine::ServiceStateMachine(ServiceContext* svcContext,
                                         transport::SessionHandle session,
                                         transport::Mode transportMode)
    //Created表示session会话已经创建
    : _state{State::Created},
      //获取对应服务入口点，mongod入口点在ServiceEntryPointMongod类中实现
      //mongos在ServiceEntryPointMongos mongod中实现
      _sep{svcContext->getServiceEntryPoint()},
      //同步线程模式，还是adaptive异步线程池模式
      _transportMode(transportMode),
      //服务上下文，mongod上下文为ServiceContextMongoD，
      //mongos上下文为ServiceContextNoop
      _serviceContext(svcContext),
      //每个链接对应一个session会话
      _sessionHandle(session),
      //根据session构造对应client信息
      _dbClient{svcContext->makeClient("conn", std::move(session))},
      //指向上面的_dbClient
      _dbClientPtr{_dbClient.get()},
      //真正生效在ServiceStateMachine::ThreadGuard 
      //状态机专门负责网络收发过程状态转换，因此状态机处理流程都是网络相关处理，线程名为conn-x线程
      _threadName{str::stream() << "conn-" << _session()->id()} {} //线程名

//获取session信息
const transport::SessionHandle& ServiceStateMachine::_session() const {
	//该客户端链接信息在该结构中，也就是ASIOSession
    return _sessionHandle;
}


/*
#0  mongo::ServiceStateMachine::_processMessage (this=this@entry=0x7f228ce66890, guard=...) at src/mongo/transport/service_state_machine.cpp:345
#1  0x00007f2285357c5f in mongo::ServiceStateMachine::_runNextInGuard (this=0x7f228ce66890, guard=...) at src/mongo/transport/service_state_machine.cpp:424
#2  0x00007f228535b69e in operator() (__closure=0x7f228cedd540) at src/mongo/transport/service_state_machine.cpp:463
#3  std::_Function_handler<void(), mongo::ServiceStateMachine::_scheduleNextWithGuard(mongo::ServiceStateMachine::ThreadGuard, mongo::transport::ServiceExecutor::ScheduleFlags, mongo::ServiceStateMachine::Ownership)::<lambda()> >::_M_invoke(const std::_Any_data &) (__functor=...) at /usr/local/include/c++/5.4.0/functional:1871
#4  0x00007f2286297c12 in operator() (this=0x7f22847a1550) at /usr/local/include/c++/5.4.0/functional:2267
#5  mongo::transport::ServiceExecutorSynchronous::schedule(std::function<void ()>, mongo::transport::ServiceExecutor::ScheduleFlags) (this=this@entry=0x7f2289601480, task=..., 
    flags=flags@entry=mongo::transport::ServiceExecutor::kMayRecurse) at src/mongo/transport/service_executor_synchronous.cpp:125
#6  0x00007f228535685d in mongo::ServiceStateMachine::_scheduleNextWithGuard (this=this@entry=0x7f228ce66890, guard=..., flags=flags@entry=mongo::transport::ServiceExecutor::kMayRecurse, 
    ownershipModel=ownershipModel@entry=mongo::ServiceStateMachine::kOwned) at src/mongo/transport/service_state_machine.cpp:467
#7  0x00007f22853591f1 in mongo::ServiceStateMachine::_sourceCallback (this=this@entry=0x7f228ce66890, status=...) at src/mongo/transport/service_state_machine.cpp:292
#8  0x00007f2285359deb in mongo::ServiceStateMachine::_sourceMessage (this=this@entry=0x7f228ce66890, guard=...) at src/mongo/transport/service_state_machine.cpp:251
#9  0x00007f2285357cf1 in mongo::ServiceStateMachine::_runNextInGuard (this=0x7f228ce66890, guard=...) at src/mongo/transport/service_state_machine.cpp:421
#10 0x00007f228535b69e in operator() (__closure=0x7f228cedd500) at src/mongo/transport/service_state_machine.cpp:463
#11 std::_Function_handler<void(), mongo::ServiceStateMachine::_scheduleNextWithGuard(mongo::ServiceStateMachine::ThreadGuard, mongo::transport::ServiceExecutor::ScheduleFlags, mongo::ServiceStateMachine::Ownership)::<lambda()> >::_M_invoke(const std::_Any_data &) (__functor=...) at /usr/local/include/c++/5.4.0/functional:1871
#12 0x00007f2286298175 in operator() (this=<optimized out>) at /usr/local/include/c++/5.4.0/functional:2267
#13 operator() (__closure=0x7f228ce81410) at src/mongo/transport/service_executor_synchronous.cpp:142
#14 std::_Function_handler<void(), mongo::transport::ServiceExecutorSynchronous::schedule(mongo::transport::ServiceExecutor::Task, mongo::transport::ServiceExecutor::ScheduleFlags)::<lambda()> >::_M_invoke(const std::_Any_data &) (
    __functor=...) at /usr/local/include/c++/5.4.0/functional:1871
#15 0x00007f22867e7d44 in operator() (this=<optimized out>) at /usr/local/include/c++/5.4.0/functional:2267
#16 mongo::(anonymous namespace)::runFunc (ctx=0x7f228cedd0a0) at src/mongo/transport/service_entry_point_utils.cpp:55
#17 0x00007f22834bce25 in start_thread () from /lib64/libpthread.so.0
#18 0x00007f22831ea34d in clone () from /lib64/libc.so.6
*/   
//网络状态机开始接收数据处理   
void ServiceStateMachine::_sourceMessage(ThreadGuard guard) {
    invariant(_inMessage.empty());
	//TransportLayerASIO::sourceMessage  TransportLayerASIO::ASIOSession  后面的wait asio会读取数据放入_inMessage
	//ServiceStateMachine::_sourceMessage->Session::sourceMessage->TransportLayerASIO::sourceMessage
	//获取本session接收数据的ticket，也就是ASIOSourceTicket
    auto ticket = _session()->sourceMessage(&_inMessage);  
	//log() << "ddd test ......1.... _sourceMessage:" << getThreadName(); 
	//进入等等接收数据状态
    _state.store(State::SourceWait);  
	//boost-asio库中的队列任务调度和底层数据收发流程都切入到worker-n线程
    guard.release();

	//guard release后getTransportLayer()->asyncWait等待就进入worker-x线程,其他时候都是conn-x线程
	//log() << "ddd test .. ServiceStateMachine::_sourceMessage ";
	//调用boost-asio进行数据读取及其回调处理
	//线程模型默认同步方式，也就是一个链接一个线程
    if (_transportMode == transport::Mode::kSynchronous) {
        _sourceCallback([this](auto ticket) {
            MONGO_IDLE_THREAD_BLOCK;
			//TransportLayerASIO::wait....最终TransportLayerASIO::ASIOSourceTicket::_bodyCallback读取完整数据后才执行_sourceCallback回调 
            return _session()->getTransportLayer()->wait(std::move(ticket));
        }(std::move(ticket))); 
    } else if (_transportMode == transport::Mode::kAsynchronous) {
    	//TransportLayerASIO::asyncWait   
        _session()->getTransportLayer()->asyncWait( 
            ////TransportLayerASIO::ASIOSourceTicket::_bodyCallback读取到一个完整报文后执行该回调
            std::move(ticket), [this](Status status) { _sourceCallback(status); });
    }
}  

//发送数据
void ServiceStateMachine::_sinkMessage(ThreadGuard guard, Message toSink) {
    // Sink our response to the client
    //ServiceStateMachine::_sinkMessage->Session::sinkMessage->TransportLayerASIO::sinkMessage
    //获取ASIOSinkTicket
    auto ticket = _session()->sinkMessage(toSink);

    _state.store(State::SinkWait);
	//boost-asio库中的队列任务调度和底层数据收发流程都切入到worker-n线程
    guard.release();
	//log() << "ddd test .. ServiceStateMachine::_sinkMessage ";

	//调用boost-asio进行数据发送及其回调处理
    if (_transportMode == transport::Mode::kSynchronous) {
		//最终在ASIOSinkTicket发送数据成功后执行_sinkCallback
        _sinkCallback(_session()->getTransportLayer()->wait(std::move(ticket)));
    } else if (_transportMode == transport::Mode::kAsynchronous) {
		//最终在ASIOSinkTicket发送数据成功后执行_sinkCallback
		_session()->getTransportLayer()->asyncWait(
            std::move(ticket), [this](Status status) { _sinkCallback(status); });
    }
}

//mongos  TransportLayerASIO::asyncWait
//TransportLayerASIO::ASIOSourceTicket::_bodyCallback接收到一个mongodb报文后的回调处理
void ServiceStateMachine::_sourceCallback(Status status) {
    // The first thing to do is create a ThreadGuard which will take ownership of the SSM in this
    // thread.
    //构造ThreadGuard
    ThreadGuard guard(this); 

    // Make sure we just called sourceMessage();
    dassert(state() == State::SourceWait);
	//获取链接session信息
    auto remote = _session()->remote(); 
	//log() << "ddd test .. ServiceStateMachine::_sourceCallback ";

    if (status.isOK()) {
		//进入处理消息阶段  _processMessage
        _state.store(State::Process);

        // Since we know that we're going to process a message, call scheduleNext() immediately
        // to schedule the call to processMessage() on the serviceExecutor (or just unwind the
        // stack)

        // If this callback doesn't own the ThreadGuard, then we're being called recursively,
        // and the executor shouldn't start a new thread to process the message - it can use this
        // one just after this returns.
        //kMayRecurse标识State::Process阶段的处理还是由本线程执行
        //正常流程走这里
        return _scheduleNextWithGuard(std::move(guard), ServiceExecutor::kMayRecurse);
    } else if (ErrorCodes::isInterruption(status.code()) ||
               ErrorCodes::isNetworkError(status.code())) {
        LOG(2) << "Session from " << remote << " encountered a network error during SourceMessage";
        _state.store(State::EndSession);
    } else if (status == TransportLayer::TicketSessionClosedStatus) {
        // Our session may have been closed internally.
        LOG(2) << "Session from " << remote << " was closed internally during SourceMessage";
        _state.store(State::EndSession);
    } else {
        log() << "Error receiving request from client: " << status << ". Ending connection from "
              << remote << " (connection id: " << _session()->id() << ")";
        _state.store(State::EndSession);
    }

    // There was an error receiving a message from the client and we've already printed the error
    // so call runNextInGuard() to clean up the session without waiting.
    //异常流程调用
    _runNextInGuard(std::move(guard));
}

//TransportLayerASIO::ASIOSinkTicket::_sinkCallback中发送报文成功后的回调处理
void ServiceStateMachine::_sinkCallback(Status status) {
    // The first thing to do is create a ThreadGuard which will take ownership of the SSM in this
    // thread.
    ThreadGuard guard(this);

    dassert(state() == State::SinkWait);
	//log() << "ddd test .. ServiceStateMachine::_sinkCallback ";

    // If there was an error sinking the message to the client, then we should print an error and
    // end the session. No need to unwind the stack, so this will runNextInGuard() and return.
    //
    // Otherwise, update the current state depending on whether we're in exhaust or not, and call
    // scheduleNext() to unwind the stack and do the next step.
    if (!status.isOK()) {
        log() << "Error sending response to client: " << status << ". Ending connection from "
              << _session()->remote() << " (connection id: " << _session()->id() << ")";
        _state.store(State::EndSession);
		//异常情况调用
        return _runNextInGuard(std::move(guard));
    } else if (_inExhaust) { //3.6.1版本都不会满足，因为exhaust功能没用起来
    	//注意这里
    	//注意这里的状态是process   _processMessage   还需要继续进行Process处理
        _state.store(State::Process); 
    } else { //正常流程始终进入该分支 _sourceMessage    这里继续进行递归接收数据处理
        _state.store(State::Source); //注意这里的状态是Source,继续接收客户端请求
    }

	//正常流程走这里,继续进行下一次的State::Source报文接收处理
	//这里的kDeferredTask实际上也指定了工作线程下一次的接受mongodb报文这个阶段不会通过
	//_scheduleCondition条件变量通知control控制线程

	//本链接对应的一次mongo访问已经应答完成，需要继续要一次调度了
    return _scheduleNextWithGuard(std::move(guard),
                                  ServiceExecutor::kDeferredTask |
                                      ServiceExecutor::kMayYieldBeforeSchedule);
}

/*
#0  mongo::ServiceStateMachine::_processMessage (this=this@entry=0x7f228ce66890, guard=...) at src/mongo/transport/service_state_machine.cpp:345
#1  0x00007f2285357c5f in mongo::ServiceStateMachine::_runNextInGuard (this=0x7f228ce66890, guard=...) at src/mongo/transport/service_state_machine.cpp:424
#2  0x00007f228535b69e in operator() (__closure=0x7f228cedd540) at src/mongo/transport/service_state_machine.cpp:463
#3  std::_Function_handler<void(), mongo::ServiceStateMachine::_scheduleNextWithGuard(mongo::ServiceStateMachine::ThreadGuard, mongo::transport::ServiceExecutor::ScheduleFlags, mongo::ServiceStateMachine::Ownership)::<lambda()> >::_M_invoke(const std::_Any_data &) (__functor=...) at /usr/local/include/c++/5.4.0/functional:1871
#4  0x00007f2286297c12 in operator() (this=0x7f22847a1550) at /usr/local/include/c++/5.4.0/functional:2267
#5  mongo::transport::ServiceExecutorSynchronous::schedule(std::function<void ()>, mongo::transport::ServiceExecutor::ScheduleFlags) (this=this@entry=0x7f2289601480, task=..., 
    flags=flags@entry=mongo::transport::ServiceExecutor::kMayRecurse) at src/mongo/transport/service_executor_synchronous.cpp:125
#6  0x00007f228535685d in mongo::ServiceStateMachine::_scheduleNextWithGuard (this=this@entry=0x7f228ce66890, guard=..., flags=flags@entry=mongo::transport::ServiceExecutor::kMayRecurse, 
    ownershipModel=ownershipModel@entry=mongo::ServiceStateMachine::kOwned) at src/mongo/transport/service_state_machine.cpp:467
#7  0x00007f22853591f1 in mongo::ServiceStateMachine::_sourceCallback (this=this@entry=0x7f228ce66890, status=...) at src/mongo/transport/service_state_machine.cpp:292
#8  0x00007f2285359deb in mongo::ServiceStateMachine::_sourceMessage (this=this@entry=0x7f228ce66890, guard=...) at src/mongo/transport/service_state_machine.cpp:251
#9  0x00007f2285357cf1 in mongo::ServiceStateMachine::_runNextInGuard (this=0x7f228ce66890, guard=...) at src/mongo/transport/service_state_machine.cpp:421
#10 0x00007f228535b69e in operator() (__closure=0x7f228cedd500) at src/mongo/transport/service_state_machine.cpp:463
#11 std::_Function_handler<void(), mongo::ServiceStateMachine::_scheduleNextWithGuard(mongo::ServiceStateMachine::ThreadGuard, mongo::transport::ServiceExecutor::ScheduleFlags, mongo::ServiceStateMachine::Ownership)::<lambda()> >::_M_invoke(const std::_Any_data &) (__functor=...) at /usr/local/include/c++/5.4.0/functional:1871
#12 0x00007f2286298175 in operator() (this=<optimized out>) at /usr/local/include/c++/5.4.0/functional:2267
#13 operator() (__closure=0x7f228ce81410) at src/mongo/transport/service_executor_synchronous.cpp:142
#14 std::_Function_handler<void(), mongo::transport::ServiceExecutorSynchronous::schedule(mongo::transport::ServiceExecutor::Task, mongo::transport::ServiceExecutor::ScheduleFlags)::<lambda()> >::_M_invoke(const std::_Any_data &) (
    __functor=...) at /usr/local/include/c++/5.4.0/functional:1871
#15 0x00007f22867e7d44 in operator() (this=<optimized out>) at /usr/local/include/c++/5.4.0/functional:2267
#16 mongo::(anonymous namespace)::runFunc (ctx=0x7f228cedd0a0) at src/mongo/transport/service_entry_point_utils.cpp:55
#17 0x00007f22834bce25 in start_thread () from /lib64/libpthread.so.0
#18 0x00007f22831ea34d in clone () from /lib64/libc.so.6
*/
//消息处理都会走到这里  也就是dealTask
void ServiceStateMachine::_processMessage(ThreadGuard guard) {
    invariant(!_inMessage.empty());
	//log() << "	ddd test ...........	_processMessage ";

	//获取类MessageCompressorManager
	//压缩相关得，跳过
    auto& compressorMgr = MessageCompressorManager::forSession(_session());

    _compressorId = boost::none;
    if (_inMessage.operation() == dbCompressed) { //
        MessageCompressorId compressorId;
        auto swm = compressorMgr.decompressMessage(_inMessage, &compressorId);
        uassertStatusOK(swm.getStatus());
        _inMessage = swm.getValue();
        _compressorId = compressorId;
    }

	//入口流量计数
    networkCounter.hitLogicalIn(_inMessage.size());

    // Pass sourced Message to handler to generate response.
    //获取一个唯一的UniqueOperationContext，一个客户端对应一个UniqueOperationContext
    auto opCtx = Client::getCurrent()->makeOperationContext();

    // The handleRequest is implemented in a subclass for mongod/mongos and actually all the
    // database work for this request.
    //ServiceEntryPointMongod::handleRequest  ServiceEntryPointMongos::handleRequest请求处理
    DbResponse dbresponse = _sep->handleRequest(opCtx.get(), _inMessage);

    // opCtx must be destroyed here so that the operation cannot show
    // up in currentOp results after the response reaches the client
    //释放opCtx，这样currentop就看不到了
    opCtx.reset();

    // Format our response, if we have one
    Message& toSink = dbresponse.response;
    if (!toSink.empty()) { //应答处理
        invariant(!OpMsg::isFlagSet(_inMessage, OpMsg::kMoreToCome));
        toSink.header().setId(nextMessageId());
        toSink.header().setResponseToMsgId(_inMessage.header().getId());

        // If this is an exhaust cursor, don't source more Messages
        //3.6.1版本，Exhaust还没有用起来，所以不会进入_inExhaust = true;
        if (dbresponse.exhaustNS.size() > 0 && setExhaustMessage(&_inMessage, dbresponse)) {
            _inExhaust = true;  
        } else {
            _inExhaust = false;
            _inMessage.reset();
        }

        networkCounter.hitLogicalOut(toSink.size());

        if (_compressorId) {
            auto swm = compressorMgr.compressMessage(toSink, &_compressorId.value());
            uassertStatusOK(swm.getStatus());
            toSink = swm.getValue();
        }
        _sinkMessage(std::move(guard), std::move(toSink));

    } else {
        _state.store(State::Source);
        _inMessage.reset();
        return _scheduleNextWithGuard(std::move(guard), ServiceExecutor::kDeferredTask);
    }
}

//实际上没有使用  Service_state_machine_test.cpp才用
void ServiceStateMachine::runNext() {
    return _runNextInGuard(ThreadGuard(this));
}

//ServiceStateMachine::_scheduleNextWithGuard 中执行
void ServiceStateMachine::_runNextInGuard(ThreadGuard guard) {
    auto curState = state();
    dassert(curState != State::Ended);

    // If this is the first run of the SSM, then update its state to Source
    //如果是第一次运行该SSM，则状态为Created，到这里标记可以准备接收数据了
    if (curState == State::Created) { 
		//进入Source等待接收数据
        curState = State::Source;
        _state.store(curState);
    }

    // Make sure the current Client got set correctly
    dassert(Client::getCurrent() == _dbClientPtr);
	/*
	    enum class State {
        Created,     // The session has been created, but no operations have been performed yet
        Source,      // Request a new Message from the network to handle
        SourceWait,  // Wait for the new Message to arrive from the network
        Process,     // Run the Message through the database
        SinkWait,    // Wait for the database result to be sent by the network
        EndSession,  // End the session - the ServiceStateMachine will be invalid after this
        Ended        // The session has ended. It is illegal to call any method besides
                     // state() if this is the current state.
    };
	*/
	/*
	这是个状态机，内核epoll触发有网络数据到来，则执行_sourceMessage，_sourceMessage中调用TransportLayerASIO::wait来
	读取协议栈的数据，读取返回会，在_sourceMessage->ServiceStateMachine::_sourceCallback中把状态改为State::Process,意思是
	可以根据mongod协议
	*/
    try {
        switch (curState) { 
			//接收数据  readTask
            case State::Source:  
                _sourceMessage(std::move(guard));
                break;
			//以及接收到完整的一个mongodb报文，可以内部处理(解析+命令处理+应答客户端)
			//dealTask
            case State::Process: 
                _processMessage(std::move(guard));
                break;
			//链接异常或者已经关闭，则开始回收处理
			//cleanTask
            case State::EndSession:
                _cleanupSession(std::move(guard));
                break;
            default:
                MONGO_UNREACHABLE;
        }

        return;
    } catch (const DBException& e) {
        // must be right above std::exception to avoid catching subclasses
        log() << "DBException handling request, closing client connection: " << redact(e);
    } catch (const std::exception& e) {
        error() << "Uncaught std::exception: " << e.what() << ", terminating";
        quickExit(EXIT_UNCAUGHT);
    }

    if (!guard) {
        guard = ThreadGuard(this);
    }
    _state.store(State::EndSession);
    _cleanupSession(std::move(guard));
}

//ServiceEntryPointImpl::startSession中执行  启动
void ServiceStateMachine::start(Ownership ownershipModel) {
    _scheduleNextWithGuard( 
		//listener线程暂时性的变为conn线程名，在_scheduleNextWithGuard中任
		//务入队完成后，在下面的_scheduleNextWithGuard调用guard.release()恢复listener线程名
        ThreadGuard(this), transport::ServiceExecutor::kEmptyFlags, ownershipModel);
}

//上面的ServiceStateMachine::start(新连接到来)  
//ServiceStateMachine::_sourceCallback(接收到一个完整mongo报文触发)中执行
//ServiceStateMachine::_sinkCallback(发送完成一个完整mongo报文促发)中执行
//任务task等待入队调度
void ServiceStateMachine::_scheduleNextWithGuard(ThreadGuard guard,
                                                 transport::ServiceExecutor::ScheduleFlags flags,
                                                 Ownership ownershipModel) {
	//该func在ServiceExecutorAdaptive::schedule(adaptive)   ServiceExecutorSynchronous::schedule(synchronous)中执行
	//该任务func实际上由worker线程运行,worker线程从asio库的全局队列获取任务调度执行
    auto func = [ ssm = shared_from_this(), ownershipModel ] {
		//ServiceStateMachine::start中的ThreadGuard(this)中线程名赋值为conn-x
		//新任务重新构造guard
        ThreadGuard guard(ssm.get());  
		//说明是sync mode,即一个链接一个线程模式
        if (ownershipModel == Ownership::kStatic) 
            guard.markStaticOwnership();
		
		//对应:ServiceStateMachine::_runNextInGuard
        ssm->_runNextInGuard(std::move(guard)); //新链接conn线程中需要执行的task
    };

	
	//下面的逻辑由listener线程运行

	//和ServiceStateMachine::start中的ThreadGuard(this)对应
	//boost-asio库中的队列任务调度和底层数据收发流程都切入到worker-n线程
    guard.release();
	//ServiceExecutorAdaptive::schedule(adaptive)   ServiceExecutorSynchronous::schedule(synchronous)
	//第一次进入该函数的时候在这里面创建新线程，不是第一次则把task任务入队调度
    Status status = _serviceContext->getServiceExecutor()->schedule(std::move(func), flags);
    if (status.isOK()) {
        return;
    }

	//正常流程不会走到这里，会在上面的schedule里面循环调度处理
	
    // We've had an error, reacquire the ThreadGuard and destroy the SSM
    ThreadGuard terminateGuard(this);  //对应ThreadGuard& operator=(ThreadGuard&& other)

    // The service executor failed to schedule the task. This could for example be that we failed
    // to start a worker thread. Terminate this connection to leave the system in a valid state.
    _terminateAndLogIfError(status);
    _cleanupSession(std::move(terminateGuard));
}

//套接字回收处理
void ServiceStateMachine::terminate() {
    if (state() == State::Ended)
        return;

	//TransportLayerASIO::end
    _session()->getTransportLayer()->end(_session());
}

//ServiceEntryPointImpl::endAllSessions中调用
void ServiceStateMachine::terminateIfTagsDontMatch(transport::Session::TagMask tags) {
    if (state() == State::Ended)
        return;

    auto sessionTags = _session()->getTags();

    // If terminateIfTagsDontMatch gets called when we still are 'pending' where no tags have been
    // set, then skip the termination check.
    if ((sessionTags & tags) || (sessionTags & transport::Session::kPending)) {
        log() << "Skip closing connection for connection # " << _session()->id();
        return;
    }

    terminate();
}

//赋值ServiceEntryPointImpl::startSession，链接回收处理
void ServiceStateMachine::setCleanupHook(stdx::function<void()> hook) {
    invariant(state() == State::Created);
    _cleanupHook = std::move(hook);
}

ServiceStateMachine::State ServiceStateMachine::state() {
    return _state.load();
}

void ServiceStateMachine::_terminateAndLogIfError(Status status) {
    if (!status.isOK()) {
        warning(logger::LogComponent::kExecutor) << "Terminating session due to error: " << status;
        terminate();
    }
}

void ServiceStateMachine::_cleanupSession(ThreadGuard guard) {
	//进入这个状态，等待
    _state.store(State::Ended);

    _inMessage.reset();

    // By ignoring the return value of Client::releaseCurrent() we destroy the session.
    // _dbClient is now nullptr and _dbClientPtr is invalid and should never be accessed.
    Client::releaseCurrent();
}

}  // namespace mongo

