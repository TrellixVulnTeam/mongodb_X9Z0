/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kASIO

#include "mongo/platform/basic.h"

#include "mongo/executor/connection_pool.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/destructor_guard.h"
#include "mongo/util/log.h"
#include "mongo/util/lru_cache.h"
#include "mongo/util/scopeguard.h"

// One interesting implementation note herein concerns how setup() and
// refresh() are invoked outside of the global lock, but setTimeout is not.
// This implementation detail simplifies mocks, allowing them to return
// synchronously sometimes, whereas having timeouts fire instantly adds little
// value. In practice, dumping the locks is always safe (because we restrict
// ourselves to operations over the connection).

namespace mongo {
namespace executor {

/**
 * A pool for a specific HostAndPort
 *
 * Pools come into existance the first time a connection is requested and
 * go out of existence after hostTimeout passes without any of their
 * connections being used.
 */
class ConnectionPool::SpecificPool {
public:
    /**
     * These active client methods must be used whenever entering a specific pool outside of the
     * shutdown background task.  The presence of an active client will bump a counter on the
     * specific pool which will prevent the shutdown thread from deleting it.
     *
     * The complexity comes from the need to hold a lock when writing to the
     * _activeClients param on the specific pool.  Because the code beneath the client needs to lock
     * and unlock the parent mutex (and can leave unlocked), we want to start the client with the
     * lock acquired, move it into the client, then re-acquire to decrement the counter on the way
     * out.
     *
     * It's used like:
     *
     * pool.runWithActiveClient([](stdx::unique_lock<stdx::mutex> lk){ codeToBeProtected(); });
     */
    template <typename Callback>
    void runWithActiveClient(Callback&& cb) {
        runWithActiveClient(stdx::unique_lock<stdx::mutex>(_parent->_mutex),
                            std::forward<Callback>(cb));
    }

    template <typename Callback>
    void runWithActiveClient(stdx::unique_lock<stdx::mutex> lk, Callback&& cb) {
        invariant(lk.owns_lock());

        _activeClients++; //活跃客户端请求计数

        const auto guard = MakeGuard([&] {
            invariant(!lk.owns_lock());
            stdx::lock_guard<stdx::mutex> lk(_parent->_mutex);
            _activeClients--;
        });

        {
            decltype(lk) localLk(std::move(lk));
            cb(std::move(localLk));
        }
    }

    SpecificPool(ConnectionPool* parent, const HostAndPort& hostAndPort);
    ~SpecificPool();

    /**
     * Gets a connection from the specific pool. Sinks a unique_lock from the
     * parent to preserve the lock on _mutex
     */
    void getConnection(const HostAndPort& hostAndPort,
                       Milliseconds timeout,
                       stdx::unique_lock<stdx::mutex> lk,
                       GetConnectionCallback cb);

    /**
     * Cascades a failure across existing connections and requests. Invoking
     * this function drops all current connections and fails all current
     * requests with the passed status.
     */
    void processFailure(const Status& status, stdx::unique_lock<stdx::mutex> lk);

    /**
     * Returns a connection to a specific pool. Sinks a unique_lock from the
     * parent to preserve the lock on _mutex
     */
    void returnConnection(ConnectionInterface* connection, stdx::unique_lock<stdx::mutex> lk);

    /**
     * Returns the number of connections currently checked out of the pool.
     */
    size_t inUseConnections(const stdx::unique_lock<stdx::mutex>& lk);

    /**
     * Returns the number of available connections in the pool.
     */
    size_t availableConnections(const stdx::unique_lock<stdx::mutex>& lk);

    /**
     * Returns the number of in progress connections in the pool.
     */
    size_t refreshingConnections(const stdx::unique_lock<stdx::mutex>& lk);

    /**
     * Returns the total number of connections ever created in this pool.
     */
    size_t createdConnections(const stdx::unique_lock<stdx::mutex>& lk);

