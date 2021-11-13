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

#include "mongo/util/net/op_msg.h"

#include <bitset>
#include <set>

#include "mongo/base/data_type_endian.h"
#include "mongo/db/bson/dotted_path_support.h"
#include "mongo/rpc/object_check.h"
#include "mongo/util/bufreader.h"
#include "mongo/util/hex.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

auto kAllSupportedFlags = OpMsg::kChecksumPresent | OpMsg::kMoreToCome;

//OP_MSG flag��Ч�Լ��  OpMsg::parse����  flagֻ�ܶ�kAllSupportedFlags������λ���в��������������λ���˲�����ֱ�ӱ���
//�ο�https://docs.mongodb.com/manual/reference/mongodb-wire-protocol/#wire-msg-sections
bool containsUnknownRequiredFlags(uint32_t flags) {
    const uint32_t kRequiredFlagMask = 0xffff;  // Low 2 bytes are required, high 2 are optional.
    return (flags & ~kAllSupportedFlags & kRequiredFlagMask) != 0;
}

enum class Section : uint8_t {
    kBody = 0,
    kDocSequence = 1,
};

}  // namespace

//��ȡmessage�е�flag
uint32_t OpMsg::flags(const Message& message) {
    if (message.operation() != dbMsg)
        return 0;  // Other command protocols are the same as no flags set.

    return BufReader(message.singleData().data(), message.dataSize())
        .read<LittleEndian<uint32_t>>();
}

//����message�е�flags
void OpMsg::replaceFlags(Message* message, uint32_t flags) {
    invariant(!message->empty());
    invariant(message->operation() == dbMsg);
    invariant(message->dataSize() >= static_cast<int>(sizeof(uint32_t)));

    DataView(message->singleData().data()).write<LittleEndian<uint32_t>>(flags);
}

/*
OP_MSG {
    MsgHeader header;          // standard message header   Ҳ����struct Layout {}
    uint32 flagBits;           // message flags
    Sections[] sections;       // data sections
    optional<uint32> checksum; // optional CRC-32C checksum
}
*/
//opMsgRequestFromAnyProtocol->OpMsgRequest::parse����
//��message�н�����OpMsg��Ϣ
OpMsg OpMsg::parse(const Message& message) try {
    // It is the caller's responsibility to call the correct parser for a given message type.
    invariant(!message.empty());
    invariant(message.operation() == dbMsg);

	//LOG(1) << "ddd test ..... OpMsg::parse";
	//��ȡflagBits
    const uint32_t flags = OpMsg::flags(message);
	//flagBits��Ч�Լ��
    uassert(ErrorCodes::IllegalOpMsgFlag,
            str::stream() << "Message contains illegal flags value: Ob"
                          << std::bitset<32>(flags).to_string(),
            !containsUnknownRequiredFlags(flags));

	//У����Ĭ��4�ֽ�
    constexpr int kCrc32Size = 4;
	//�жϸ�mongo����body�����Ƿ�������У�鹦��
    const bool haveChecksum = flags & kChecksumPresent;
	//���������У�鹦�ܣ�����ĩβ4�ֽ�ΪУ����
    const int checksumSize = haveChecksum ? kCrc32Size : 0;

    // The sections begin after the flags and before the checksum (if present).
    //sections�ֶ�����
    BufReader sectionsBuf(message.singleData().data() + sizeof(flags),
                          message.dataSize() - sizeof(flags) - checksumSize);

    // TODO some validation may make more sense in the IDL parser. I've tagged them with comments.
    bool haveBody = false;
    OpMsg msg;
	//����sections��Ӧ������������
    while (!sectionsBuf.atEof()) {
		//BufReader::read
        const auto sectionKind = sectionsBuf.read<Section>();
		//LOG(1) << "ddd test ..... OpMsg::parse  sectionKind:" << uint32_t(sectionKind);
		//kindΪ0��Ӧ��������body���ݣ�����ͨ��bson����
        switch (sectionKind) {
			//body kind
            case Section::kBody: {
                uassert(40430, "Multiple body sections in message", !haveBody);
                haveBody = true;
				//���������bson��Ϣ����������
                msg.body = sectionsBuf.read<Validated<BSONObj>>();
                break;
            }

			//DocSequence��ʱû�����ף��õ��ĵط����٣�����
            case Section::kDocSequence: {
                // We use an O(N^2) algorithm here and an O(N*M) algorithm below. These are fastest
                // for the current small values of N, but would be problematic if it is large.
                // If we need more document sequences, raise the limit and use a better algorithm.
                uassert(ErrorCodes::TooManyDocumentSequences,
                        "Too many document sequences in OP_MSG",
                        msg.sequences.size() < 2);  // Limit is <=2 since we are about to add one.

                // The first 4 bytes are the total size, including themselves.
                const auto remainingSize =
                    sectionsBuf.read<LittleEndian<int32_t>>() - sizeof(int32_t);
                BufReader seqBuf(sectionsBuf.skip(remainingSize), remainingSize);
                const auto name = seqBuf.readCStr();
                uassert(40431,
                        str::stream() << "Duplicate document sequence: " << name,
                        !msg.getSequence(name));  // TODO IDL

                msg.sequences.push_back({name.toString()});
                while (!seqBuf.atEof()) {
                    msg.sequences.back().objs.push_back(seqBuf.read<Validated<BSONObj>>());
                }
                break;
            }

            default:
                // Using uint32_t so we append as a decimal number rather than as a char.
                uasserted(40432, str::stream() << "Unknown section kind " << uint32_t(sectionKind));
        }
    }

    uassert(40587, "OP_MSG messages must have a body", haveBody);

    // Detect duplicates between doc sequences and body. TODO IDL
    // Technically this is O(N*M) but N is at most 2.
    //body��sequenceȥ���ж�
    for (const auto& docSeq : msg.sequences) {
        const char* name = docSeq.name.c_str();  // Pointer is redirected by next call.
        auto inBody =
            !dotted_path_support::extractElementAtPathOrArrayAlongPath(msg.body, name).eoo();
        uassert(40433,
                str::stream() << "Duplicate field between body and document sequence "
                              << docSeq.name,
                !inBody);
    }

    return msg;
} catch (const DBException& ex) {
    LOG(1) << "invalid message: " << ex.code() << " " << redact(ex) << " -- "
           << redact(hexdump(message.singleData().view2ptr(), message.size()));
    throw;
}

