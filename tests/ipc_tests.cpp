// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Wang Yunzheng <wyz-mcast@outlook.com>

#include "ipc/IpcProtocol.h"
#include "ipc/Messages.h"
#include "ipc/NamedPipe.h"
#include "supervisor/RestartController.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

int failures = 0;

void check(bool condition, const std::string &message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

template <typename Left, typename Right>
void checkEqual(const Left &left, const Right &right,
                const std::string &message)
{
    check(left == right, message);
}

mcast::ipc::Message messageOf(mcast::ipc::MessageType type,
                              std::vector<std::uint8_t> payload = {})
{
    mcast::ipc::Message message;
    message.header.type = type;
    message.header.source = mcast::ipc::ProcessRole::Protocol;
    message.header.destination = mcast::ipc::ProcessRole::Shell;
    message.header.messageId = 42;
    message.header.correlationId = 7;
    message.header.sessionId = 9;
    message.header.stateVersion = 11;
    message.header.timestampMs = 123456;
    message.payload = std::move(payload);
    return message;
}

void testPayloadRoundTrip()
{
    using namespace mcast::ipc;

    PayloadBuilder builder;
    check(builder.addString(1, "Media Cast"), "add string");
    check(builder.addUInt64(2, 0x1234567890ABCDEFull), "add uint64");
    check(builder.addBool(3, true), "add bool");
    const auto payloadResult = std::move(builder).finish();
    check(payloadResult.has_value(), "builder produces payload");
    if (!payloadResult)
        return;
    const std::vector<std::uint8_t> &payload = *payloadResult;

    PayloadReader reader(payload);
    check(reader.valid(), "reader accepts payload");
    checkEqual(reader.getString(1), std::optional<std::string>("Media Cast"),
               "string round trip");
    checkEqual(reader.getUInt64(2),
               std::optional<std::uint64_t>(0x1234567890ABCDEFull),
               "uint64 round trip");
    checkEqual(reader.getBool(3), std::optional<bool>(true),
               "bool round trip");

    PayloadBuilder duplicate;
    check(duplicate.addString(1, "one"), "duplicate first field");
    check(!duplicate.addString(1, "two"), "duplicate id is rejected");
    check(!duplicate.valid(), "duplicate builder becomes invalid");

    PayloadBuilder oversized;
    const std::vector<std::uint8_t> block(kMaxFieldSize, 0x5A);
    bool rejected = false;
    for (std::uint16_t id = 0; id < 8; ++id) {
        if (!oversized.addBytes(id, block)) {
            rejected = true;
            break;
        }
    }
    check(rejected, "builder rejects total payload over limit");
    check(!std::move(oversized).finish().has_value(),
          "oversized builder has no ambiguous empty result");

    PayloadBuilder malformedSource;
    check(malformedSource.addString(1, "abc"), "build malformed source");
    auto malformed = std::move(malformedSource).finish();
    check(malformed.has_value(), "malformed source payload");
    if (malformed) {
        (*malformed)[6] = 99;
        PayloadReader malformedReader(*malformed);
        check(!malformedReader.valid(), "unknown TLV field type rejected");
    }
}

void testFrameRoundTrip()
{
    using namespace mcast::ipc;

    PayloadBuilder builder;
    check(builder.addString(1, "hello"), "build frame payload");
    const auto payloadResult = std::move(builder).finish();
    check(payloadResult.has_value(), "frame payload result");
    if (!payloadResult)
        return;
    const auto &payload = *payloadResult;
    const Message original = messageOf(MessageType::Hello, payload);
    const std::vector<std::uint8_t> frame = encodeFrame(original);
    check(frame.size() == kHeaderSize + payload.size(), "encoded frame size");
    check(frame[0] == 'M' && frame[1] == 'C' && frame[2] == 'S'
              && frame[3] == '1',
          "wire magic is MCS1, got "
              + std::to_string(frame[0]) + ","
              + std::to_string(frame[1]) + ","
              + std::to_string(frame[2]) + ","
              + std::to_string(frame[3]));

    const FrameDecodeResult decoded = decodeFrame(frame);
    checkEqual(
        decoded.status, DecodeStatus::Ok,
        "frame decodes status="
            + std::to_string(static_cast<int>(decoded.status))
            + " minor=" + std::to_string(
                decoded.message.header.versionMinor)
            + " type=" + std::to_string(
                static_cast<int>(decoded.message.header.type))
            + " source=" + std::to_string(
                static_cast<int>(decoded.message.header.source))
            + " destination=" + std::to_string(
                static_cast<int>(decoded.message.header.destination))
            + " flags=" + std::to_string(
                static_cast<int>(decoded.message.header.flags))
            + " rawMajor=" + std::to_string(frame[4] | (frame[5] << 8))
            + " rawMinor=" + std::to_string(frame[6] | (frame[7] << 8))
            + " expectedMajor=" + std::to_string(kProtocolVersionMajor)
            + " expectedMinor=" + std::to_string(kProtocolVersionMinor));
    checkEqual(decoded.consumed, frame.size(), "frame consumed size");
    checkEqual(decoded.message.header.type, MessageType::Hello,
               "message type round trip");
    checkEqual(decoded.message.header.messageId, 42ull,
               "message id round trip");
    check(decoded.message.payload == payload, "payload round trip");
}

void testInvalidFrames()
{
    using namespace mcast::ipc;

    const Message original = messageOf(MessageType::Heartbeat);
    std::vector<std::uint8_t> frame = encodeFrame(original);

    FrameDecodeResult truncated = decodeFrame(
        std::span<const std::uint8_t>(frame).first(kHeaderSize - 1));
    checkEqual(truncated.status, DecodeStatus::NeedMoreData,
               "short header needs more data");
    checkEqual(truncated.required, kHeaderSize, "short header requirement");

    std::vector<std::uint8_t> badMagic = frame;
    badMagic[0] ^= 0xFF;
    const FrameDecodeResult badMagicResult = decodeFrame(badMagic);
    checkEqual(badMagicResult.status, DecodeStatus::InvalidMagic,
               "bad magic rejected");

    std::vector<std::uint8_t> badVersion = frame;
    badVersion[4] = 9;
    const FrameDecodeResult badVersionResult = decodeFrame(badVersion);
    checkEqual(badVersionResult.status, DecodeStatus::UnsupportedVersion,
               "bad version rejected");

    std::vector<std::uint8_t> oversized = frame;
    const std::uint32_t tooLarge = kMaxPayloadSize + 1;
    for (int index = 0; index < 4; ++index)
        oversized[54 + index] =
            static_cast<std::uint8_t>(tooLarge >> (index * 8));
    const FrameDecodeResult oversizedResult = decodeFrame(oversized);
    checkEqual(oversizedResult.status, DecodeStatus::PayloadTooLarge,
               "oversized payload rejected");

    std::vector<std::uint8_t> badReserved = frame;
    badReserved[kHeaderReservedOffset] = 1;
    checkEqual(decodeFrame(badReserved).status, DecodeStatus::InvalidHeader,
               "nonzero reserved bytes rejected");

    std::vector<std::uint8_t> badFlags = frame;
    badFlags[12] = 0x80;
    checkEqual(decodeFrame(badFlags).status, DecodeStatus::InvalidHeader,
               "unknown flags rejected");

    Message withPayload = original;
    withPayload.payload.assign(32, 0x42);
    std::vector<std::uint8_t> truncatedPayload = encodeFrame(withPayload);
    truncatedPayload.pop_back();
    checkEqual(decodeFrame(truncatedPayload).status,
               DecodeStatus::NeedMoreData,
               "truncated payload needs more data");
}

void testHelloSchema()
{
    using namespace mcast::ipc;

    HelloInfo hello{
        GetCurrentProcessId(),
        "build-194a625",
        "instance-nonce",
        std::string(64, 'a'),
        0x5,
    };
    std::string error;
    const auto message = makeHelloMessage(
        ConnectionChannel::Control, ProcessRole::Protocol, ProcessRole::Shell,
        1, hello, error);
    check(message.has_value(), "Hello message created: " + error);
    if (!message)
        return;

    const auto decoded = decodeHelloMessage(
        *message, ConnectionChannel::Control, error);
    check(decoded.has_value(), "Hello message decoded: " + error);
    if (!decoded)
        return;
    checkEqual(decoded->processId, hello.processId, "Hello pid");
    checkEqual(decoded->buildId, hello.buildId, "Hello build id");
    checkEqual(
        decoded->instanceNonce, hello.instanceNonce,
        "Hello instance nonce");
    checkEqual(
        decoded->executableHash, hello.executableHash,
        "Hello executable hash");
    checkEqual(
        decoded->capabilities, hello.capabilities,
        "Hello capabilities");

    HelloExpectation expectation{
        hello.processId,
        hello.buildId,
        hello.instanceNonce,
        hello.executableHash,
        0x7,
    };
    check(validateHello(*decoded, expectation, error),
          "Hello peer identity matches expectation: " + error);
    expectation.peerProcessId += 1;
    check(!validateHello(*decoded, expectation, error),
          "Hello rejects mismatched peer PID");

    const HelloAckInfo ack{
        true,
        kProtocolVersionMajor,
        kProtocolVersionMinor,
        hello.instanceNonce,
        0x5,
        "accepted",
    };
    const auto ackMessage = makeHelloAckMessage(
        ConnectionChannel::Control, ProcessRole::Shell, ProcessRole::Protocol, 2,
        message->header.messageId, ack, error);
    check(ackMessage.has_value(), "HelloAck created: " + error);
    if (ackMessage) {
        const auto wrongCorrelation = decodeHelloAckMessage(
            *ackMessage, message->header.messageId + 1,
            ConnectionChannel::Control, error);
        check(!wrongCorrelation, "HelloAck rejects wrong correlation");

        const auto decodedAck = decodeHelloAckMessage(
            *ackMessage, message->header.messageId,
            ConnectionChannel::Control, error);
        check(decodedAck.has_value(), "HelloAck decoded: " + error);
        if (decodedAck) {
            check(decodedAck->accepted, "HelloAck accepted flag");
            checkEqual(
                decodedAck->instanceNonce, ack.instanceNonce,
                "HelloAck nonce");
            checkEqual(
                decodedAck->capabilities, ack.capabilities,
                "HelloAck capabilities");
        }
    }

    MessageHeader direction;
    direction.type = MessageType::UiActivateRequest;
    direction.source = ProcessRole::Protocol;
    direction.destination = ProcessRole::Shell;
    check(validateMessageDirection(
              ConnectionChannel::Control, direction, error),
          "valid UiActivateRequest direction");
    direction.source = ProcessRole::Ui;
    check(!validateMessageDirection(
              ConnectionChannel::Control, direction, error),
          "invalid UiActivateRequest source rejected");

    direction.type = MessageType::SessionSnapshot;
    direction.source = ProcessRole::Protocol;
    direction.destination = ProcessRole::Ui;
    check(validateMessageDirection(
              ConnectionChannel::Playback, direction, error),
          "valid playback snapshot direction");
    direction.destination = ProcessRole::Shell;
    check(!validateMessageDirection(
              ConnectionChannel::Playback, direction, error),
          "Shell rejected on playback pipe");

    direction.type = MessageType::ShellHandoffRequest;
    direction.source = ProcessRole::Shell;
    direction.destination = ProcessRole::Shell;
    check(validateMessageDirection(
              ConnectionChannel::Control, direction, error),
          "Shell handoff direction accepted");
    check(!validateMessageDirection(
              ConnectionChannel::Playback, direction, error),
          "Shell handoff rejected on playback pipe");
    direction.type = MessageType::ShellHandoffComplete;
    check(validateMessageDirection(
              ConnectionChannel::Control, direction, error),
          "Shell handoff completion accepted");
    direction.type = MessageType::ShellHandoffFinalAck;
    check(validateMessageDirection(
              ConnectionChannel::Control, direction, error),
          "Shell handoff final acknowledgement accepted");
    direction.type = MessageType::UiOpenRequest;
    direction.source = ProcessRole::Shell;
    direction.destination = ProcessRole::Protocol;
    check(validateMessageDirection(
              ConnectionChannel::Control, direction, error),
          "Shell UiOpenRequest accepted");
    check(!validateMessageDirection(
              ConnectionChannel::Playback, direction, error),
          "Shell UiOpenRequest rejected on playback pipe");
    direction.type = MessageType::UiOpenReady;
    direction.source = ProcessRole::Protocol;
    direction.destination = ProcessRole::Shell;
    check(validateMessageDirection(
              ConnectionChannel::Control, direction, error),
          "Protocol UiOpenReady accepted");
    check(!validateMessageDirection(
              ConnectionChannel::Playback, direction, error),
          "Protocol UiOpenReady rejected on playback pipe");
    direction.type = MessageType::SetReceivingEnabled;
    direction.source = ProcessRole::Shell;
    direction.destination = ProcessRole::Protocol;
    check(validateMessageDirection(
              ConnectionChannel::Command, direction, error),
          "SetReceivingEnabled accepted on command pipe");
    check(!validateMessageDirection(
              ConnectionChannel::Control, direction, error),
          "SetReceivingEnabled rejected on control pipe");
    direction.type = MessageType::ReceivingState;
    direction.source = ProcessRole::Protocol;
    direction.destination = ProcessRole::Shell;
    check(validateMessageDirection(
              ConnectionChannel::Command, direction, error),
          "ReceivingState accepted on command pipe");
    check(validateMessageDirection(
              ConnectionChannel::Control, direction, error),
          "ReceivingState accepted as an unsolicited control update");
    direction.type = MessageType::PlaybackCommand;
    direction.source = ProcessRole::Protocol;
    direction.destination = ProcessRole::Ui;
    check(validateMessageDirection(
              ConnectionChannel::Playback, direction, error),
          "PlaybackCommand accepted on playback pipe");
    check(!validateMessageDirection(
              ConnectionChannel::Control, direction, error),
          "PlaybackCommand rejected on control pipe");
    direction.type = MessageType::PlaybackCommandResult;
    direction.source = ProcessRole::Ui;
    direction.destination = ProcessRole::Protocol;
    check(validateMessageDirection(
              ConnectionChannel::Playback, direction, error),
          "PlaybackCommandResult accepted on playback pipe");
    direction.type = MessageType::UiEndCastRequest;
    check(validateMessageDirection(
              ConnectionChannel::Playback, direction, error),
          "UiEndCastRequest accepted on playback pipe");
    check(!validateMessageDirection(
              ConnectionChannel::Control, direction, error),
          "UiEndCastRequest rejected on control pipe");
    direction.type = MessageType::PlaybackUiReady;
    check(validateMessageDirection(
              ConnectionChannel::Playback, direction, error),
          "PlaybackUiReady accepted on playback pipe");
    check(!validateMessageDirection(
              ConnectionChannel::Control, direction, error),
          "PlaybackUiReady rejected on control pipe");
    direction.type = MessageType::PlaybackSnapshotRequest;
    direction.source = ProcessRole::Protocol;
    direction.destination = ProcessRole::Ui;
    check(validateMessageDirection(
              ConnectionChannel::Playback, direction, error),
          "PlaybackSnapshotRequest accepted on playback pipe");
    direction.type = MessageType::PlaybackSnapshotResponse;
    direction.source = ProcessRole::Ui;
    direction.destination = ProcessRole::Protocol;
    check(validateMessageDirection(
              ConnectionChannel::Playback, direction, error),
          "PlaybackSnapshotResponse accepted on playback pipe");

    const mcast::ipc::PeerIdentityGrant identityGrant{
        ProcessRole::Ui,
        GetCurrentProcessId(),
        hello.buildId,
        hello.instanceNonce,
        hello.executableHash,
        2,
    };
    const auto grantMessage = mcast::ipc::makePeerIdentityGrantMessage(
        ProcessRole::Protocol, 4, 2, identityGrant, error);
    check(grantMessage.has_value(), "peer identity grant created: " + error);
    if (grantMessage) {
        const auto decodedGrant =
            mcast::ipc::decodePeerIdentityGrantMessage(
                *grantMessage, error);
        check(decodedGrant.has_value(),
              "peer identity grant decoded: " + error);
        if (decodedGrant) {
            checkEqual(decodedGrant->processId, identityGrant.processId,
                       "grant PID");
            checkEqual(decodedGrant->activationId,
                       identityGrant.activationId, "grant activation id");
        }
    }

    const mcast::ipc::UiActivateDecisionInfo activationDecision{true, 2};
    const auto decisionMessage =
        mcast::ipc::makeUiActivateDecisionMessage(
            3, 2, 1, activationDecision, error);
    check(decisionMessage.has_value(),
          "Ui activation decision created: " + error);
    if (decisionMessage) {
        const auto decodedDecision =
            mcast::ipc::decodeUiActivateDecisionMessage(
                *decisionMessage, 2, error);
        check(decodedDecision.has_value(),
              "Ui activation decision decoded: " + error);
        if (decodedDecision) {
            checkEqual(decodedDecision->started, true,
                       "decision started state");
            checkEqual(decodedDecision->activationId, 2,
                       "decision activation id");
        }
    }

    HelloInfo invalid = hello;
    invalid.instanceNonce.clear();
    check(!encodeHelloPayload(invalid, error).has_value(),
          "Hello rejects empty nonce");
}

void testCastConfirmationSchema()
{
    using namespace mcast::ipc;

    const CastConfirmationRequestInfo request{
        mcast::cast::ProtocolKind::AirPlay,
        42,
        "peer-42",
        "Living Room Phone",
        "192.0.2.42",
        "Example Movie",
        9000,
    };
    std::string error;
    const auto requestMessage = makeCastConfirmationRequestMessage(
        100, request, error);
    check(requestMessage.has_value(),
          "cast confirmation request created: " + error);
    if (requestMessage) {
        const auto frame = encodeFrame(*requestMessage);
        check(!frame.empty(), "cast confirmation request frame encoded");
        const auto decodedFrame = decodeFrame(frame);
        check(decodedFrame.status == DecodeStatus::Ok,
              "cast confirmation request survives frame decode");
        check(decodedFrame.message.header.type
                  == MessageType::CastConfirmationRequest,
              "cast confirmation request keeps message type");
        const auto decoded = decodeCastConfirmationRequestMessage(
            decodedFrame.message, error);
        check(decoded.has_value(),
              "cast confirmation request decoded: " + error);
        if (decoded) {
            checkEqual(
                static_cast<int>(decoded->protocol),
                static_cast<int>(mcast::cast::ProtocolKind::AirPlay),
                "cast confirmation protocol");
            checkEqual(decoded->claimId, request.claimId,
                       "cast confirmation claim id");
            checkEqual(decoded->peerDisplayName, request.peerDisplayName,
                       "cast confirmation peer name");
            checkEqual(decoded->mediaTitle, request.mediaTitle,
                       "cast confirmation media title");
            checkEqual(decoded->remainingTimeoutMs,
                       request.remainingTimeoutMs,
                       "cast confirmation timeout");
        }

        Message wrongDirection = *requestMessage;
        wrongDirection.header.source = ProcessRole::Shell;
        check(!decodeCastConfirmationRequestMessage(
                  wrongDirection, error).has_value(),
              "cast confirmation request rejects Shell source");

        Message oldVersion = *requestMessage;
        oldVersion.header.versionMinor = 0;
        check(!decodeCastConfirmationRequestMessage(
                  oldVersion, error).has_value(),
              "cast confirmation request requires IPC minor 1");
        Message wrongSession = *requestMessage;
        wrongSession.header.sessionId = 1;
        check(!decodeCastConfirmationRequestMessage(
                  wrongSession, error).has_value(),
              "cast confirmation request rejects candidate session id");
        Message futureVersion = *requestMessage;
        futureVersion.header.versionMinor = kProtocolVersionMinor + 1;
        checkEqual(
            decodeFrame(encodeFrame(futureVersion)).status,
            DecodeStatus::UnsupportedVersion,
            "frame decoder rejects unknown future minor");
    }

    const CastConfirmationDecisionInfo decision{
        mcast::cast::ProtocolKind::AirPlay,
        42,
        true,
    };
    const auto decisionMessage = makeCastConfirmationDecisionMessage(
        101, requestMessage ? requestMessage->header.messageId : 100,
        decision, error);
    check(decisionMessage.has_value(),
          "cast confirmation decision created: " + error);
    if (decisionMessage) {
        const auto decodedFrame = decodeFrame(encodeFrame(*decisionMessage));
        check(decodedFrame.status == DecodeStatus::Ok
                  && decodedFrame.message.header.type
                      == MessageType::CastConfirmationDecision,
              "cast confirmation decision survives frame decode");
        const auto decoded = decodeCastConfirmationDecisionMessage(
            decodedFrame.message, 100, error);
        check(decoded.has_value(),
              "cast confirmation decision decoded: " + error);
        if (decoded) {
            check(decoded->accepted,
                  "cast confirmation decision keeps accepted flag");
            checkEqual(decoded->claimId, decision.claimId,
                       "cast confirmation decision claim id");
        }
        check(!decodeCastConfirmationDecisionMessage(
                  *decisionMessage, 99, error).has_value(),
              "cast confirmation decision rejects wrong correlation");
        check(!decodeCastConfirmationDecisionMessage(
                  *decisionMessage, 0, error).has_value(),
              "cast confirmation decision rejects zero expected correlation");
    }

    const auto uiRequestMessage = makeUiCastConfirmationRequestMessage(
        200, requestMessage ? requestMessage->header.messageId : 100,
        request, error);
    check(uiRequestMessage.has_value(),
          "UI cast confirmation request created: " + error);
    if (uiRequestMessage) {
        const auto frame = decodeFrame(encodeFrame(*uiRequestMessage));
        check(frame.status == DecodeStatus::Ok
                  && frame.message.header.type
                      == MessageType::UiCastConfirmationRequest,
              "UI cast confirmation request survives frame decode");
        const auto decoded = decodeUiCastConfirmationRequestMessage(
            frame.message, 100, error);
        check(decoded.has_value(),
              "UI cast confirmation request decoded: " + error);
        if (decoded) {
            checkEqual(decoded->claimId, request.claimId,
                       "UI cast confirmation request claim id");
        }
        Message oldVersion = *uiRequestMessage;
        oldVersion.header.versionMinor = 1;
        check(!decodeUiCastConfirmationRequestMessage(
                  oldVersion, 100, error).has_value(),
              "UI cast confirmation requires IPC minor 2");
    }

    const auto uiDecisionMessage = makeUiCastConfirmationDecisionMessage(
        201, uiRequestMessage ? uiRequestMessage->header.messageId : 200,
        decision, error);
    check(uiDecisionMessage.has_value(),
          "UI cast confirmation decision created: " + error);
    if (uiDecisionMessage) {
        const auto frame = decodeFrame(encodeFrame(*uiDecisionMessage));
        check(frame.status == DecodeStatus::Ok
                  && frame.message.header.type
                      == MessageType::UiCastConfirmationDecision,
              "UI cast confirmation decision survives frame decode");
        const auto decoded = decodeUiCastConfirmationDecisionMessage(
            frame.message, 200, error);
        check(decoded.has_value(),
              "UI cast confirmation decision decoded: " + error);
    }

    const auto uiCancelMessage = makeUiCastConfirmationCancelMessage(
        202, uiRequestMessage ? uiRequestMessage->header.messageId : 200,
        error);
    check(uiCancelMessage.has_value(),
          "UI cast confirmation cancel created: " + error);
    if (uiCancelMessage) {
        checkEqual(
            decodeFrame(encodeFrame(*uiCancelMessage)).status,
            DecodeStatus::Ok,
            "UI cast confirmation cancel survives frame decode");
        check(decodeUiCastConfirmationCancelMessage(
                  *uiCancelMessage, error).has_value(),
              "UI cast confirmation cancel schema decodes");
        Message oldVersion = *uiCancelMessage;
        oldVersion.header.versionMinor = 1;
        check(!decodeUiCastConfirmationCancelMessage(
                  oldVersion, error).has_value(),
              "UI cast confirmation cancel requires IPC minor 2");
    }

    auto invalid = request;
    invalid.protocol = mcast::cast::ProtocolKind::None;
    check(!makeCastConfirmationRequestMessage(1, invalid, error).has_value(),
          "cast confirmation rejects None protocol");
    invalid = request;
    invalid.remainingTimeoutMs = 0;
    check(!makeCastConfirmationRequestMessage(1, invalid, error).has_value(),
          "cast confirmation rejects zero timeout");
    invalid = request;
    invalid.peerId.clear();
    check(!makeCastConfirmationRequestMessage(1, invalid, error).has_value(),
          "cast confirmation rejects empty peer id");
    invalid = request;
    invalid.peerDisplayName = "\xC0\xAF";
    check(!makeCastConfirmationRequestMessage(1, invalid, error).has_value(),
          "cast confirmation rejects invalid UTF-8 text");
    invalid = request;
    invalid.mediaTitle = "line\nbreak";
    check(!makeCastConfirmationRequestMessage(1, invalid, error).has_value(),
          "cast confirmation rejects control characters");
    invalid = request;
    invalid.mediaTitle = "\xE2\x80\xAE";
    check(!makeCastConfirmationRequestMessage(1, invalid, error).has_value(),
          "cast confirmation rejects bidi controls");

    MessageHeader direction;
    direction.type = MessageType::CastConfirmationRequest;
    direction.source = ProcessRole::Protocol;
    direction.destination = ProcessRole::Shell;
    check(validateMessageDirection(
              ConnectionChannel::Control, direction, error),
          "cast confirmation request direction accepted");
    check(!validateMessageDirection(
              ConnectionChannel::Playback, direction, error),
          "cast confirmation request rejected on playback pipe");
    direction.type = MessageType::CastConfirmationDecision;
    direction.source = ProcessRole::Shell;
    direction.destination = ProcessRole::Protocol;
    check(validateMessageDirection(
              ConnectionChannel::Control, direction, error),
          "cast confirmation decision direction accepted");
    direction.type = MessageType::UiCastConfirmationRequest;
    direction.source = ProcessRole::Shell;
    direction.destination = ProcessRole::Ui;
    check(validateMessageDirection(
              ConnectionChannel::Control, direction, error),
          "UI cast confirmation request direction accepted");
    direction.type = MessageType::UiCastConfirmationDecision;
    direction.source = ProcessRole::Ui;
    direction.destination = ProcessRole::Shell;
    check(validateMessageDirection(
              ConnectionChannel::Control, direction, error),
          "UI cast confirmation decision direction accepted");
    direction.type = MessageType::UiCastConfirmationCancel;
    direction.source = ProcessRole::Shell;
    direction.destination = ProcessRole::Ui;
    check(validateMessageDirection(
              ConnectionChannel::Control, direction, error),
          "UI cast confirmation cancel direction accepted");
}

void testPipeRoundTrip()
{
    using namespace mcast::ipc;

    const std::wstring instance =
        L"test-" + std::to_wstring(GetCurrentProcessId());
    const std::wstring name = makeControlPipeName(instance);
    check(!name.empty(), "control pipe name");

    PipeListener listener(name);
    check(listener.valid(), "pipe listener valid");

    std::atomic_bool clientOk{ false };
    std::string clientError;
    std::jthread client([&] {
        auto connection = PipeClient::connect(
            name, std::chrono::milliseconds(3000), clientError);
        if (!connection)
            return;

        PayloadBuilder hello;
        hello.addString(1, "protocol");
        const auto helloPayload = std::move(hello).finish();
        if (!helloPayload)
            return;
        Message request = messageOf(MessageType::Hello, *helloPayload);
        if (!connection->writeMessage(
                request, std::chrono::milliseconds(1000), clientError))
            return;

        Message reply;
        if (!connection->readMessage(
                reply, std::chrono::milliseconds(1000), clientError))
            return;
        if (reply.header.type != MessageType::HelloAck)
            return;
        clientOk = true;
    });

    std::string serverError;
    auto server = listener.accept(
        std::chrono::milliseconds(1000), serverError);
    check(server != nullptr, "server accepts client: " + serverError);
    if (server) {
        Message request;
        check(server->readMessage(
                  request, std::chrono::milliseconds(1000), serverError),
              "server reads hello: " + serverError);

        Message reply;
        reply.header = request.header;
        reply.header.type = MessageType::HelloAck;
        reply.header.flags = MessageFlags::Response;
        reply.header.source = ProcessRole::Shell;
        reply.header.destination = ProcessRole::Protocol;
        reply.header.correlationId = request.header.messageId;
        check(server->writeMessage(
                  reply, std::chrono::milliseconds(1000), serverError),
              "server writes hello ack: " + serverError);
    }

    client.join();
    check(clientOk, "pipe client completed: " + clientError);
}

void testClientWaitsForServer()
{
    using namespace mcast::ipc;
    using namespace std::chrono_literals;

    const std::wstring instance =
        L"late-server-" + std::to_wstring(GetCurrentProcessId());
    const std::wstring name = makeControlPipeName(instance);
    std::atomic_bool connected{ false };
    std::string clientError;
    std::jthread client([&] {
        auto connection = PipeClient::connect(name, 2000ms, clientError);
        connected = connection != nullptr;
    });

    std::this_thread::sleep_for(100ms);
    PipeListener listener(name);
    std::string serverError;
    auto server = listener.accept(2000ms, serverError);
    check(server != nullptr, "late server accepts waiting client: " + serverError);
    client.join();
    check(connected, "client retries file-not-found: " + clientError);
}

void testPipeTimeouts()
{
    using namespace mcast::ipc;
    using namespace std::chrono_literals;

    const std::wstring instance =
        L"timeout-" + std::to_wstring(GetCurrentProcessId());
    PipeListener listener(makeControlPipeName(instance));
    std::string error;
    const auto started = std::chrono::steady_clock::now();
    auto none = listener.accept(100ms, error);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    check(!none, "accept times out without client");
    check(elapsed < 2s, "accept timeout is bounded");

    std::string clientError;
    std::jthread client([&] {
        auto connection = PipeClient::connect(
            makeControlPipeName(instance), 1000ms, clientError);
        if (!connection)
            return;
        std::this_thread::sleep_for(500ms);
    });
    auto server = listener.accept(1000ms, error);
    check(server != nullptr, "server accepts timeout client: " + error);
    if (server) {
        Message message;
        const auto readStart = std::chrono::steady_clock::now();
        check(!server->readMessage(message, 100ms, error),
              "read times out on silent peer");
        check(std::chrono::steady_clock::now() - readStart < 2s,
              "read timeout is bounded");
    }
    client.join();
}

void testSlowHeaderDeadline()
{
    using namespace mcast::ipc;
    using namespace std::chrono_literals;

    const std::wstring instance =
        L"slow-header-" + std::to_wstring(GetCurrentProcessId());
    const std::wstring name = makeControlPipeName(instance);
    PipeListener listener(name);
    std::string clientError;
    std::jthread client([&] {
        HANDLE handle = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 100; ++attempt) {
            handle = CreateFileW(
                name.c_str(), GENERIC_READ | GENERIC_WRITE,
                0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle != INVALID_HANDLE_VALUE)
                break;
            std::this_thread::sleep_for(10ms);
        }
        if (handle == INVALID_HANDLE_VALUE)
            return;
        const std::uint8_t byte = 0;
        for (int index = 0; index < kHeaderSize + 16; ++index) {
            DWORD written = 0;
            if (!WriteFile(handle, &byte, 1, &written, nullptr)
                || written != 1)
                break;
            std::this_thread::sleep_for(30ms);
        }
        CloseHandle(handle);
    });

    std::string serverError;
    auto server = listener.accept(1000ms, serverError);
    check(server != nullptr, "slow client accepted: " + serverError);
    if (server) {
        Message message;
        const auto started = std::chrono::steady_clock::now();
        check(!server->readMessage(message, 120ms, serverError),
              "slow fragmented header times out");
        const auto elapsed = std::chrono::steady_clock::now() - started;
        check(elapsed < 400ms,
              "read timeout is total deadline, not per fragment");
    }
    client.join();
}

