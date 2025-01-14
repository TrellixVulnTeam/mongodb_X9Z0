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

#include "mongo/platform/basic.h"

#include "mongo/executor/task_executor_pool.h"

#include <algorithm>

#include "mongo/db/server_parameters.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/task_executor.h"
#include "mongo/util/processinfo.h"

namespace mongo {
namespace executor {

// If less than or equal to 0, the suggested pool size will be determined by the number of cores. If
// set to a particular positive value, this will be used as the pool size.
MONGO_EXPORT_SERVER_PARAMETER(taskExecutorPoolSize, int, 1);//0); ddd add change

//makeShardingTaskExecutorPool调用
size_t TaskExecutorPool::getSuggestedPoolSize() {
    auto poolSize = taskExecutorPoolSize.load();
    if (poolSize > 0) {
        return poolSize;
    }

    ProcessInfo p;
    unsigned numCores = p.getNumCores();

    // Never suggest a number outside the range [4, 64].
    return std::max(4U, std::min(64U, numCores));
}

//initializeGlobalShardingState->TaskExecutorPool::startup->ShardingTaskExecutor::startup->ThreadPoolTaskExecutor::startup
void TaskExecutorPool::startup() {
    invariant(!_executors.empty());
    invariant(_fixedExecutor);

	//参考makeShardingTaskExecutorPool

	//对应NetworkInterfaceASIO-ShardRegistry线程
    _fixedExecutor->startup(); //ShardingTaskExecutor::startup->ThreadPoolTaskExecutor::startup
    for (auto&& exec : _executors) {
		//对应NetworkInterfaceASIO-TaskExecutorPool-线程
        exec->startup(); //ShardingTaskExecutor::startup->ThreadPoolTaskExecutor::startup
    }
}

void TaskExecutorPool::shutdownAndJoin() {
    _fixedExecutor->shutdown();
    _fixedExecutor->join();
    for (auto&& exec : _executors) {
        exec->shutdown();
        exec->join();
    }
}

//见makeShardingTaskExecutorPool
void TaskExecutorPool::addExecutors(std::vector<std::unique_ptr<TaskExecutor>> executors,
                                    std::unique_ptr<TaskExecutor> fixedExecutor) {
    invariant(_executors.empty());
    invariant(fixedExecutor);
    invariant(!_fixedExecutor);

    _fixedExecutor = std::move(fixedExecutor);
    _executors = std::move(executors);
}

//轮询获取_executors[i]
TaskExecutor* TaskExecutorPool::getArbitraryExecutor() {
    invariant(!_executors.empty());
    uint64_t idx = (_counter.fetchAndAdd(1) % _executors.size());
    return _executors[idx].get();
}

TaskExecutor* TaskExecutorPool::getFixedExecutor() {
    invariant(_fixedExecutor);
    return _fixedExecutor.get();
}

void TaskExecutorPool::appendConnectionStats(ConnectionPoolStats* stats) const {
    // Get stats from our fixed executor.
    _fixedExecutor->appendConnectionStats(stats);
    // Get stats from our pooled executors.
    for (auto&& executor : _executors) {
        executor->appendConnectionStats(stats);
    }
}

}  // namespace executor
}  // namespace mongo
