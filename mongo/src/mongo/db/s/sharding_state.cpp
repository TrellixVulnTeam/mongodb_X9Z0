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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/sharding_state.h"

#include "mongo/base/init.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_initialization_mongod.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_thread_pool.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/metadata/config_server_metadata.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/sharding_network_connection_hook.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_initialization.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>

namespace mongo {

using std::shared_ptr;
using std::string;
using std::vector;

using CallbackArgs = executor::TaskExecutor::CallbackArgs;

namespace {

const auto getShardingState = ServiceContext::declareDecoration<ShardingState>();

/**
 * Updates the config server field of the shardIdentity document with the given connection string
 * if setName is equal to the config server replica set name.
 *
 * Note: This is intended to be used on a new thread that hasn't called Client::initThread.
 * One example use case is for the ReplicaSetMonitor asynchronous callback when it detects changes
 * to replica set membership.
 */
void updateShardIdentityConfigStringCB(const string& setName, const string& newConnectionString) {
    auto configsvrConnStr = grid.shardRegistry()->getConfigServerConnectionString();
    if (configsvrConnStr.getSetName() != setName) {
        // Ignore all change notification for other sets that are not the config server.
        return;
    }

    Client::initThread("updateShardIdentityConfigConnString");
    auto uniqOpCtx = getGlobalServiceContext()->makeOperationContext(&cc());

    auto status = ShardingState::get(uniqOpCtx.get())
                      ->updateShardIdentityConfigString(uniqOpCtx.get(), newConnectionString);
    if (!status.isOK() && !ErrorCodes::isNotMasterError(status.code())) {
        warning() << "error encountered while trying to update config connection string to "
                  << newConnectionString << causedBy(redact(status));
    }
}

}  // namespace

ShardingState::ShardingState()
    : _chunkSplitter(stdx::make_unique<ChunkSplitter>()),
      _initializationState(static_cast<uint32_t>(InitializationState::kNew)),
      _initializationStatus(Status(ErrorCodes::InternalError, "Uninitialized value")),
      _globalInit(&initializeGlobalShardingStateForMongod) {}

ShardingState::~ShardingState() = default;

ShardingState* ShardingState::get(ServiceContext* serviceContext) {
    return &getShardingState(serviceContext);
}

ShardingState* ShardingState::get(OperationContext* operationContext) {
    return ShardingState::get(operationContext->getServiceContext());
}

bool ShardingState::enabled() const {
    return _getInitializationState() == InitializationState::kInitialized;
}

void ShardingState::setEnabledForTest(const std::string& shardName) {
    _setInitializationState(InitializationState::kInitialized);
    _shardName = shardName;
}

Status ShardingState::canAcceptShardedCommands() const {
	LOG(1) << "ddd test ...............serverGlobalParams.clusterRole:" << (int)serverGlobalParams.clusterRole;
    if (serverGlobalParams.clusterRole != ClusterRole::ShardServer) {
        return {ErrorCodes::NoShardingEnabled,
                "Cannot accept sharding commands if not started with --shardsvr"};
    } else if (!enabled()) {
        return {ErrorCodes::ShardingStateNotInitialized,
                "Cannot accept sharding commands if sharding state has not "
                "been initialized with a shardIdentity document"};
    } else {
        return Status::OK();
    }
}

ConnectionString ShardingState::getConfigServer(OperationContext* opCtx) {
    invariant(enabled());
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return Grid::get(opCtx)->shardRegistry()->getConfigServerConnectionString();
}

string ShardingState::getShardName() {
    invariant(enabled());
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _shardName;
}

void ShardingState::shutDown(OperationContext* opCtx) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (enabled()) {
        Grid::get(opCtx)->getExecutorPool()->shutdownAndJoin();
        Grid::get(opCtx)->catalogClient()->shutDown(opCtx);
    }
}

Status ShardingState::updateConfigServerOpTimeFromMetadata(OperationContext* opCtx) {
    if (!enabled()) {
        // Nothing to do if sharding state has not been initialized.
        return Status::OK();
    }

    boost::optional<repl::OpTime> opTime = rpc::ConfigServerMetadata::get(opCtx).getOpTime();
    if (opTime) {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                    ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized to update config opTime");
        }

        Grid::get(opCtx)->advanceConfigOpTime(*opTime);
    }

    return Status::OK();
}