void testConcurrentCloseAndListenerShutdown()
{
    using namespace mcast::ipc;
    using namespace std::chrono_literals;

    const std::wstring instance =
        L"close-race-" + std::to_wstring(GetCurrentProcessId());
    const std::wstring name = makeControlPipeName(instance);
    PipeListener listener(name);
    std::string clientError;
    std::jthread client([&] {
        auto connection = PipeClient::connect(name, 1000ms, clientError);
        if (connection)
            std::this_thread::sleep_for(500ms);
    });
    std::string error;
    auto server = listener.accept(1000ms, error);
    check(server != nullptr, "close race server accepts: " + error);
    if (server) {
        std::atomic_bool readReturned{ false };
        std::jthread reader([&] {
            Message message;
            std::string readError;
            server->readMessage(message, 5000ms, readError);
            readReturned = true;
        });
        std::this_thread::sleep_for(50ms);
        const auto started = std::chrono::steady_clock::now();
        server->close();
        reader.join();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        check(readReturned, "close interrupts blocking read");
        check(elapsed < 1000ms, "close and cancel complete promptly");
        Message afterClose;
        const auto afterCloseStart = std::chrono::steady_clock::now();
        check(!server->readMessage(afterClose, 5000ms, error),
              "read started after close is rejected");
        check(std::chrono::steady_clock::now() - afterCloseStart < 100ms,
              "closing flag prevents new I/O");
    }
    client.join();

    const std::wstring acceptName = makeControlPipeName(
        instance + L"-accept");
    auto pendingListener = std::make_unique<PipeListener>(acceptName);
    std::atomic_bool acceptReturned{ false };
    std::atomic_bool acceptSucceeded{ false };
    PipeListener *listenerPtr = pendingListener.get();
    std::jthread acceptor([&] {
        std::string acceptError;
        auto connection = listenerPtr->accept(5000ms, acceptError);
        acceptSucceeded = connection != nullptr;
        acceptReturned = true;
    });
    std::this_thread::sleep_for(50ms);
    const auto started = std::chrono::steady_clock::now();
    listenerPtr->stop();
    acceptor.join();
    check(acceptReturned, "listener stop wakes accept");
    check(!acceptSucceeded, "listener stop returns no connection");
    check(std::chrono::steady_clock::now() - started < 1000ms,
          "listener stop is bounded");
    pendingListener.reset();

    PipeListener stoppedListener(makeControlPipeName(
        instance + L"-stopped-before-accept"));
    stoppedListener.stop();
    const auto stoppedStart = std::chrono::steady_clock::now();
    auto stoppedConnection = stoppedListener.accept(5000ms, error);
    check(!stoppedConnection, "stopped listener never accepts");
    check(std::chrono::steady_clock::now() - stoppedStart < 100ms,
          "stop-before-accept does not wait timeout");
}

