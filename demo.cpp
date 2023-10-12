#include <mutex>
#include <queue>
#include <format>
#include <thread>
#include <iostream>

#include <time.h>
#include <signal.h>
#include <sys/types.h>

#ifdef _WIN32
    #include <ws2tcpip.h>
    #pragma comment(lib, "ws2_32.lib")
#else
    #include <netdb.h>
    #include <unistd.h>
    #include <arpa/inet.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
#endif

#include <opencv2/opencv.hpp>

#define FRAME_CNT    (20)
#define FRAME_PKT    (1442)
#define FRAME_SEQ    (2880)

#define FRAME_WIDTH  (1920)
#define FRAME_HEIGHT (1080)

#define LISTEN_ADDR  "192.168.1.102"
#define LISTEN_PORT  (8001)

bool running = false;

std::mutex           frame_mutex;
std::queue<cv::Mat>  frame_queue;
std::queue<uint16_t> count_queue;

#ifndef _WIN32
    void signal_handle(int signum)
    {
        switch (signum) {
        case SIGINT:
        case SIGTERM:
            running = false;
            break;
        default:
            break;
        }
    }
#endif

void rgb565_bgr888(uint8_t *dst, const uint8_t *src, int size)
{
    for (int i = 0; i < size; i++) {
        uint16_t rgb565 = *src++ | (*src++ << 8); // (G[5:2] ... B[7:3]) | (R[7:3] ... G[7:5]) << 8

    #ifdef _WIN32
        rgb565 = ntohs(rgb565);
    #endif

        dst[i * 3 + 0] = (rgb565 & 0x001f) << 3; // B
        dst[i * 3 + 1] = (rgb565 & 0x07e0) >> 3; // G
        dst[i * 3 + 2] = (rgb565 & 0xf800) >> 8; // R
    }
}

void t1_recvframe(void)
{
    int ret = 0;
    int sock_fd = 0;
    struct sockaddr_in sock_addr = { 0 };
    socklen_t sock_opt = 16 * 1024 * 1024;
    socklen_t sock_len = sizeof(sockaddr_in);

    uint16_t pkt_idx = 0;
    uint8_t  pkt_buff[FRAME_PKT] = {0};

    uint16_t count_curr = 0;
    bool     frame_sync = false;
    cv::Mat  frame_buff(cv::Size(FRAME_WIDTH, FRAME_HEIGHT), CV_8UC3, cv::Scalar(0, 0, 0));

    std::cout << "T1: 视频接收线程...启动" << std::endl;

    sock_addr.sin_family = AF_INET;
    sock_addr.sin_port = htons(LISTEN_PORT);

    inet_pton(sock_addr.sin_family, LISTEN_ADDR, &sock_addr.sin_addr);

#ifdef _WIN32
    WSADATA wsa_data = { 0 };
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cout << "T1: 故障！套接字未初始化" << std::endl;
        goto err_t1;
    }
#endif

    if ((sock_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        std::cout << "T1: 故障！无法创建套接字" << std::endl;
        goto err_t1;
    }

    if ((ret = ::setsockopt(sock_fd, SOL_SOCKET, SO_RCVBUF, (const char *)&sock_opt, sizeof(sock_opt))) < 0) {
        std::cout << "T1: 故障！无法设定缓冲区大小" << std::endl;
        goto err_t1s;
    };

    if ((ret = ::bind(sock_fd, (const sockaddr *)&sock_addr, sizeof(sock_addr))) < 0) {
        std::cout << "T1: 故障！无法绑定到指定端口" << std::endl;
        goto err_t1s;
    };

    while (running) {
        static uint16_t pkt_idx_prev = 0;

        frame_sync = false;
        count_curr = 0;

        while (true) {
            if ((ret = ::recvfrom(sock_fd, (char *)pkt_buff, FRAME_PKT, 0, (sockaddr *)&sock_addr, &sock_len)) < 0) {
                std::cout << "T1: 注意！无法读取套接字" << std::endl;
                goto err_t1s;
            }

            pkt_idx_prev = pkt_idx;
            pkt_idx = pkt_buff[0] << 8 | pkt_buff[1];

            if ((pkt_idx == 0 && pkt_idx_prev != FRAME_SEQ - 1) ||
                (pkt_idx != 0 && pkt_idx_prev != pkt_idx - 1)) {
                std::cout << std::format("T1: 注意！丢包：{:04d} => {:04d}", pkt_idx_prev, pkt_idx) << std::endl;

                if (pkt_idx_prev >= pkt_idx) {
                    frame_sync = false;
                    count_curr = 0;
                }
            }

            if (pkt_idx == 0) {
                if (!frame_sync) {
                    frame_sync = true;
                }
            } else {
                if (!frame_sync) {
                    continue;
                }
            }

            count_curr++;

            if (pkt_idx < FRAME_SEQ) {
                rgb565_bgr888(frame_buff.data + pkt_idx * (FRAME_PKT - 2) * 3 / 2, pkt_buff + 2, (FRAME_PKT - 2) / 2);
            }

            if (frame_sync && (pkt_idx == FRAME_SEQ - 1)) {
                break;
            }
        }

        if (frame_queue.empty()) {
            frame_mutex.lock();
            count_queue.push(count_curr);
            frame_queue.push(frame_buff.clone());
            frame_mutex.unlock();
        }

        frame_buff.setTo(cv::Scalar(0, 0, 0));
    }

err_t1s:
#ifdef _WIN32
    closesocket(sock_fd);

    WSACleanup();
#else
    close(sock_fd);
#endif

err_t1:
    running = false;

    std::cout << "T1: 视频接收线程...关闭" << std::endl;
}