CollectionShardingState* ShardingState::getNS(const std::string& ns, OperationContext* opCtx) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    CollectionShardingStateMap::iterator it = _collections.find(ns);
    if (it == _collections.end()) {
        auto inserted =
            _collections.insert(make_pair(ns,
                                          stdx::make_unique<CollectionShardingState>(
                                              opCtx->getServiceContext(), NamespaceString(ns))));
        invariant(inserted.second);
        it = std::move(inserted.first);
    }

    return it->second.get();
}

ChunkSplitter* ShardingState::getChunkSplitter() {
    return _chunkSplitter.get();
}

void ShardingState::initiateChunkSplitter() {
    _chunkSplitter->initiateChunkSplitter();
}

void ShardingState::interruptChunkSplitter() {
    _chunkSplitter->interruptChunkSplitter();
}

void ShardingState::markCollectionsNotShardedAtStepdown() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    for (auto& coll : _collections) {
        auto& css = coll.second;
        css->markNotShardedAtStepdown();
    }
}

void ShardingState::setGlobalInitMethodForTest(GlobalInitFunc func) {
    _globalInit = func;
}

//shard mongod增、删、改对应版本检测：performSingleUpdateOp->assertCanWrite_inlock
//shard mongod读对应version版本检测：FindCmd::run->assertCanWrite_inlock


//SetShardVersion::run   execCommandDatabase(shard server mongod)
//(insertBatchAndHandleErrors performUpdates)->handleError中调用，也就是shard server
//
//刷新元数据信息，例如表对应chunk路由信息等
Status ShardingState::onStaleShardVersion(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          const ChunkVersion& expectedVersion) {
    invariant(!opCtx->getClient()->isInDirectClient());
    invariant(!opCtx->lockState()->isLocked());
    invariant(enabled());

    LOG(2) << "metadata refresh requested for " << nss.ns() << " at shard version "
           << expectedVersion;

    // Ensure any ongoing migrations have completed
    auto& oss = OperationShardingState::get(opCtx);
    oss.waitForMigrationCriticalSectionSignal(opCtx);

    const auto collectionShardVersion = [&] {
        // Fast path - check if the requested version is at a higher version than the current
        // metadata version or a different epoch before verifying against config server
        AutoGetCollection autoColl(opCtx, nss, MODE_IS);
        const auto currentMetadata = CollectionShardingState::get(opCtx, nss)->getMetadata();
        if (currentMetadata) {
            return currentMetadata->getShardVersion();
        }

        return ChunkVersion::UNSHARDED();
    }();

	//本地的shardversion和代理mongos发送过来的做比较，如果本地缓存的版本号比mongos的高，则啥也不做
	//不用刷新元数据
    if (collectionShardVersion.epoch() == expectedVersion.epoch() &&
        collectionShardVersion >= expectedVersion) {
        // Don't need to remotely reload if we're in the same epoch and the requested version is
        // smaller than the one we know about. This means that the remote side is behind.
        return Status::OK();
    }

    try {
		//否则更新本地元数据信息
        _refreshMetadata(opCtx, nss);
        return Status::OK();
    } catch (const DBException& ex) {
        log() << "Failed to refresh metadata for collection" << nss << causedBy(redact(ex));
        return ex.toStatus();
    }
}

//MigrationSourceManager::MigrationSourceManager调用
//MigrationSourceManager::commitChunkMetadataOnConfig
//RecvChunkStartCommand::errmsgRun
//FlushRoutingTableCacheUpdates::run
//获取nss对应的最新ChunkVersion
Status ShardingState::refreshMetadataNow(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         ChunkVersion* latestShardVersion) {
    try {
		//更新元数据信息，返回chunk版本信息
        *latestShardVersion = _refreshMetadata(opCtx, nss);
        return Status::OK();
    } catch (const DBException& ex) {
        return ex.toStatus();
    }
}

