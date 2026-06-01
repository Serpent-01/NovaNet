#include <cassert>
#include <iostream>
#include <string>

#include "rpc_meta.pb.h"

int main() {
    using namespace novanet::rpc;
    using namespace novanet::rpc::meta;
    // ================================
    // 1. 测试 UnaryRequestMeta
    // ================================
    UnaryRequestMeta requestMeta;
    requestMeta.set_service_name("CalculatorService");
    requestMeta.set_method_name("Add");
    requestMeta.set_request_payload("fake_add_request_bytes");

    (*requestMeta.mutable_metadata())["trace_id"] = "trace-001";
    (*requestMeta.mutable_metadata())["client"] = "RpcMetaSmokeTest";

    std::string requestBytes;
    const bool requestSerialized = requestMeta.SerializeToString(&requestBytes);
    assert(requestSerialized);
    assert(!requestBytes.empty());

    UnaryRequestMeta parsedRequest;
    const bool requestParsed = parsedRequest.ParseFromString(requestBytes);
    assert(requestParsed);

    assert(parsedRequest.service_name() == "CalculatorService");
    assert(parsedRequest.method_name() == "Add");
    assert(parsedRequest.request_payload() == "fake_add_request_bytes");
    assert(parsedRequest.metadata().at("trace_id") == "trace-001");
    assert(parsedRequest.metadata().at("client") == "RpcMetaSmokeTest");

    // ================================
    // 2. 测试 UnaryResponseMeta
    // ================================
    UnaryResponseMeta responseMeta;
    responseMeta.set_error_code(RPC_OK);
    responseMeta.set_error_text("");
    responseMeta.set_response_payload("fake_add_response_bytes");

    std::string responseBytes;
    const bool responseSerialized =
        responseMeta.SerializeToString(&responseBytes);
    assert(responseSerialized);
    assert(!responseBytes.empty());

    UnaryResponseMeta parsedResponse;
    const bool responseParsed = parsedResponse.ParseFromString(responseBytes);
    assert(responseParsed);

    assert(parsedResponse.error_code() == RPC_OK);
    assert(parsedResponse.error_text().empty());
    assert(parsedResponse.response_payload() == "fake_add_response_bytes");

    // ================================
    // 3. 测试 StreamOpenMeta
    // ================================
    StreamOpenMeta streamOpen;
    streamOpen.set_service_name("ChatService");
    streamOpen.set_method_name("Generate");
    streamOpen.set_request_payload("fake_generate_request_bytes");
    streamOpen.set_initial_window_bytes(64 * 1024);

    std::string streamOpenBytes;
    const bool streamOpenSerialized =
        streamOpen.SerializeToString(&streamOpenBytes);
    assert(streamOpenSerialized);
    assert(!streamOpenBytes.empty());

    StreamOpenMeta parsedStreamOpen;
    const bool streamOpenParsed =
        parsedStreamOpen.ParseFromString(streamOpenBytes);
    assert(streamOpenParsed);

    assert(parsedStreamOpen.service_name() == "ChatService");
    assert(parsedStreamOpen.method_name() == "Generate");
    assert(parsedStreamOpen.request_payload() == "fake_generate_request_bytes");
    assert(parsedStreamOpen.initial_window_bytes() == 64 * 1024);

    // ================================
    // 4. 测试 StreamDataMeta
    // ================================
    StreamDataMeta streamData;
    streamData.set_sequence(1);
    streamData.set_data("hello");

    std::string streamDataBytes;
    const bool streamDataSerialized =
        streamData.SerializeToString(&streamDataBytes);
    assert(streamDataSerialized);
    assert(!streamDataBytes.empty());

    StreamDataMeta parsedStreamData;
    const bool streamDataParsed =
        parsedStreamData.ParseFromString(streamDataBytes);
    assert(streamDataParsed);

    assert(parsedStreamData.sequence() == 1);
    assert(parsedStreamData.data() == "hello");

    std::cout << "[PASS] RpcMetaSmokeTest passed.\n";
    return 0;
}