void t2_showframe(void)
{
    uint16_t update = 0;
    uint16_t count_curr = 0;
    uint64_t fps = 0, fps_sum = 0;
    uint64_t err = 0, err_sum = 0;
    struct timespec start  = {0, 0};
    struct timespec finish = {0, 0};
    cv::Mat frame_buff(cv::Size(FRAME_WIDTH, FRAME_HEIGHT), CV_8UC3, cv::Scalar(0, 0, 0));

    std::cout << "T2: 视频显示线程...启动" << std::endl;

    cv::namedWindow("Frame", cv::WINDOW_NORMAL);
    cv::setWindowProperty("Frame", cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);

    while (running) {
    #ifndef _WIN32
        clock_gettime(CLOCK_REALTIME, &start);
    #endif

        if (!frame_queue.empty()) {
        #ifndef _WIN32
            clock_gettime(CLOCK_REALTIME, &finish);
        #endif

            frame_mutex.lock();
            count_curr = count_queue.front();
            frame_buff = frame_queue.front();
            count_queue.pop();
            frame_queue.pop();
            frame_mutex.unlock();

            if (count_curr != FRAME_SEQ) {
                std::cout << std::format("T2: 收到！丢帧：{:04d}", count_curr) << std::endl;
                continue;
            }

            fps_sum += (finish.tv_sec - start.tv_sec) * 1000 + (finish.tv_nsec - start.tv_nsec) / 1000000;
            err_sum += FRAME_SEQ - count_curr;

            if (update++ == FRAME_CNT + 2) {
                update = 0;

                if (fps_sum) {
                    fps = 10000 * FRAME_CNT / fps_sum;
                    fps_sum = 0;
                }

                err = err_sum * 10000 / FRAME_CNT / FRAME_SEQ;
                err_sum = 0;
            }

            std::string s = std::format("FPS:{:.1f}", fps / 10.0);
            cv::putText(frame_buff, s.c_str(), cv::Point(10, 30), cv::FONT_HERSHEY_COMPLEX, 1.0, cv::Scalar(0, 255, 0), 2, 8, 0);
            std::string e = std::format("ERR:{:.2f}%", err / 100.0);
            cv::putText(frame_buff, e.c_str(), cv::Point(10, 60), cv::FONT_HERSHEY_COMPLEX, 1.0, cv::Scalar(0, 0, 255), 2, 8, 0);

            cv::imshow("Frame", frame_buff);
            cv::pollKey();
        } else {
        #ifdef _WIN32
            Sleep(1);
        #else
            usleep(1000);
        #endif
            continue;
        }
    }

    cv::destroyWindow("Frame");

    std::cout << "T2: 视频显示线程...关闭" << std::endl;
}

int main(void)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#else
    signal(SIGINT, signal_handle);
    signal(SIGTERM, signal_handle);
#endif

    std::cout << "M : 视频处理进程...启动" << std::endl;

    running = true;

    std::thread t1(t1_recvframe);
    t1.detach();
    std::thread t2(t2_showframe);
    t2.join();

    if (t1.joinable()) {
        t1.join();
    }

    std::cout << "M : 视频处理进程...关闭" << std::endl;

    return 0;
}