// NOTE: This method will be called inside a database lock so it should never take any database
// locks, perform I/O, or any long running operations.
Status ShardingState::initializeFromShardIdentity(OperationContext* opCtx,
                                                  const ShardIdentityType& shardIdentity) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);
    invariant(opCtx->lockState()->isLocked());

    Status validationStatus = shardIdentity.validate();
    if (!validationStatus.isOK()) {
        return Status(
            validationStatus.code(),
            str::stream()
                << "Invalid shard identity document found when initializing sharding state: "
                << validationStatus.reason());
    }

    log() << "initializing sharding state with: " << shardIdentity;

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto configSvrConnStr = shardIdentity.getConfigsvrConnString();

    if (enabled()) {
        invariant(!_shardName.empty());
        fassert(40372, _shardName == shardIdentity.getShardName());

        auto prevConfigsvrConnStr =
            Grid::get(opCtx)->shardRegistry()->getConfigServerConnectionString();
        invariant(prevConfigsvrConnStr.type() == ConnectionString::SET);
        fassert(40373, prevConfigsvrConnStr.getSetName() == configSvrConnStr.getSetName());

        invariant(_clusterId.isSet());
        fassert(40374, _clusterId == shardIdentity.getClusterId());

        return Status::OK();
    }

    if (_getInitializationState() == InitializationState::kError) {
        return {ErrorCodes::ManualInterventionRequired,
                str::stream() << "Server's sharding metadata manager failed to initialize and will "
                                 "remain in this state until the instance is manually reset"
                              << causedBy(_initializationStatus)};
    }

    ShardedConnectionInfo::addHook(opCtx->getServiceContext());

    try {
        Status status = _globalInit(opCtx, configSvrConnStr, generateDistLockProcessId(opCtx));
        if (status.isOK()) {
            ReplicaSetMonitor::setSynchronousConfigChangeHook(
                &ShardRegistry::replicaSetChangeShardRegistryUpdateHook);
            ReplicaSetMonitor::setAsynchronousConfigChangeHook(&updateShardIdentityConfigStringCB);

            // Determine primary/secondary/standalone state in order to properly initialize sharding
            // components.
            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            bool isReplSet =
                replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;
            bool isStandaloneOrPrimary =
                !isReplSet || (repl::ReplicationCoordinator::get(opCtx)->getMemberState() ==
                               repl::MemberState::RS_PRIMARY);

            CatalogCacheLoader::get(opCtx).initializeReplicaSetRole(isStandaloneOrPrimary);

            _chunkSplitter->setReplicaSetMode(isStandaloneOrPrimary);

            log() << "initialized sharding components for "
                  << (isStandaloneOrPrimary ? "primary" : "secondary") << " node.";
            _setInitializationState(InitializationState::kInitialized);
        } else {
            log() << "failed to initialize sharding components" << causedBy(status);
            _initializationStatus = status;
            _setInitializationState(InitializationState::kError);
        }
        _shardName = shardIdentity.getShardName();
        _clusterId = shardIdentity.getClusterId();

        return status;
    } catch (const DBException& ex) {
        auto errorStatus = ex.toStatus();
        _initializationStatus = errorStatus;
        _setInitializationState(InitializationState::kError);
        return errorStatus;
    }

    MONGO_UNREACHABLE;
}

ShardingState::InitializationState ShardingState::_getInitializationState() const {
    return static_cast<InitializationState>(_initializationState.load());
}

void ShardingState::_setInitializationState(InitializationState newState) {
    _initializationState.store(static_cast<uint32_t>(newState));
}

