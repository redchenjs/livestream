#include <mutex>
#include <queue>
#include <format>
#include <thread>
#include <iostream>

#include <time.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
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

using namespace std;

#define FRAME_DLY     (20)
#define FRAME_PKT     (1442)
#define FRAME_SEQ     (1280)

#define FRAME_WIDTH   (1280)
#define FRAME_HEIGHT  ( 720)

#define COLOR_WHITE   (0xffffff)
#define COLOR_BLACK   (0x000000)
#define COLOR_BLUE    (0xff0000)
#define COLOR_GREEN   (0x00ff00)
#define COLOR_RED     (0x0000ff)
#define COLOR_CYAN    (0xffff00)
#define COLOR_YELLOW  (0x00ffff)
#define COLOR_MAGENTA (0xff00ff)

#define TARGET_ADDR   "127.0.0.1"
#define TARGET_PORT   (8001)

bool running = false;

std::mutex          frame_mutex;
std::queue<cv::Mat> frame_queue;

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

void bgr888_rgb565(uint8_t *dst, const uint8_t *src, int size)
{
    for (int i = 0; i < size; i++) {
        uint8_t b = *src++; // B
        uint8_t g = *src++; // G
        uint8_t r = *src++; // R

        dst[i * 2 + 0] = ((r >> 3) << 3) | (g >> 5); // R[7:3] ... G[7:5]
        dst[i * 2 + 1] = ((g >> 2) << 5) | (b >> 3); // G[5:2] ... B[7:3]
    }
}

void print_test_pattern(uint8_t *frame, uint32_t width, uint32_t height, uint32_t stride)
{
    uint32_t color = 0;
    uint32_t start = 0;

    for (uint32_t ycoi = 0; ycoi < height; ycoi++) {
        for (uint32_t xcoi = 0; xcoi < (width * stride); xcoi += stride) {
            if (width * stride / 8 * 1 > xcoi) {
                color = COLOR_WHITE;
            } else if (width * stride / 8 * 2 > xcoi) {
                color = COLOR_YELLOW;
            } else if (width * stride / 8 * 3 > xcoi) {
                color = COLOR_CYAN;
            } else if (width * stride / 8 * 4 > xcoi) {
                color = COLOR_GREEN;
            } else if (width * stride / 8 * 5 > xcoi) {
                color = COLOR_MAGENTA;
            } else if (width * stride / 8 * 6 > xcoi) {
                color = COLOR_RED;
            } else if (width * stride / 8 * 7 > xcoi) {
                color = COLOR_BLUE;
            } else {
                color = COLOR_BLACK;
            }

            frame[start + xcoi + 0] = 0xff & (color >> 16); // B
            frame[start + xcoi + 1] = 0xff & (color >> 8);  // G
            frame[start + xcoi + 2] = 0xff & (color);       // R
        }

        start += width * stride;
    }
}

void t1_genframe(void)
{
    cv::Mat frame_buff(cv::Size(FRAME_WIDTH, FRAME_HEIGHT), CV_8UC3, cv::Scalar(0, 0, 0));

    print_test_pattern(frame_buff.data, FRAME_WIDTH, FRAME_HEIGHT, 3);

    std::cout << "T1: 视频生成线程...启动\n";

    while (running) {
        if (frame_queue.empty()) {
            frame_mutex.lock();
            frame_queue.push(frame_buff.clone());
            frame_mutex.unlock();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(FRAME_DLY));
    }

    std::cout << "T1: 视频生成线程...关闭\n";
}

void t2_sendframe(void)
{
    int ret = 0;
    int sock_fd = 0;
    struct sockaddr_in sock_addr = { 0 };

    uint16_t pkt_idx = 0;
    uint8_t  pkt_buff[FRAME_PKT] = {0};

    cv::Mat frame_buff(cv::Size(FRAME_WIDTH, FRAME_HEIGHT), CV_8UC3, cv::Scalar(0, 0, 0));

    std::cout << "T2: 视频发送线程...启动\n";

    sock_addr.sin_family = AF_INET;
    sock_addr.sin_port = htons(TARGET_PORT);

    inet_pton(sock_addr.sin_family, TARGET_ADDR, &sock_addr.sin_addr);

#ifdef _WIN32
    WSADATA wsa_data = { 0 };
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cout << "T2: 故障！套接字未初始化\n";
        goto err_t2;
    }
#endif

    if ((sock_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        std::cout << "T2: 故障！无法创建套接字\n";
        goto err_t2;
    }

    while (running) {
        if (!frame_queue.empty()) {
            frame_mutex.lock();
            frame_buff = frame_queue.front();
            frame_queue.pop();
            frame_mutex.unlock();

            for (pkt_idx = 0; pkt_idx < FRAME_SEQ; pkt_idx++) {
                pkt_buff[0] = (pkt_idx >> 8) & 0xff;
                pkt_buff[1] = (pkt_idx >> 0) & 0xff;

                bgr888_rgb565(pkt_buff + 2, frame_buff.data + pkt_idx * (FRAME_PKT - 2) * 3 / 2, (FRAME_PKT - 2) / 2);

                if ((ret = sendto(sock_fd, (const char *)pkt_buff, FRAME_PKT, 0, (const sockaddr *)&sock_addr, sizeof(sock_addr))) < 0) {
                    std::cout << "T2: 注意！无法写入套接字\n";
                    goto err_t2s;
                }
            }
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

err_t2s:
#ifdef _WIN32
    closesocket(sock_fd);

    WSACleanup();
#else
    close(sock_fd);
#endif

err_t2:
    running = false;

    std::cout << "T2: 视频发送线程...关闭\n";
}

int main(void)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#else
    signal(SIGINT, signal_handle);
    signal(SIGTERM, signal_handle);
#endif

    std::cout << "M : 视频生成进程...启动\n";

    running = true;

    thread t1(t1_genframe);
    t1.detach();
    thread t2(t2_sendframe);
    t2.join();

    if (t1.joinable()) {
        t1.join();
    }

    std::cout << "M : 视频生成进程...关闭\n";

    return 0;
}
