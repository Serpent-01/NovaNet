#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <cstring>

using namespace std;

// --- 极限压测配置 ---
const char* SERVER_IP = "127.0.0.1";
const int SERVER_PORT = 8080;
const int THREAD_COUNT = 8;         // 压测线程数
const int CONN_PER_THREAD = 500;    // 每个线程连接数（总并发 4000）
const int PIPELINE_SIZE = 10;       // 初始发送包量（管道深度）
const int BATCH_SEND = 3;           // 每次收到回包后再补发的包量
const int TEST_SECONDS = 60;
const char* PAYLOAD = "Ping";

std::atomic<long long> g_total_requests(0);

void setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void worker_thread() {
    int epfd = epoll_create1(0);
    vector<int> fds;
    int payload_len = strlen(PAYLOAD);

    for (int i = 0; i < CONN_PER_THREAD; ++i) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        struct sockaddr_in serv_addr;
        serv_addr.sin_family = AF_INET;
        serv_addr.sin_port = htons(SERVER_PORT);
        inet_pton(AF_INET, SERVER_IP, &serv_addr.sin_addr);

        if (connect(sock, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            continue; 
        }

        setNonBlocking(sock);
        
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = sock;
        epoll_ctl(epfd, EPOLL_CTL_ADD, sock, &ev);
        
        fds.push_back(sock);

        // --- 核心改动：初始流水线发射 ---
        for(int p = 0; p < PIPELINE_SIZE; ++p) {
            send(sock, PAYLOAD, payload_len, 0);
        }
    }

    struct epoll_event events[1024];
    char buf[4096];

    auto start_time = chrono::steady_clock::now();
    while (true) {
        int n = epoll_wait(epfd, events, 1024, 5);
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            if (events[i].events & EPOLLIN) {
                // 读取所有可用数据
                while (read(fd, buf, sizeof(buf)) > 0) {
                    g_total_requests.fetch_add(1, std::memory_order_relaxed);
                    
                    // --- 核心改动：持续补弹 ---
                    // 每读到一个包，立刻疯狂补发，让服务端缓冲区永远不空
                    for(int s = 0; s < BATCH_SEND; ++s) {
                        send(fd, PAYLOAD, payload_len, 0);
                    }
                }
            }
        }
        
        auto now = chrono::steady_clock::now();
        if (chrono::duration_cast<chrono::seconds>(now - start_time).count() >= TEST_SECONDS) break;
    }

    for (int fd : fds) close(fd);
    close(epfd);
}

int main() {
    cout << "FIRE!! [Pipelining Mode]" << endl;
    cout << "Total Conns: " << THREAD_COUNT * CONN_PER_THREAD << endl;

    vector<thread> threads;
    auto start_time = chrono::steady_clock::now();

    for (int i = 0; i < THREAD_COUNT; ++i) {
        threads.emplace_back(worker_thread);
    }

    for (int i = 0; i < TEST_SECONDS; ++i) {
        this_thread::sleep_for(chrono::seconds(1));
        cout << "Step " << i+1 << " - QPS: " << g_total_requests.load() / (i + 1) << endl;
    }

    for (auto& t : threads) t.join();
    auto end_time = chrono::steady_clock::now();
    auto duration = chrono::duration_cast<chrono::milliseconds>(end_time - start_time).count();

    cout << "\n--- ULTIMATE LIMIT RESULT ---" << endl;
    cout << "Final Avg QPS: " << (double)g_total_requests.load() / (duration / 1000.0) << " req/s" << endl;

    return 0;
}