    /**
     * Returns the total number of connections currently open that belong to
     * this pool. This is the sum of refreshingConnections, availableConnections,
     * and inUseConnections.
     */
    size_t openConnections(const stdx::unique_lock<stdx::mutex>& lk);

private:
    using OwnedConnection = std::unique_ptr<ConnectionInterface>;
    using OwnershipPool = stdx::unordered_map<ConnectionInterface*, OwnedConnection>;
    using LRUOwnershipPool = LRUCache<OwnershipPool::key_type, OwnershipPool::mapped_type>;
    using Request = std::pair<Date_t, GetConnectionCallback>;
    struct RequestComparator {
        bool operator()(const Request& a, const Request& b) {
            return a.first > b.first;
        }
    };

    void addToReady(stdx::unique_lock<stdx::mutex>& lk, OwnedConnection conn);

    void fulfillRequests(stdx::unique_lock<stdx::mutex>& lk);

    void spawnConnections(stdx::unique_lock<stdx::mutex>& lk);

    void shutdown();

    template <typename OwnershipPoolType>
    typename OwnershipPoolType::mapped_type takeFromPool(
        OwnershipPoolType& pool, typename OwnershipPoolType::key_type connPtr);

    OwnedConnection takeFromProcessingPool(ConnectionInterface* connection);

    void updateStateInLock();

private:
    ConnectionPool* const _parent;

    const HostAndPort _hostAndPort;


	/*
	ConnectionPool 针对每 个Shard 机器维护一个连接池，这个连接池包含4个小的池子，用于管理连接的生命周期:
	processingPool： 正在建立的连接
	readyPool：已经建立并且可用的连接
	checkoutPool： 正在使用的连接
	droppedProcessingPool：失败的连接，需要释放

	mongos有一个ASIO的ReactorPool，每个Pool有有若干个连接池，每个连接池负责管理某个特定的mongod的连接。 
	每个连接池又分为 1. readyPool (管理空闲连接) 2. processingPool （管理在创建中/定期检查健康状态的连接） 
	3. checkoutPool （管理正在使用中的连接）

	ConnectionPool 针对每 个Shard 机器维护一个连接池，这个连接池包含4个小的池子，用于管理连接的生命周期。
	
processingPool： 正在建立的连接
readyPool：已经建立并且可用的连接
checkoutPool： 正在使用的连接
droppedProcessingPool：失败的连接，需要释放
连接池管理规则

连接池的总连接会控制在[minConnections, maxConnections]之间，默认为1和无穷大
当需要新建连接时，会发起一个新建连接的异步请求，并把请求放到 processingPool 
当连接建立成功后，会把请求转移到readyPool ，readyPool 里的连接可以直接用于服务新的请求
服务某个请求时会从 readyPool 里取出连接后，会将连接转移到 checkOutPool，标识为正在使用
连接使用完后，会归还到 readyPool；
当遇到请求失败 或 一个网络连接空闲超过1分钟时，会释放连接
	*/
	//mongos到mongod的空闲链接在指定时间内如果没有请求，会cancel删除
    LRUOwnershipPool _readyPool;
    OwnershipPool _processingPool;
    OwnershipPool _droppedProcessingPool;
    OwnershipPool _checkedOutPool;

	//赋值见ConnectionPool::SpecificPool::getConnection
    std::priority_queue<Request, std::vector<Request>, RequestComparator> _requests;

    std::unique_ptr<TimerInterface> _requestTimer;
    Date_t _requestTimerExpiration;
    size_t _activeClients; //赋值见runWithActiveClient
    size_t _generation;
    bool _inFulfillRequests;
    bool _inSpawnConnections;

    size_t _created;

    /**
     * The current state of the pool
     *
     * The pool begins in a running state. Moves to idle when no requests
     * are pending and no connections are checked out. It finally enters
     * shutdown after hostTimeout has passed (and waits there for current
     * refreshes to process out).
     *
     * At any point a new request sets the state back to running and
     * restarts all timers.
     */
    enum class State {
        // The pool is active
        kRunning,

        // No current activity, waiting for hostTimeout to pass
        kIdle,

        // hostTimeout is passed, we're waiting for any processing
        // connections to finish before shutting down
        kInShutdown,
    };

