#pragma once
#include <cstddef>
#include <sys/types.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cerrno>
#include <sys/uio.h>
#include <unistd.h>


namespace novanet::net{

class Buffer{

public:
    static constexpr size_t kCheapPrepend = 8;
    
    static constexpr size_t kInitialSize = 1024;

    static_assert(kCheapPrepend >= 8 ,"NovaNet protocol header needs at least 8 bytes prepend space!");

    explicit Buffer(size_t initialiSize = kInitialSize) 
            :buffer_(kCheapPrepend + initialiSize),
            readerIndex_(kCheapPrepend),
            writerIndex_(kCheapPrepend){}

    size_t readableBytes() const{
        return writerIndex_ - readerIndex_;
    }

    size_t writeableBytes() const{
        return buffer_.size() - writerIndex_;
    }

    size_t prependableBytes() const{
        return readerIndex_;
    }


    const char* peek()const{
        return begin() + readerIndex_;
    }

    void retrieve(size_t len){
        assert(len <= readableBytes());
        if(len < readableBytes()){
            readerIndex_ += len;
        }else{
            retrieveAll();
        }
    }

    void retrieveAll(){
        readerIndex_ = kCheapPrepend;
        writerIndex_ = kCheapPrepend;
    }

    std::string retrieveAsString(size_t len){
        assert(len <= readableBytes);
        std::string res(peek(),len);
        retrieve(len);
        return res;
    }

    std::string retrieveAllAsString(){
        return retrieveAsString(readableBytes());
    }

    char* beginWrite() {return begin() + writerIndex_;}

    const char* beginWrite() const{return begin() + writerIndex_ ;}

    void append(const char* data,size_t len){
        ensureWritableBytes(len);
        std::copy(data,data+len,beginWrite());
        hasWritten(len);
    }

    void append(std::string& str){
        append(str.data(),str.size());
    }



    void ensureWritableBytes(size_t len){
        if(writeableBytes() < len){
            makeSpace(len);
        }
        assert(writeableBytes >= len);
    }

    void hasWritten(size_t len){
        assert(len <= writeableBytes());
        writerIndex_ += len;
    }

    ssize_t readFd(int fd,int* savedErrno){
        char extraBuf[65536];
        struct iovec vec[2];
        const size_t writeable = writeableBytes();
        vec[0].iov_base = beginWrite();
        vec[0].iov_len = writeable;

        vec[1].iov_base = extraBuf;
        vec[1].iov_len = sizeof(extraBuf);

        const int iov_cnt = writeable < sizeof(extraBuf) ? 2 : 1;

        const ssize_t n = ::readv(fd,vec,iov_cnt);

        if(n < 0){
            *savedErrno = errno;
        }else if(static_cast<size_t>(n) <= writeable){
            writerIndex_ += static_cast<size_t>(n);
        }else{
            writerIndex_ = buffer_.size();
            append(extraBuf,static_cast<size_t>(n) - writeable);
        }
        return n;
    }
    ssize_t writeFd(int fd,int* savedErrno){
        const size_t nreadable = readableBytes();

        ssize_t n = ::write(fd,peek(),nreadable);
        if(n < 0){
            *savedErrno = errno;
            return n;
        }
        retrieve(static_cast<size_t>(n));
        return n;
    }
private:
    char* begin(){return buffer_.data();}
    const char* begin() const {return buffer_.data();}
    
    void makeSpace(size_t len){
        if(writeableBytes() + prependableBytes() < len + kCheapPrepend ){
            buffer_.resize(writerIndex_ + len);
        }else{
            const size_t readable = readableBytes();
            /*
                平移后状态（碎片被消除，拼凑出了一大块连续的尾部空间）：
                [ kCheapPrepend ] [  有效可读数据  ] [          合并后的超大可写空间           ]
                0                 8                新writerIndex                          capacity

            */
            std::copy(begin() + readerIndex_,begin() + writerIndex_,begin() + kCheapPrepend);
            readerIndex_ = kCheapPrepend;
            writerIndex_ = readerIndex_ + readable;
            assert(readable == readableBytes());
        }
    }

private:
    std::vector<char> buffer_;
    size_t readerIndex_; //读游标
    size_t writerIndex_; //写游标
};


}//namespace novanet::net