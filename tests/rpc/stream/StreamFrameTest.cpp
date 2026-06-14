#include <cassert>
#include <string>

#include "novanet/rpc/stream/StreamFrame.h"

using novanet::rpc::FrameType;
using novanet::rpc::RpcMessage;
using novanet::rpc::StreamFrame;

int main() {
    {
        StreamFrame frame = StreamFrame::makeData(1, 1001, "chunk");

        assert(frame.valid());
        assert(frame.isStreamFrame());
        assert(frame.isData());
        assert(!frame.isEnd());
        assert(!frame.isCancel());
        assert(frame.streamId() == 1);
        assert(frame.requestId() == 1001);
        assert(frame.payload() == "chunk");
    }

    {
        StreamFrame frame = StreamFrame::makeEnd(2, 1002);

        assert(frame.valid());
        assert(frame.isStreamFrame());
        assert(frame.isEnd());
        assert(frame.payload().empty());
    }

    {
        StreamFrame frame = StreamFrame::makeCancel(3, 1003, "client cancel");

        assert(frame.valid());
        assert(frame.isCancel());
        assert(frame.payload() == "client cancel");
    }

    {
        StreamFrame frame = StreamFrame::makeOpen(4, 1004, "open-payload");

        assert(frame.valid());
        assert(frame.isOpen());
        assert(frame.payload() == "open-payload");
    }

    {
        /*
         * streamId = 0 对 stream frame 非法。
         */
        StreamFrame frame = StreamFrame::makeData(0, 1005, "bad");

        assert(!frame.valid());
        assert(frame.isData());
    }

    {
        /*
         * requestId = 0 也不建议允许。
         */
        StreamFrame frame = StreamFrame::makeData(1, 0, "bad");

        assert(!frame.valid());
        assert(frame.isData());
    }

    {
        /*
         * UNARY_REQUEST 不是 stream frame。
         */
        RpcMessage msg(FrameType::UNARY_REQUEST, 0, 1006, "payload");
        StreamFrame frame(msg);

        assert(!frame.isStreamFrame());
        assert(!frame.valid());
    }

    {
        /*
         * 包装已有 RpcMessage。
         */
        RpcMessage msg(FrameType::STREAM_DATA, 9, 9001, "wrapped");
        StreamFrame frame(msg);

        assert(frame.valid());
        assert(frame.isData());
        assert(frame.streamId() == 9);
        assert(frame.requestId() == 9001);
        assert(frame.payload() == "wrapped");
    }

    return 0;
}