StatusWith<bool> ShardingState::initializeShardingAwarenessIfNeeded(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());

    // In sharded readOnly mode, we ignore the shardIdentity document on disk and instead *require*
    // a shardIdentity document to be passed through --overrideShardIdentity.
    if (storageGlobalParams.readOnly) {
        if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
            if (serverGlobalParams.overrideShardIdentity.isEmpty()) {
                return {ErrorCodes::InvalidOptions,
                        "If started with --shardsvr in queryableBackupMode, a shardIdentity "
                        "document must be provided through --overrideShardIdentity"};
            }
            auto swOverrideShardIdentity =
                ShardIdentityType::fromBSON(serverGlobalParams.overrideShardIdentity);
            if (!swOverrideShardIdentity.isOK()) {
                return swOverrideShardIdentity.getStatus();
            }
            {
                // Global lock is required to call initializeFromShardIdenetity().
                Lock::GlobalWrite lk(opCtx);
                auto status =
                    initializeFromShardIdentity(opCtx, swOverrideShardIdentity.getValue());
                if (!status.isOK()) {
                    return status;
                }
            }
            return true;
        } else {
            // Error if --overrideShardIdentity is used but *not* started with --shardsvr.
            if (!serverGlobalParams.overrideShardIdentity.isEmpty()) {
                return {
                    ErrorCodes::InvalidOptions,
                    str::stream()
                        << "Not started with --shardsvr, but a shardIdentity document was provided "
                           "through --overrideShardIdentity: "
                        << serverGlobalParams.overrideShardIdentity};
            }
            return false;
        }
    }
    // In sharded *non*-readOnly mode, error if --overrideShardIdentity is provided. Use the
    // shardIdentity document on disk if one exists, but it is okay if no shardIdentity document is
    // provided at all (sharding awareness will be initialized when a shardIdentity document is
    // inserted).
    else {
        if (!serverGlobalParams.overrideShardIdentity.isEmpty()) {
            return {
                ErrorCodes::InvalidOptions,
                str::stream() << "--overrideShardIdentity is only allowed in sharded "
                                 "queryableBackupMode. If not in queryableBackupMode, you can edit "
                                 "the shardIdentity document by starting the server *without* "
                                 "--shardsvr, manually updating the shardIdentity document in the "
                              << NamespaceString::kServerConfigurationNamespace.toString()
                              << " collection, and restarting the server with --shardsvr."};
        }

        // Load the shardIdentity document from disk.
        BSONObj shardIdentityBSON;
        bool foundShardIdentity = false;
        try {
            AutoGetCollection autoColl(
                opCtx, NamespaceString::kServerConfigurationNamespace, MODE_IS);
            foundShardIdentity = Helpers::findOne(opCtx,
                                                  autoColl.getCollection(),
                                                  BSON("_id" << ShardIdentityType::IdName),
                                                  shardIdentityBSON);
        } catch (const DBException& ex) {
            return ex.toStatus();
        }

        if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
            if (!foundShardIdentity) {
                warning() << "Started with --shardsvr, but no shardIdentity document was found on "
                             "disk in "
                          << NamespaceString::kServerConfigurationNamespace
                          << ". This most likely means this server has not yet been added to a "
                             "sharded cluster.";
                return false;
            }

            invariant(!shardIdentityBSON.isEmpty());

            auto swShardIdentity = ShardIdentityType::fromBSON(shardIdentityBSON);
            if (!swShardIdentity.isOK()) {
                return swShardIdentity.getStatus();
            }
            {
                // Global lock is required to call initializeFromShardIdenetity().
                Lock::GlobalWrite lk(opCtx);
                auto status = initializeFromShardIdentity(opCtx, swShardIdentity.getValue());
                if (!status.isOK()) {
                    return status;
                }
            }
            return true;
        } else {
            // Warn if a shardIdentity document is found on disk but *not* started with --shardsvr.
            if (!shardIdentityBSON.isEmpty()) {
                warning() << "Not started with --shardsvr, but a shardIdentity document was found "
                             "on disk in "
                          << NamespaceString::kServerConfigurationNamespace << ": "
                          << shardIdentityBSON;
            }
            return false;
        }
    }
}

//ShardingState::refreshMetadataNow  ShardingState::onStaleShardVersion调用  
//更新元数据信息，返回chunk版本信息
ChunkVersion ShardingState::_refreshMetadata(OperationContext* opCtx, const NamespaceString& nss) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(enabled());

	//获取分片ID信息
    const ShardId shardId = getShardName();

	//shardId合法性检查
    uassert(ErrorCodes::NotYetInitialized,
            str::stream() << "Cannot refresh metadata for " << nss.ns()
                          << " before shard name has been set",
            shardId.isValid());

	//获取集合chunks路由信息  routingInfo为CachedCollectionRoutingInfo类型
	//通过这里把chunks路由信息和metadata元数据信息关联起来
	//(元数据刷新，通过这里把chunks路由信息和metadata元数据信息关联起来)流程： 
	//    ShardingState::_refreshMetadata->CatalogCache::getShardedCollectionRoutingInfoWithRefresh
    const auto routingInfo = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(opCtx, nss));

	//获取ChunkManager   从cfg获取的新路由信息
	const auto cm = routingInfo.cm();//CachedCollectionRoutingInfo::cm

	//没有ChunkManager，说明没有启用分片功能
    if (!cm) {
        // No chunk manager, so unsharded.

        // Exclusive collection lock needed since we're now changing the metadata
        AutoGetCollection autoColl(opCtx, nss, MODE_IX, MODE_X);

        auto css = CollectionShardingState::get(opCtx, nss);
		//CollectionShardingState::refreshMetadata
        css->refreshMetadata(opCtx, nullptr);

        return ChunkVersion::UNSHARDED();
    }

    {
        AutoGetCollection autoColl(opCtx, nss, MODE_IS);
		//获取CollectionShardingState，对应当前mongos缓存的路由相关信息
        auto css = CollectionShardingState::get(opCtx, nss);

        // We already have newer version  
        
        //css对应本地的，cm对应从cfg刚获取的最新的路由信息

		//ChunkVersion如果没有发生变化，则不用更新元数据，直接返回
        //CollectionShardingState::getMetadata
        if (css->getMetadata() &&
			//ScopedCollectionMetadata::getMetadata  CollectionMetadata::getCollVersion
            css->getMetadata()->getCollVersion().epoch() == cm->getVersion().epoch() &&
            css->getMetadata()->getCollVersion() >= cm->getVersion()) {
            LOG(1) << "Skipping refresh of metadata for " << nss << " "
                   << css->getMetadata()->getCollVersion() << " with an older " << cm->getVersion();
			//直接返回ChunkVersion
			return css->getMetadata()->getShardVersion();
        }
    }

    // Exclusive collection lock needed since we're now changing the metadata
    AutoGetCollection autoColl(opCtx, nss, MODE_IX, MODE_X);

    auto css = CollectionShardingState::get(opCtx, nss);

    // We already have newer version
    //css对应本地的，cm对应从cfg刚获取的最新的路由信息，比较本地版本信息和从cfg获取到的cm最新版本信息是否一致
    if (css->getMetadata() && //CollectionShardingState::getMetadata
    	//ScopedCollectionMetadata::getMetadata  CollectionMetadata::getCollVersion
        css->getMetadata()->getCollVersion().epoch() == cm->getVersion().epoch() &&
        css->getMetadata()->getCollVersion() >= cm->getVersion()) {
        LOG(1) << "Skipping refresh of metadata for " << nss << " "
               << css->getMetadata()->getCollVersion() << " with an older " << cm->getVersion();
        return css->getMetadata()->getShardVersion();
    }


	//注意ChunkManager和CollectionMetadata的关系，参考ShardingState::_refreshMetadata

	
	//把最新获取到的路由信息cm和shardId构造对应CollectionMetadata
    std::unique_ptr<CollectionMetadata> newCollectionMetadata =
        stdx::make_unique<CollectionMetadata>(cm, shardId);

	//跟新元数据版本信息  CollectionShardingState::refreshMetadata
	//把最新获取到的
    css->refreshMetadata(opCtx, std::move(newCollectionMetadata));

	//返回新的版本信息
    return css->getMetadata()->getShardVersion();
}