//OpMsg���л�
Message OpMsg::serialize() const {
    OpMsgBuilder builder;
    for (auto&& seq : sequences) {
        auto docSeq = builder.beginDocSequence(seq.name);
        for (auto&& obj : seq.objs) {
            docSeq.append(obj);
        }
    }
    builder.beginBody().appendElements(body);
    return builder.finish();
}

/**
 * Makes all BSONObjs in this object share ownership with buffer.
 */
void OpMsg::shareOwnershipWith(const ConstSharedBuffer& buffer) {
    if (!body.isOwned()) {
        body.shareOwnershipWith(buffer);
    }
    for (auto&& seq : sequences) {
        for (auto&& obj : seq.objs) {
            if (!obj.isOwned()) {
                obj.shareOwnershipWith(buffer);
            }
        }
    }
}

//���kDocSequence���͵�name����
auto OpMsgBuilder::beginDocSequence(StringData name) -> DocSequenceBuilder {
    invariant(_state == kEmpty || _state == kDocSequence);
    invariant(!_openBuilder);
    _openBuilder = true;
    _state = kDocSequence;
	//���kind 1���ֽ�
    _buf.appendStruct(Section::kDocSequence);
    int sizeOffset = _buf.len();
    _buf.skip(sizeof(int32_t));  // section size.
    //���name
    _buf.appendStr(name, true);
    return DocSequenceBuilder(this, &_buf, sizeOffset);
}

void OpMsgBuilder::finishDocumentStream(DocSequenceBuilder* docSequenceBuilder) {
    invariant(_state == kDocSequence);
    invariant(_openBuilder);
    _openBuilder = false;
    const int32_t size = _buf.len() - docSequenceBuilder->_sizeOffset;
    invariant(size > 0);
    DataView(_buf.buf()).write<LittleEndian<int32_t>>(size, docSequenceBuilder->_sizeOffset);
}
//���kBody��������
BSONObjBuilder OpMsgBuilder::beginBody() {
    invariant(_state == kEmpty || _state == kDocSequence);
    _state = kBody;
    _buf.appendStruct(Section::kBody);
    invariant(_bodyStart == 0);
    _bodyStart = _buf.len();  // Cannot be 0.
    return BSONObjBuilder(_buf);
}

//��ȡbody����
BSONObjBuilder OpMsgBuilder::resumeBody() {
    invariant(_state == kBody);
    invariant(_bodyStart != 0);
    return BSONObjBuilder(BSONObjBuilder::ResumeBuildingTag(), _buf, _bodyStart);
}

AtomicBool OpMsgBuilder::disableDupeFieldCheck_forTest{false};

//����message����
Message OpMsgBuilder::finish() {
    if (kDebugBuild && !disableDupeFieldCheck_forTest.load()) {
        std::set<StringData> seenFields;
        for (auto elem : resumeBody().asTempObj()) {
            if (!(seenFields.insert(elem.fieldNameStringData()).second)) {
                severe() << "OP_MSG with duplicate field '" << elem.fieldNameStringData()
                         << "' : " << redact(resumeBody().asTempObj());
                fassert(40474, false);
            }
        }
    }

    invariant(_state == kBody);
    invariant(_bodyStart);
    invariant(!_openBuilder);
    _state = kDone;

    const auto size = _buf.len();
    MSGHEADER::View header(_buf.buf());
    header.setMessageLength(size);
    // header.setRequestMsgId(...); // These are currently filled in by the networking layer.
    // header.setResponseToMsgId(...);
    header.setOpCode(dbMsg);
    return Message(_buf.release());
}

}  // namespace mongo
