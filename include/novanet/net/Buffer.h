#pragma once
#include <bits/types/struct_iovec.h>
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
    explicit Buffer(size_t initialSize = kInitialSize) 
                    :buffer_(kCheapPrepend + initialSize),
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

    //返回可读数据的首地址
    const char* peek() const{
        return begin() + readerIndex_;
    }

    // 消费/读取指定长度的数据，将 readerIndex_ 向后推进
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

    // 把数据作为 std::string 提取出来，并自动消费这部分数据
    std::string retrieveAsString(size_t len){
        assert(len <= readableBytes());
        std::string res (peek(),len);
        //数据被成功提取后，必须在底层数据结构中将其标记为“已读取”。
        retrieve(len);
        return res;
    }

    // 提取所有剩余的可读数据
    std::string retrieveAllAsString(){
        return retrieveAsString(readableBytes());
    }
    
    // 获取当前可写区域的起始指针
    char* beginWrite(){
        return begin() + writerIndex_;
    }
    const char* beginWrite () const{
        return begin() + writerIndex_;
    }

    void append(const char* data,size_t len){
        ensureWritableBytes(len); // 确保空间足够，不够会自动扩容或整理碎片
        std::copy(data,data+len,beginWrite());// 填入数据
        hasWritten(len);// 推进 writerIndex_
    }


    void append(const std::string& str){
        append(str.data(),str.size());
    }

    // 检查容量，不够则触发 makeSpace()
    void ensureWritableBytes(size_t len){
        if(writeableBytes() < len){
            makeSpace(len);
        }
        assert(writeableBytes >= len);
    }

    // 手动推进写入游标
    void hasWritten(size_t len){
        assert(len <= writeableBytes());
        writerIndex_ += len;
    }

    ssize_t readFd(int fd,int* savedErrno){

        char extraBuf[1024];
        struct iovec vec[2];

        const size_t wirtable = writeableBytes();
        vec[0].iov_base = beginWrite();
        vec[0].iov_len = wirtable;

        vec[1].iov_base = extraBuf;
        vec[1].iov_len = sizeof(extraBuf);

        const int iovcnt = wirtable < sizeof(extraBuf) ? 2 : 1;

        const ssize_t n = ::readv(fd,vec,iovcnt);

        if(n < 0){
            *savedErrno = errno;
        }else if(static_cast<size_t>(n) <= wirtable){
            writerIndex_ += static_cast<size_t>(n);
        }else{
            writerIndex_ = buffer_.size();
            append(extraBuf,static_cast<size_t>(n) - wirtable);
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
        retrieve(n);
        return n;
    }

    void makeSpace(size_t len){
        if(writeableBytes() + prependableBytes() < len + kCheapPrepend){
            buffer_.resize(writerIndex_ + len);
        }
    }

private:
    //获取底层 vector的首地址
    char* begin(){return buffer_.data();}
    const char* begin() const {return buffer_.data();}

private:
    /*
        | prependable | readable data | writable space |
        0       readerIndex_       writerIndex_      buffer_.size()
    */
    std::vector<char> buffer_;
    size_t readerIndex_;//读游标 : 前面有多少空间已被废弃，或预留
    size_t writerIndex_; //写游标 : 数据写到了哪里，其后为可用空间
};


}//namespace novanet::net