//MoveChunkCommand::run调用  
//源分片收到mongos发送过来的moveChunk命令后，设置源分片处于迁移状态，保证源分片同一时刻每个表只会迁移一个chunk
StatusWith<ScopedRegisterDonateChunk> ShardingState::registerDonateChunk(
    const MoveChunkRequest& args) {
    //ActiveMigrationsRegistry::registerDonateChunk
    return _activeMigrationsRegistry.registerDonateChunk(args);
}


StatusWith<ScopedRegisterReceiveChunk> ShardingState::registerReceiveChunk(
    const NamespaceString& nss, const ChunkRange& chunkRange, const ShardId& fromShardId) {
	//ActiveMigrationsRegistry::registerReceiveChunk
	return _activeMigrationsRegistry.registerReceiveChunk(nss, chunkRange, fromShardId);
}

boost::optional<NamespaceString> ShardingState::getActiveDonateChunkNss() {
    return _activeMigrationsRegistry.getActiveDonateChunkNss();
}

BSONObj ShardingState::getActiveMigrationStatusReport(OperationContext* opCtx) {
    return _activeMigrationsRegistry.getActiveMigrationStatusReport(opCtx);
}

//ShardingStateCmd::run调用
/*
xx:PRIMARY> db.runCommand({ shardingState: 1 })
"enabled" : true,
"configServer" : "xx/10.xx.xx.238:20014,10.xx.xx.234:20009,10.xx.xx.91:20016",
"shardName" : "xx_shard_1",
"clusterId" : ObjectId("5e4acb7a658f0a4a5029f452"),
"versions" : {
		"cloud_track.system.drop.1622998800i482t18.dailyCloudOperateInfo_01" : Timestamp(0, 0),
		"config.system.drop.1622826001i6304t18.cache.chunks.cloud_track.dailyCloudOperateInfo_30" : Timestamp(0, 0),
		"cloud_track.system.drop.1622826000i5598t18.dailyCloudOperateInfo_30" : Timestamp(0, 0),
		"config.system.drop.1622653201i5382t18.cache.chunks.cloud_track.dailyCloudOperateInfo_28" : Timestamp(0, 0),
		"config.system.drop.1622566801i4563t18.cache.chunks.cloud_track.dailyCloudOperateInfo_27" : Timestamp(0, 0),
		"config.system.drop.1622480401i6387t18.cache.chunks.cloud_track.dailyCloudOperateInfo_26" : Timestamp(0, 0),
		"cloud_track.system.drop.1622480400i723t18.dailyCloudOperateInfo_26" : Timestamp(0, 0),
		"cloud_track.system.drop.1622307600i100t18.dailyCloudOperateInfo_24" : Timestamp(0, 0),
		"cloud_track.system.drop.1622221200i533t18.dailyCloudOperateInfo_23" : Timestamp(0, 0),
		"config.system.drop.1621789201i5341t18.cache.chunks.cloud_track.dailyCloudOperateInfo_18" : Timestamp(0, 0),
		"config.system.drop.1621702801i5647t18.cache.chunks.cloud_track.dailyCloudOperateInfo_17" : Timestamp(0, 0),
		"config.system.drop.1621616401i7264t18.cache.chunks.cloud_track.dailyCloudOperateInfo_16" : Timestamp(0, 0),

*/ //只有mongod支持，获取某个分片上的表版本信息
void ShardingState::appendInfo(OperationContext* opCtx, BSONObjBuilder& builder) {
    const bool isEnabled = enabled();
    builder.appendBool("enabled", isEnabled);
    if (!isEnabled)
        return;

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    builder.append("configServer",
                   Grid::get(opCtx)->shardRegistry()->getConfigServerConnectionString().toString());
    builder.append("shardName", _shardName);
    builder.append("clusterId", _clusterId);

    BSONObjBuilder versionB(builder.subobjStart("versions"));
    for (CollectionShardingStateMap::const_iterator it = _collections.begin();
         it != _collections.end();
         ++it) {
        ScopedCollectionMetadata metadata = it->second->getMetadata();
        if (metadata) {
            versionB.appendTimestamp(it->first, metadata->getShardVersion().toLong());
        } else {
            versionB.appendTimestamp(it->first, ChunkVersion::UNSHARDED().toLong());
        }
    }

    versionB.done();
}