    State _state;
};

constexpr Milliseconds ConnectionPool::kDefaultHostTimeout;
size_t const ConnectionPool::kDefaultMaxConns = std::numeric_limits<size_t>::max();
size_t const ConnectionPool::kDefaultMinConns = 1;
size_t const ConnectionPool::kDefaultMaxConnecting = std::numeric_limits<size_t>::max();
constexpr Milliseconds ConnectionPool::kDefaultRefreshRequirement;
constexpr Milliseconds ConnectionPool::kDefaultRefreshTimeout;

const Status ConnectionPool::kConnectionStateUnknown =
    Status(ErrorCodes::InternalError, "Connection is in an unknown state");

ConnectionPool::ConnectionPool(std::unique_ptr<DependentTypeFactoryInterface> impl,
                               std::string name,
                               Options options)
    : _name(std::move(name)), _options(std::move(options)), _factory(std::move(impl)) {}

ConnectionPool::~ConnectionPool() = default;

void ConnectionPool::dropConnections(const HostAndPort& hostAndPort) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto iter = _pools.find(hostAndPort);

    if (iter == _pools.end())
        return;

    iter->second->runWithActiveClient(std::move(lk), [&](decltype(lk) lk) {
        iter->second->processFailure(
            Status(ErrorCodes::PooledConnectionsDropped, "Pooled connections dropped"),
            std::move(lk));
    });
}

//NetworkInterfaceASIO::startCommand中调用
void ConnectionPool::get(const HostAndPort& hostAndPort,
                         Milliseconds timeout,
                         GetConnectionCallback cb) {
    SpecificPool* pool;

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto iter = _pools.find(hostAndPort);
	//获取hostAndPort对应的连接池
    if (iter == _pools.end()) {
        auto handle = stdx::make_unique<SpecificPool>(this, hostAndPort);
        pool = handle.get();
        _pools[hostAndPort] = std::move(handle);
    } else {
        pool = iter->second.get();
    }

    invariant(pool);

    pool->runWithActiveClient(std::move(lk), [&](decltype(lk) lk) {
		//SpecificPool::getConnection
        pool->getConnection(hostAndPort, timeout, std::move(lk), std::move(cb));
    });
}

void ConnectionPool::appendConnectionStats(ConnectionPoolStats* stats) const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    for (const auto& kv : _pools) {
        HostAndPort host = kv.first;

        auto& pool = kv.second;
        ConnectionStatsPer hostStats{pool->inUseConnections(lk),
                                     pool->availableConnections(lk),
                                     pool->createdConnections(lk),
                                     pool->refreshingConnections(lk)};
        stats->updateStatsForHost(_name, host, hostStats);
    }
}

size_t ConnectionPool::getNumConnectionsPerHost(const HostAndPort& hostAndPort) const {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    auto iter = _pools.find(hostAndPort);
    if (iter != _pools.end()) {
        return iter->second->openConnections(lk);
    }

    return 0;
}

void ConnectionPool::returnConnection(ConnectionInterface* conn) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto iter = _pools.find(conn->getHostAndPort());

    invariant(iter != _pools.end());

    iter->second->runWithActiveClient(std::move(lk), [&](decltype(lk) lk) {
        iter->second->returnConnection(conn, std::move(lk));
    });
}

ConnectionPool::SpecificPool::SpecificPool(ConnectionPool* parent, const HostAndPort& hostAndPort)
    : _parent(parent),
      _hostAndPort(hostAndPort),
      _readyPool(std::numeric_limits<size_t>::max()),
      _requestTimer(parent->_factory->makeTimer()),
      _activeClients(0),
      _generation(0),
      _inFulfillRequests(false),
      _inSpawnConnections(false),
      _created(0),
      _state(State::kRunning) {}

ConnectionPool::SpecificPool::~SpecificPool() {
    DESTRUCTOR_GUARD(_requestTimer->cancelTimeout();)
}

size_t ConnectionPool::SpecificPool::inUseConnections(const stdx::unique_lock<stdx::mutex>& lk) {
    return _checkedOutPool.size();
}

size_t ConnectionPool::SpecificPool::availableConnections(
    const stdx::unique_lock<stdx::mutex>& lk) {
    return _readyPool.size();
}

size_t ConnectionPool::SpecificPool::refreshingConnections(
    const stdx::unique_lock<stdx::mutex>& lk) {
    return _processingPool.size();
}

