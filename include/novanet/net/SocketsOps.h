#pragma once
#include <netinet/in.h>
#include <sys/socket.h>

namespace novanet::net::sockets {

//--- 核心生命周期封装 (失败即退出) ---

int createNonblockingOrDie(sa_family_t family);

void bindOrDie(int sokcfd,const struct sockaddr* addr);

void listenOrDie(int sockfd);

// --- 核心 I/O 操作 ---
// accept 失败会返回负数,对 EAGAIN/EMFILE 做了错误分类

int accept(int sockfd, struct sockaddr_storage* addr);

ssize_t read(int sockfd,void* buf,size_t count);

ssize_t write(int sockfd,void* buf,size_t count);

void close(int sockfd);

void ignoreSigPipe();
void setReuseAddr(int sockfd,bool on);
void setReusePort(int sockfd,bool on);
void setTcpNoDelay(int sockfd,bool on);

void toIpPort(char* buf,size_t size,const struct sockaddr* addr);
void toIp(char* buf,size_t size,const struct sockaddr* addr);
void fromIpPort(const char* ip, uint16_t port,struct sockaddr_in* addr);
void fromIpPort(const char* ip, uint16_t port,struct sockaddr_in6* addr);

inline const struct sockaddr* sockaddr_cast(const struct sockaddr_in6* addr){
    return reinterpret_cast<const struct sockaddr*>(addr);
}
inline  struct sockaddr* sockaddr_cast(struct sockaddr_in6* addr){
    return reinterpret_cast<struct sockaddr*>(addr);
}
inline const struct sockaddr* sockaddr_cast(const struct sockaddr_in* addr){
    return reinterpret_cast<const struct sockaddr*>(addr);
}

inline const struct sockaddr_in* sockaddr_in_cast(const struct sockaddr* addr){
    return reinterpret_cast<const struct sockaddr_in*>(addr);
}

inline const struct sockaddr_in6* sockaddr_in6_cast(const struct sockaddr* addr){
    return reinterpret_cast<const struct sockaddr_in6*>(addr);
}

}  // namespace novanet::net::sockets