bool ShardingState::needCollectionMetadata(OperationContext* opCtx, const string& ns) {
    if (!enabled())
        return false;

    Client* client = opCtx->getClient();

    // Shard version information received from mongos may either by attached to the Client or
    // directly to the OperationContext.
    return ShardedConnectionInfo::get(client, false) ||
        OperationShardingState::get(opCtx).hasShardVersion();
}

Status ShardingState::updateShardIdentityConfigString(OperationContext* opCtx,
                                                      const std::string& newConnectionString) {
    BSONObj updateObj(ShardIdentityType::createConfigServerUpdateObject(newConnectionString));

    UpdateRequest updateReq(NamespaceString::kServerConfigurationNamespace);
    updateReq.setQuery(BSON("_id" << ShardIdentityType::IdName));
    updateReq.setUpdates(updateObj);
    UpdateLifecycleImpl updateLifecycle(NamespaceString::kServerConfigurationNamespace);
    updateReq.setLifecycle(&updateLifecycle);

    try {
        AutoGetOrCreateDb autoDb(
            opCtx, NamespaceString::kServerConfigurationNamespace.db(), MODE_X);

        auto result = update(opCtx, autoDb.getDb(), updateReq);
        if (result.numMatched == 0) {
            warning() << "failed to update config string of shard identity document because "
                      << "it does not exist. This shard could have been removed from the cluster";
        } else {
            LOG(2) << "Updated config server connection string in shardIdentity document to"
                   << newConnectionString;
        }
    } catch (const DBException& exception) {
        return exception.toStatus();
    }

    return Status::OK();
}

executor::TaskExecutor* ShardingState::getRangeDeleterTaskExecutor() {
    stdx::lock_guard<stdx::mutex> lk(_rangeDeleterExecutor.lock);
    if (_rangeDeleterExecutor.taskExecutor.get() == nullptr) {
        static const char kExecName[] = "NetworkInterfaceCollectionRangeDeleter-TaskExecutor";
        auto net = executor::makeNetworkInterface(kExecName);
        auto pool = stdx::make_unique<executor::NetworkInterfaceThreadPool>(net.get());
        _rangeDeleterExecutor.taskExecutor =
            stdx::make_unique<executor::ThreadPoolTaskExecutor>(std::move(pool), std::move(net));
        _rangeDeleterExecutor.taskExecutor->startup();
    }
    return _rangeDeleterExecutor.taskExecutor.get();
}

ShardingState::RangeDeleterExecutor::~RangeDeleterExecutor() {
    if (taskExecutor) {
        taskExecutor->shutdown();
        taskExecutor->join();
    }
}

}  // namespace mongo