size_t ConnectionPool::SpecificPool::createdConnections(const stdx::unique_lock<stdx::mutex>& lk) {
    return _created;
}

size_t ConnectionPool::SpecificPool::openConnections(const stdx::unique_lock<stdx::mutex>& lk) {
    return _checkedOutPool.size() + _readyPool.size() + _processingPool.size();
}
//mongos和后端mongod交互:mongos和后端mongod的链接处理在NetworkInterfaceASIO::_connect，mongos转发数据到mongod在NetworkInterfaceASIO::_beginCommunication
//mongos和客户端交互:ServiceEntryPointMongos::handleRequest

//ConnectionPool::get中调用
void ConnectionPool::SpecificPool::getConnection(const HostAndPort& hostAndPort,
                                                 Milliseconds timeout,
                                                 stdx::unique_lock<stdx::mutex> lk,
                                                 GetConnectionCallback cb) { //cb赋值见NetworkInterfaceASIO::startCommand
    if (timeout < Milliseconds(0) || timeout > _parent->_options.refreshTimeout) {
        timeout = _parent->_options.refreshTimeout;
    }

    const auto expiration = _parent->_factory->now() + timeout;

    _requests.push(make_pair(expiration, std::move(cb)));

    updateStateInLock();

    spawnConnections(lk);  //这里面建链接 
    fulfillRequests(lk); // cb真正在这里面执行
}

void ConnectionPool::SpecificPool::returnConnection(ConnectionInterface* connPtr,
                                                    stdx::unique_lock<stdx::mutex> lk) {
    auto needsRefreshTP = connPtr->getLastUsed() + _parent->_options.refreshRequirement;

    auto conn = takeFromPool(_checkedOutPool, connPtr);

    updateStateInLock();

    // Users are required to call indicateSuccess() or indicateFailure() before allowing
    // a connection to be returned. Otherwise, we have entered an unknown state.
    invariant(conn->getStatus() != kConnectionStateUnknown);

    if (conn->getGeneration() != _generation) {
        // If the connection is from an older generation, just return.
        return;
    }

    if (!conn->getStatus().isOK()) {
        // TODO: alert via some callback if the host is bad
        log() << "Ending connection to host " << _hostAndPort << " due to bad connection status; "
              << openConnections(lk) << " connections to that host remain open";
        return;
    }

    auto now = _parent->_factory->now();
    if (needsRefreshTP <= now) {
        // If we need to refresh this connection

        if (_readyPool.size() + _processingPool.size() + _checkedOutPool.size() >=
            _parent->_options.minConnections) {
            // If we already have minConnections, just let the connection lapse
            log() << "Ending idle connection to host " << _hostAndPort
                  << " because the pool meets constraints; " << openConnections(lk)
                  << " connections to that host remain open";
            return;
        }

        _processingPool[connPtr] = std::move(conn);

        // Unlock in case refresh can occur immediately
        lk.unlock();
        connPtr->refresh(
            _parent->_options.refreshTimeout, [this](ConnectionInterface* connPtr, Status status) {
                connPtr->indicateUsed(); //计数

                runWithActiveClient([&](stdx::unique_lock<stdx::mutex> lk) {
                    auto conn = takeFromProcessingPool(connPtr);

                    // If the host and port were dropped, let this lapse
                    if (conn->getGeneration() != _generation) {
                        spawnConnections(lk);
                        return;
                    }

                    // If we're in shutdown, we don't need refreshed connections
                    if (_state == State::kInShutdown)
                        return;

                    // If the connection refreshed successfully, throw it back in
                    // the ready pool
                    if (status.isOK()) {
                        addToReady(lk, std::move(conn));
                        spawnConnections(lk);
                        return;
                    }

                    // If we've exceeded the time limit, start a new connect,
                    // rather than failing all operations.  We do this because the
                    // various callers have their own time limit which is unrelated
                    // to our internal one.
                    if (status.code() == ErrorCodes::NetworkInterfaceExceededTimeLimit) {
                        log() << "Pending connection to host " << _hostAndPort
                              << " did not complete within the connection timeout,"
                              << " retrying with a new connection;" << openConnections(lk)
                              << " connections to that host remain open";
                        spawnConnections(lk);
                        return;
                    }

                    // Otherwise pass the failure on through
                    processFailure(status, std::move(lk));
                });
            });
        lk.lock();
    } else {
        // If it's fine as it is, just put it in the ready queue
        addToReady(lk, std::move(conn));
    }

    updateStateInLock();
}

