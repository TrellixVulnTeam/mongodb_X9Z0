/**
 *    Copyright (C) 2009-2016 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/commands.h"

#include <string>
#include <vector>

#include "mongo/bson/mutable/document.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/audit.h"
#include "mongo/db/auth/action_set.h"
#include "mongo/db/auth/action_type.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/auth/privilege.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/server_parameters.h"
#include "mongo/rpc/write_concern_error_detail.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"

namespace mongo {

using std::string;
using std::stringstream;

using logger::LogComponent;

Command::CommandMap* Command::_commandsByBestName = nullptr;
Command::CommandMap* Command::_commands = nullptr;

Counter64 Command::unknownCommands;
static ServerStatusMetricField<Counter64> displayUnknownCommands("commands.<UNKNOWN>",
                                                                 &Command::unknownCommands);

namespace {

//mongod --setParameter=enableTestCommands
ExportedServerParameter<bool, ServerParameterType::kStartupOnly> testCommandsParameter(
    ServerParameterSet::getGlobal(), "enableTestCommands", &Command::testCommandsEnabled);

const char kWriteConcernField[] = "writeConcern";
const WriteConcernOptions kMajorityWriteConcern(
    WriteConcernOptions::kMajority,
    // Note: Even though we're setting UNSET here, kMajority implies JOURNAL if journaling is
    // supported by the mongod.
    WriteConcernOptions::SyncMode::UNSET,
    Seconds(60));

}  // namespace

Command::~Command() = default;

BSONObj Command::appendPassthroughFields(const BSONObj& cmdObjWithPassthroughFields,
                                         const BSONObj& request) {
    BSONObjBuilder b;
    b.appendElements(request);
    for (const auto& elem :
         Command::filterCommandRequestForPassthrough(cmdObjWithPassthroughFields)) {
        const auto name = elem.fieldNameStringData();
        if (Command::isGenericArgument(name) && !request.hasField(name)) {
            b.append(elem);
        }
    }
    return b.obj();
}

BSONObj Command::appendMajorityWriteConcern(const BSONObj& cmdObj) {
	//���obj���Ѿ���"writeConcern" field��ֱ�ӷ���
    if (cmdObj.hasField(kWriteConcernField)) {
        return cmdObj;
    }

	//���û��"writeConcern" filed��Ϣ����append׷��
    BSONObjBuilder cmdObjWithWriteConcern;
    cmdObjWithWriteConcern.appendElementsUnique(cmdObj);
    cmdObjWithWriteConcern.append(kWriteConcernField, kMajorityWriteConcern.toBSON());
    return cmdObjWithWriteConcern.obj();
}

// The type of the first field in 'cmdObj' must be mongo::String. The first field is
// interpreted as a collection name.
//��ȡcmdobj��һ��elem��Ҳ���Ǳ���
string Command::parseNsFullyQualified(const string& dbname, const BSONObj& cmdObj) {
    BSONElement first = cmdObj.firstElement();
    uassert(ErrorCodes::BadValue,
            str::stream() << "collection name has invalid type " << typeName(first.type()),
            first.canonicalType() == canonicalizeBSONType(mongo::String));
    const NamespaceString nss(first.valueStringData());
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid namespace specified '" << nss.ns() << "'",
            nss.isValid());
    return nss.ns();
}

// The type of the first field in 'cmdObj' must be mongo::String or Symbol.
// The first field is interpreted as a collection name.
//��ȡcmdobj��һ��elem�������.��
NamespaceString Command::parseNsCollectionRequired(const string& dbname, const BSONObj& cmdObj) {
    // Accepts both BSON String and Symbol for collection name per SERVER-16260
    // TODO(kangas) remove Symbol support in MongoDB 3.0 after Ruby driver audit
    BSONElement first = cmdObj.firstElement();
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "collection name has invalid type " << typeName(first.type()),
            first.canonicalType() == canonicalizeBSONType(mongo::String));
    const NamespaceString nss(dbname, first.valueStringData());
    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid namespace specified '" << nss.ns() << "'",
            nss.isValid());
    return nss;
}

//��ȡ����������UUID
NamespaceString Command::parseNsOrUUID(OperationContext* opCtx,
                                       const string& dbname,
                                       const BSONObj& cmdObj) {
    BSONElement first = cmdObj.firstElement();
    if (first.type() == BinData && first.binDataType() == BinDataType::newUUID) {
        UUIDCatalog& catalog = UUIDCatalog::get(opCtx);
        UUID uuid = uassertStatusOK(UUID::parse(first));
        NamespaceString nss = catalog.lookupNSSByUUID(uuid);
        uassert(ErrorCodes::NamespaceNotFound,
                str::stream() << "UUID " << uuid << " specified in "
                              << cmdObj.firstElement().fieldNameStringData()
                              << " command not found in "
                              << dbname
                              << " database",
                nss.isValid() && nss.db() == dbname);

        return nss;
    } else {
        // Ensure collection identifier is not a Command
        const NamespaceString nss(parseNsCollectionRequired(dbname, cmdObj));
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Invalid collection name specified '" << nss.ns() << "'",
                nss.isNormal());
        return nss;
    }
}

//��test.test
string Command::parseNs(const string& dbname, const BSONObj& cmdObj) const {
    BSONElement first = cmdObj.firstElement();
    if (first.type() != mongo::String)
        return dbname;

    return str::stream() << dbname << '.' << cmdObj.firstElement().valueStringData();
}

//���Բο�//CreateIndexesCmd::addRequiredPrivileges
//���ns�Ƿ�׼ƥ��
ResourcePattern Command::parseResourcePattern(const std::string& dbname,
                                              const BSONObj& cmdObj) const {
    const std::string ns = parseNs(dbname, cmdObj);
	//���ns��Ч��
    if (!NamespaceString::validCollectionComponent(ns)) { //ֻ��db��
        return ResourcePattern::forDatabaseName(ns);
    }
    return ResourcePattern::forExactNamespace(NamespaceString(ns)); //db.collection
}

//BasicCommand�̳и���(�๹��һ����basicCommandʵ�֣���AddShardCmd()��) WriteCommand  Command����Դ��Command::findCommand
//mongod  WriteCommand(CmdInsert  CmdUpdate  CmdDelete�ȼ̳�WriteCommand��,WriteCommand�̳�Command��)
//mongos  ClusterWriteCmd(ClusterCmdInsert  ClusterCmdUpdate  ClusterCmdDelete��̳и��࣬��Ӧmongosת��)

//commandͳ����db.serverStatus().metrics.commands����鿴
//����: AddShardCmd() : BasicCommand("addShard", "addshard") {}
//����ע�ᣬ����ע�����������ȫ�����浽_commandsȫ��map����
Command::Command(StringData name, StringData oldName) //name��oldNameʵ������ͬһ��command��ֻ�ǿ��ܸ�����
    : _name(name.toString()), //������
     //��Ӧ����ִ��ͳ�ƣ�total�����ܵģ�failed����ִ��ʧ�ܵĴ���
      _commandsExecutedMetric("commands." + _name + ".total", &_commandsExecuted),
      _commandsFailedMetric("commands." + _name + ".failed", &_commandsFailed) {
    //���_commands map��û�����ɣ���newһ��
    if (_commands == 0)
        _commands = new CommandMap();
	//ûʲô��
    if (_commandsByBestName == 0)
        _commandsByBestName = new CommandMap();
	//��name�����Ӧ��command��ӵ�map����
    Command*& c = (*_commands)[name];
    if (c)
        log() << "warning: 2 commands with name: " << _name;
    c = this;
    (*_commandsByBestName)[name] = this;

	//�󲿷�����name��oldName��һ���ģ�������������ֻ���¼һ��
    if (!oldName.empty()) //Ҳ����name��oldName���������Ӧ����ͬһ��this��
        (*_commands)[oldName.toString()] = this;
}

void Command::help(stringstream& help) const {
    help << "no help defined";
}

Status Command::explain(OperationContext* opCtx,
                        const string& dbname,
                        const BSONObj& cmdObj,
                        ExplainOptions::Verbosity verbosity,
                        BSONObjBuilder* out) const {
    return {ErrorCodes::IllegalOperation, str::stream() << "Cannot explain cmd: " << getName()};
}

BSONObj Command::runCommandDirectly(OperationContext* opCtx, const OpMsgRequest& request) {
	//�ҵ������Ӧ��command
	auto command = Command::findCommand(request.getCommandName());
    invariant(command);

    BSONObjBuilder out;
    try {
		//command::publicRun
        bool ok = command->publicRun(opCtx, request, out);
        appendCommandStatus(out, ok);
    } catch (const StaleConfigException&) {
        // These exceptions are intended to be handled at a higher level and cannot losslessly
        // round-trip through Status.
        throw;
    } catch (const DBException& ex) {
        out.resetToEmpty();
        appendCommandStatus(out, ex.toStatus());
    }
    return out.obj();
}

//����name��ȡ��Ӧ��Command��Ϣ   //��ӵط���Command::Command
//strategy.cpp�е�runCommand�е���  ����name�����Ƿ�֧�֣����е�command����_commands�б���
Command* Command::findCommand(StringData name) {
    CommandMap::const_iterator i = _commands->find(name);
    if (i == _commands->end())
        return 0;
    return i->second;
}

//��result��append status��Ϣ
bool Command::appendCommandStatus(BSONObjBuilder& result, const Status& status) {
    appendCommandStatus(result, status.isOK(), status.reason());
    BSONObj tmp = result.asTempObj();
    if (!status.isOK() && !tmp.hasField("code")) {
        result.append("code", status.code());
        result.append("codeName", ErrorCodes::errorString(status.code()));
    }
    return status.isOK();
}

//Command::appendCommandStatus����  //��result��append ok����errmsg��Ϣ
void Command::appendCommandStatus(BSONObjBuilder& result, bool ok, const std::string& errmsg) {
    BSONObj tmp = result.asTempObj();
    bool have_ok = tmp.hasField("ok");
    bool need_errmsg = !ok && !tmp.hasField("errmsg");

    if (!have_ok)
        result.append("ok", ok ? 1.0 : 0.0);

    if (need_errmsg) {
        result.append("errmsg", errmsg);
    }
}

//��result��append awaitReplicationStatus��wtimeout��Ϣ
void Command::appendCommandWCStatus(BSONObjBuilder& result,
                                    const Status& awaitReplicationStatus,
                                    const WriteConcernResult& wcResult) {
    if (!awaitReplicationStatus.isOK() && !result.hasField("writeConcernError")) {
        WriteConcernErrorDetail wcError;
        wcError.setErrCode(awaitReplicationStatus.code());
        wcError.setErrMessage(awaitReplicationStatus.reason());
        if (wcResult.wTimedOut) {
            wcError.setErrInfo(BSON("wtimeout" << true));
        }
        result.append("writeConcernError", wcError.toBSON());
    }
}

//����Ƿ��и������Ȩ�ޣ������_checkAuthorizationImpl����
Status BasicCommand::checkAuthForRequest(OperationContext* opCtx, const OpMsgRequest& request) {
    uassertNoDocumentSequences(request);
    return checkAuthForOperation(opCtx, request.getDatabase().toString(), request.body);
}

//�����BasicCommand::checkAuthForRequest����
Status BasicCommand::checkAuthForOperation(OperationContext* opCtx,
                                           const std::string& dbname,
                                           const BSONObj& cmdObj) {
    return checkAuthForCommand(opCtx->getClient(), dbname, cmdObj);
}

//�����BasicCommand::checkAuthForOperation����
Status BasicCommand::checkAuthForCommand(Client* client,
                                         const std::string& dbname,
                                         const BSONObj& cmdObj) {
    std::vector<Privilege> privileges;
	//�����������ӦCreateIndexesCmd::addRequiredPrivileges  DropIndexesCmd::addRequiredPrivileges ��ͬ�����в�ͬʵ��
	//DropDatabaseCmd::addRequiredPrivileges�� 
	//log() << "ddd test ................... checkAuthForCommand,  begin !!!!!!!!!!!!!!!!!!";
	//��ȡdbname��Ӧ��Privilege
    this->addRequiredPrivileges(dbname, cmdObj, &privileges);
    if (AuthorizationSession::get(client)->isAuthorizedForPrivileges(privileges)) {
		//log() << "ddd test ................... checkAuthForCommand,  ok !!!!!!!!!!!!!!!!!!";
        return Status::OK();
	}
	//log() << "ddd test ................... checkAuthForCommand,  failed !!!!!!!!!!!!!!!!!!";
    return Status(ErrorCodes::Unauthorized, "unauthorized");
}

void Command::redactForLogging(mutablebson::Document* cmdObj) {}

BSONObj Command::getRedactedCopyForLogging(const BSONObj& cmdObj) {
    namespace mmb = mutablebson;
    mmb::Document cmdToLog(cmdObj, mmb::Document::kInPlaceDisabled);
    redactForLogging(&cmdToLog);
    BSONObjBuilder bob;
    cmdToLog.writeTo(&bob);
    return bob.obj();
}

//Command::checkAuthorization����  command��֤���
static Status _checkAuthorizationImpl(Command* c,
                                      OperationContext* opCtx,
                                      const OpMsgRequest& request) {
    namespace mmb = mutablebson;
    auto client = opCtx->getClient();
    auto dbname = request.getDatabase();
	//����ֻ����admin��ִ�У������ǰ������admin�⣬��ֱ�ӱ���
    if (c->adminOnly() && dbname != "admin") {
        return Status(ErrorCodes::Unauthorized,
                      str::stream() << c->getName()
                                    << " may only be run against the admin database.");
    }

	//���ʹ������֤���ܣ�����֤���
    if (AuthorizationSession::get(client)->getAuthorizationManager().isAuthEnabled()) {
		//����mongos����ɾ�ģ���ClusterWriteCmd::checkAuthForRequest
		//BasicCommand::checkAuthForRequest  ClusterWriteCmd::checkAuthForRequest WriteCommand::checkAuthForRequest�ȣ���ͬ������ܻ��в�ͬʵ��
        Status status = c->checkAuthForRequest(opCtx, request);
        if (status == ErrorCodes::Unauthorized) {
            mmb::Document cmdToLog(request.body, mmb::Document::kInPlaceDisabled);
            c->redactForLogging(&cmdToLog);
            return Status(ErrorCodes::Unauthorized,
                          str::stream() << "not authorized on " << dbname << " to execute command "
                                        << cmdToLog.toString());
        }
        if (!status.isOK()) {
            return status;
        }
    } else if (c->adminOnly() && c->localHostOnlyIfNoAuth() &&
               !client->getIsLocalHostConnection()) {
        return Status(ErrorCodes::Unauthorized,
                      str::stream() << c->getName()
                                    << " must run from localhost when running db without auth");
    }
    return Status::OK();
}

//command��֤���  execCommandDatabase����
//command��֤���,���ͻ�������ʹ�õ��˺������Ƿ��в�����command��Ȩ��
Status Command::checkAuthorization(Command* c,
                                   OperationContext* opCtx,
                                   const OpMsgRequest& request) {
    Status status = _checkAuthorizationImpl(c, opCtx, request);
    if (!status.isOK()) {
        log(LogComponent::kAccessControl) << status;
    }
	//�����أ�����
    audit::logCommandAuthzCheck(opCtx->getClient(), request, c, status.code());
    return status;
}
//execCommandClient->Command::publicRun 
//execCommandClient�е���
//Command::runCommandDirectly ����

//3.6�汾mongosͨ������·������:ServiceEntryPointMongos::handleRequest->Strategy::clientCommand
//			->runCommand->execCommandClient
//һ��3.6�汾mongodͨ������·������:ServiceEntryPointMongod::handleRequest->runCommands
//			->execCommandDatabase->runCommandImpl->Command::publicRun����
bool Command::publicRun(OperationContext* opCtx,
                        const OpMsgRequest& request,
                        BSONObjBuilder& result) {
    try {
		//mongod(WriteCommand::enhancedRun(insert  delete update))
		//��������BasicCommand::enhancedRun  
		//mongos (ClusterWriteCmd::enhancedRun)(insert  delete update)) ��ͬ�����Ӧ��ͬ�ӿ�
		//mongos��ӦClusterFindCmd::run  mongod��Ӧ FindCmd::run
		//mongos��ӦClusterGetMoreCmd::run   mongod��ӦGetMoreCmd::run  
		
		return enhancedRun(opCtx, request, result); //BasicCommand::enhancedRun
    } catch (const DBException& e) {
        if (e.code() == ErrorCodes::Unauthorized) {
            audit::logCommandAuthzCheck(
                opCtx->getClient(), request, this, ErrorCodes::Unauthorized);
        }
        throw;
    }
}

bool Command::isHelpRequest(const BSONElement& helpElem) {
    return !helpElem.eoo() && helpElem.trueValue();
}

const char Command::kHelpFieldName[] = "help";

void Command::generateHelpResponse(OperationContext* opCtx,
                                   rpc::ReplyBuilderInterface* replyBuilder,
                                   const Command& command) {
    std::stringstream ss;
    BSONObjBuilder helpBuilder;
    ss << "help for: " << command.getName() << " ";
    command.help(ss);
    helpBuilder.append("help", ss.str());

    replyBuilder->setCommandReply(helpBuilder.obj());
    replyBuilder->setMetadata(rpc::makeEmptyMetadata());
}

namespace {
const stdx::unordered_set<std::string> userManagementCommands{"createUser",
                                                              "updateUser",
                                                              "dropUser",
                                                              "dropAllUsersFromDatabase",
                                                              "grantRolesToUser",
                                                              "revokeRolesFromUser",
                                                              "createRole",
                                                              "updateRole",
                                                              "dropRole",
                                                              "dropAllRolesFromDatabase",
                                                              "grantPrivilegesToRole",
                                                              "revokePrivilegesFromRole",
                                                              "grantRolesToRole",
                                                              "revokeRolesFromRole",
                                                              "_mergeAuthzCollections",
                                                              "authSchemaUpgrade"};
}  // namespace

bool Command::isUserManagementCommand(const std::string& name) {
    return userManagementCommands.count(name);
}

void BasicCommand::uassertNoDocumentSequences(const OpMsgRequest& request) {
    uassert(40472,
            str::stream() << "The " << getName() << " command does not support document sequences.",
            request.sequences.empty());
}

//Command::publicRun�е���
bool BasicCommand::enhancedRun(OperationContext* opCtx,
                               const OpMsgRequest& request,
                               BSONObjBuilder& result) {
    uassertNoDocumentSequences(request);
	//ErrmsgCommandDeprecated::run   FindCmd::run  
	//getMore����mongos��ӦClusterGetMoreCmd::run  mongod��ӦGetMoreCmd::run
	//mongos��ӦClusterFindCmd::run  mongod��Ӧ FindCmd::run
    return run(opCtx, request.getDatabase().toString(), request.body, result);
}

//���ErrmsgCommandDeprecated
bool ErrmsgCommandDeprecated::run(OperationContext* opCtx,
                                  const std::string& db,
                                  const BSONObj& cmdObj,
                                  BSONObjBuilder& result) {
    std::string errmsg; 
    auto ok = errmsgRun(opCtx, db, cmdObj, errmsg, result);
    if (!errmsg.empty()) {
        appendCommandStatus(result, ok, errmsg);
    }
    return ok;
}

//Command::appendPassthroughFields����
//cmdObj��Ӧ�������еĲ���elem�����⴦��
BSONObj Command::filterCommandRequestForPassthrough(const BSONObj& cmdObj) {
    BSONObjBuilder bob;
    for (auto elem : cmdObj) {
        const auto name = elem.fieldNameStringData();
        if (name == "$readPreference") {
			//�����$readPreference��ת��Ϊ$queryOptions
            BSONObjBuilder(bob.subobjStart("$queryOptions")).append(elem);
        } else if (!Command::isGenericArgument(name) ||  //
                   name == "$queryOptions" ||            //
                   name == "maxTimeMS" ||                //
                   name == "readConcern" ||              //
                   name == "writeConcern" ||             //
                   name == "lsid" ||                     //
                   name == "txnNumber") {
            //���elem����Щname����ֱ�Ӻ���
            // This is the whitelist of generic arguments that commands can be trusted to blindly
            // forward to the shards.
            bob.append(elem);
        }
    }
    return bob.obj();
}

//Command::filterCommandReplyForPassthrough����  ȥ��cmdobj�е������ֶ�
//cmdObj��ӦӦ���еĲ���elem�����⴦��
void Command::filterCommandReplyForPassthrough(const BSONObj& cmdObj, BSONObjBuilder* output) {
    for (auto elem : cmdObj) {
        const auto name = elem.fieldNameStringData();
        if (name == "$configServerState" ||  //
            name == "$gleStats" ||           //
            name == "$clusterTime" ||        //
            name == "$oplogQueryData" ||     //
            name == "$replData" ||           //
            name == "operationTime") {
            continue;
        }
        output->append(elem);
    }
}

BSONObj Command::filterCommandReplyForPassthrough(const BSONObj& cmdObj) {
    BSONObjBuilder bob;
    filterCommandReplyForPassthrough(cmdObj, &bob);
    return bob.obj();
}

}  // namespace mongo