void testRestartPolicy()
{
    using namespace mcast::supervisor;
    using namespace std::chrono_literals;

    RestartPolicy policy;
    policy.maxAttempts = 3;
    policy.initialDelay = 250ms;
    policy.maxDelay = 1000ms;
    policy.stableReset = 30s;

    RestartController controller(policy);
    const auto start = std::chrono::steady_clock::time_point{} + 1s;
    controller.markStarted(start);
    auto first = controller.markExited(start + 1s);
    auto second = controller.markExited(start + 2s);
    auto third = controller.markExited(start + 3s);
    auto fourth = controller.markExited(start + 4s);
    check(first && first->delay == 250ms, "first restart delay");
    check(second && second->delay == 500ms, "second restart delay");
    check(third && third->delay == 1000ms, "third restart delay");
    check(!fourth, "restart budget exhausts");

    controller.markStarted(start + 5s);
    auto afterStable = controller.markExited(start + 40s);
    check(afterStable && afterStable->attempt == 1,
          "stable run resets restart budget");
    check(afterStable
              && controller.isCurrent(afterStable->generation),
          "restart plan carries current generation");
    if (afterStable)
        controller.cancelPendingRestart();
    check(afterStable
              && !controller.isCurrent(afterStable->generation),
          "cancel invalidates pending restart");

    RestartPolicy invalid;
    invalid.initialDelay = -5ms;
    invalid.maxDelay = -10ms;
    invalid.stableReset = -20s;
    RestartController sanitized(invalid);
    check(sanitized.policy().initialDelay > 0ms,
          "invalid delay is sanitized");
    check(sanitized.policy().maxDelay >= sanitized.policy().initialDelay,
          "max delay is normalized");
    check(sanitized.policy().stableReset > 0s,
          "invalid stable reset is sanitized");
}

} // namespace

int main()
{
    testPayloadRoundTrip();
    testFrameRoundTrip();
    testInvalidFrames();
    testHelloSchema();
    testCastConfirmationSchema();
    testPipeRoundTrip();
    testClientWaitsForServer();
    testPipeTimeouts();
    testSlowHeaderDeadline();
    testConcurrentCloseAndListenerShutdown();
    testRestartPolicy();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All IPC tests passed\n";
    return EXIT_SUCCESS;
}