// Adds a live connection to the ready pool
//和后端链接简历成功后会执行 NetworkInterfaceASIO::AsyncOp::finish
void ConnectionPool::SpecificPool::addToReady(stdx::unique_lock<stdx::mutex>& lk,
                                              OwnedConnection conn) {
    auto connPtr = conn.get();

    // This makes the connection the new most-recently-used connection.
    _readyPool.add(connPtr, std::move(conn));

    // Our strategy for refreshing connections is to check them out and
    // immediately check them back in (which kicks off the refresh logic in
    // returnConnection
    connPtr->setTimeout(_parent->_options.refreshRequirement, [this, connPtr]() {
        OwnedConnection conn;

        runWithActiveClient([&](stdx::unique_lock<stdx::mutex> lk) {
            if (!_readyPool.count(connPtr)) {
                // We've already been checked out. We don't need to refresh
                // ourselves.
                return;
            }

            conn = takeFromPool(_readyPool, connPtr);

            // If we're in shutdown, we don't need to refresh connections
            if (_state == State::kInShutdown)
                return;

            _checkedOutPool[connPtr] = std::move(conn);

            connPtr->indicateSuccess();

            returnConnection(connPtr, std::move(lk));
        });
    });

    fulfillRequests(lk);//链接简历成功后，继续处理客户端请求，例如可能上次的时候没有空闲可用链接，就可以通过这里激活
}

// Drop connections and fail all requests
void ConnectionPool::SpecificPool::processFailure(const Status& status,
                                                  stdx::unique_lock<stdx::mutex> lk) {
    // Bump the generation so we don't reuse any pending or checked out
    // connections
    _generation++;

    // Drop ready connections
    _readyPool.clear();

    // Log something helpful
    log() << "Dropping all pooled connections to " << _hostAndPort
          << " due to failed operation on a connection";

    // Migrate processing connections to the dropped pool
    for (auto&& x : _processingPool) {
        _droppedProcessingPool[x.first] = std::move(x.second);
    }
    _processingPool.clear();

    // Move the requests out so they aren't visible
    // in other threads
    decltype(_requests) requestsToFail;
    {
        using std::swap;
        swap(requestsToFail, _requests);
    }

    // Update state to reflect the lack of requests
    updateStateInLock();

    // Drop the lock and process all of the requests
    // with the same failed status
    lk.unlock();

    while (requestsToFail.size()) {
        requestsToFail.top().second(status);
        requestsToFail.pop();
    }
}

// fulfills as many outstanding requests as possible
//ConnectionPool::SpecificPool::getConnection  ConnectionPool::SpecificPool::addToReady中调用
//从_readyPool链接池获取一个链接后，执行cb回调
void ConnectionPool::SpecificPool::fulfillRequests(stdx::unique_lock<stdx::mutex>& lk) {
    // If some other thread (possibly this thread) is fulfilling requests,
    // don't keep padding the callstack.
    if (_inFulfillRequests)
        return;

    _inFulfillRequests = true;
    auto guard = MakeGuard([&] { _inFulfillRequests = false; });

    while (_requests.size()) {
        // _readyPool is an LRUCache, so its begin() object is the MRU item.
        auto iter = _readyPool.begin();
//说明_readyPool链接池没有可用链接，直接退出，等有可用链接后，会从ConnectionPool::SpecificPool::addToReady继续调用该函数，获取链接转发数据
        if (iter == _readyPool.end()) 
            break;

        // Grab the connection and cancel its timeout
        auto conn = std::move(iter->second);
        _readyPool.erase(iter);
        conn->cancelTimeout(); //mongos到mongod的空闲链接在指定时间内如果没有请求，会cancel删除

        if (!conn->isHealthy()) {
            log() << "dropping unhealthy pooled connection to " << conn->getHostAndPort();

            if (_readyPool.empty()) {
                log() << "after drop, pool was empty, going to spawn some connections";
                // Spawn some more connections to the bad host if we're all out.
                spawnConnections(lk);
            }

            // Drop the bad connection.
            conn.reset();
            // Retry.
            continue;
        }

        // Grab the request and callback
        //cb赋值见NetworkInterfaceASIO::startCommand中的nextStep
        auto cb = std::move(_requests.top().second);
        _requests.pop();

        auto connPtr = conn.get();

        // check out the connection
        //把从_readyPool获取的这个链接转给_checkedOutPool
        _checkedOutPool[connPtr] = std::move(conn);

        updateStateInLock();

        // pass it to the user
        connPtr->resetToUnknown();
        lk.unlock();
        cb(ConnectionHandle(connPtr, ConnectionHandleDeleter(_parent)));
        lk.lock();
    }
}

//mongos和后端mongod交互:mongos和后端mongod的链接处理在NetworkInterfaceASIO::_connect，mongos转发数据到mongod在NetworkInterfaceASIO::_beginCommunication
//mongos和客户端交互:ServiceEntryPointMongos::handleRequest


// spawn enough connections to satisfy open requests and minpool, while
// honoring maxpool  mongs建立和后端mongos的链接
void ConnectionPool::SpecificPool::spawnConnections(stdx::unique_lock<stdx::mutex>& lk) {
    // If some other thread (possibly this thread) is spawning connections,
    // don't keep padding the callstack.
    if (_inSpawnConnections)
        return;

	//2019-03-07T11:17:40.056+0800 I ASIO     [conn----yangtest1] ddd test...........ConnectionPool::SpecificPool::spawnConnections
	//log() << "ddd test...........ConnectionPool::SpecificPool::spawnConnections";
    _inSpawnConnections = true;
    auto guard = MakeGuard([&] { _inSpawnConnections = false; });

    // We want minConnections <= outstanding requests <= maxConnections
    auto target = [&] {
        return std::max(
            _parent->_options.minConnections,
            std::min(_requests.size() + _checkedOutPool.size(), _parent->_options.maxConnections));
    };

    // While all of our inflight connections are less than our target
    while ((_readyPool.size() + _processingPool.size() + _checkedOutPool.size() < target()) &&
           (_processingPool.size() < _parent->_options.maxConnecting)) {
        std::unique_ptr<ConnectionPool::ConnectionInterface> handle;
        try {
            // make a new connection and put it in processing
            // ASIOImpl::makeConnection  获取ASIOConnection类
            handle = _parent->_factory->makeConnection(_hostAndPort, _generation);
        } catch (std::system_error& e) {
            severe() << "Failed to construct a new connection object: " << e.what();
            fassertFailed(40336);
        }

        auto connPtr = handle.get(); //获取ASIOConnection类
        _processingPool[connPtr] = std::move(handle);

        ++_created;

        // Run the setup callback
        lk.unlock();
		//开始键连接 ASIOConnection::setup
        connPtr->setup(
            _parent->_options.refreshTimeout, 
            //链接建立成功后执行或者后端应答数据后执行，由 NetworkInterfaceASIO::AsyncOp::finish调用
            [this](ConnectionInterface* connPtr, Status status) {
                connPtr->indicateUsed();

                runWithActiveClient([&](stdx::unique_lock<stdx::mutex> lk) {
                    auto conn = takeFromProcessingPool(connPtr);

                    if (conn->getGeneration() != _generation) {
                        // If the host and port was dropped, let the
                        // connection lapse
                        spawnConnections(lk);
                    } else if (status.isOK()) { //链接建立成功，放入_readyPool池
                        addToReady(lk, std::move(conn));
                        spawnConnections(lk);
                    } else if (status.code() == ErrorCodes::NetworkInterfaceExceededTimeLimit) {
                        // If we've exceeded the time limit, restart the connect, rather than
                        // failing all operations.  We do this because the various callers
                        // have their own time limit which is unrelated to our internal one.
                        spawnConnections(lk);
                    } else {
                        // If the setup failed, cascade the failure edge
                        processFailure(status, std::move(lk));
                    }
                });
            });
        // Note that this assumes that the refreshTimeout is sound for the
        // setupTimeout

        lk.lock();
    }
}

// Called every second after hostTimeout until all processing connections reap
void ConnectionPool::SpecificPool::shutdown() {
    stdx::unique_lock<stdx::mutex> lk(_parent->_mutex);

    // We're racing:
    //
    // Thread A (this thread)
    //   * Fired the shutdown timer
    //   * Came into shutdown() and blocked
    //
    // Thread B (some new consumer)
    //   * Requested a new connection
    //   * Beat thread A to the mutex
    //   * Cancelled timer (but thread A already made it in)
    //   * Set state to running
    //   * released the mutex
    //
    // So we end up in shutdown, but with kRunning.  If we're here we raced and
    // we should just bail.
    if (_state == State::kRunning) {
        return;
    }

    _state = State::kInShutdown;

    // If we have processing connections, wait for them to finish or timeout
    // before shutdown
    if (_processingPool.size() || _droppedProcessingPool.size() || _activeClients) {
        _requestTimer->setTimeout(Seconds(1), [this]() { shutdown(); });

        return;
    }

    invariant(_requests.empty());
    invariant(_checkedOutPool.empty());

    _parent->_pools.erase(_hostAndPort);
}

template <typename OwnershipPoolType>
typename OwnershipPoolType::mapped_type ConnectionPool::SpecificPool::takeFromPool(
    OwnershipPoolType& pool, typename OwnershipPoolType::key_type connPtr) {
    auto iter = pool.find(connPtr);
    invariant(iter != pool.end());

    auto conn = std::move(iter->second);
    pool.erase(iter);
    return conn;
}

ConnectionPool::SpecificPool::OwnedConnection ConnectionPool::SpecificPool::takeFromProcessingPool(
    ConnectionInterface* connPtr) {
    if (_processingPool.count(connPtr))
        return takeFromPool(_processingPool, connPtr);

    return takeFromPool(_droppedProcessingPool, connPtr);
}


// Updates our state and manages the request timer
void ConnectionPool::SpecificPool::updateStateInLock() {
    if (_requests.size()) {
        // We have some outstanding requests, we're live

        // If we were already running and the timer is the same as it was
        // before, nothing to do
        if (_state == State::kRunning && _requestTimerExpiration == _requests.top().first)
            return;

        _state = State::kRunning;

        _requestTimer->cancelTimeout();

        _requestTimerExpiration = _requests.top().first;

        auto timeout = _requests.top().first - _parent->_factory->now();

        // We set a timer for the most recent request, then invoke each timed
        // out request we couldn't service
        _requestTimer->setTimeout(timeout, [this]() {
            runWithActiveClient([&](stdx::unique_lock<stdx::mutex> lk) {
                auto now = _parent->_factory->now();

                while (_requests.size()) {
                    auto& x = _requests.top();

                    if (x.first <= now) {
                        auto cb = std::move(x.second);
                        _requests.pop();

                        lk.unlock();
                        cb(Status(ErrorCodes::NetworkInterfaceExceededTimeLimit,
                                  "Couldn't get a connection within the time limit"));
                        lk.lock();
                    } else {
                        break;
                    }
                }

                updateStateInLock();
            });
        });
    } else if (_checkedOutPool.size()) {
        // If we have no requests, but someone's using a connection, we just
        // hang around until the next request or a return

        _requestTimer->cancelTimeout();
        _state = State::kRunning;
        _requestTimerExpiration = _requestTimerExpiration.max();
    } else {
        // If we don't have any live requests and no one has checked out connections

        // If we used to be idle, just bail
        if (_state == State::kIdle)
            return;

        _state = State::kIdle;

        _requestTimer->cancelTimeout();

        _requestTimerExpiration = _parent->_factory->now() + _parent->_options.hostTimeout;

        auto timeout = _parent->_options.hostTimeout;

        // Set the shutdown timer
        _requestTimer->setTimeout(timeout, [this]() { shutdown(); });
    }
}

}  // namespace executor
}  // namespace mongo
