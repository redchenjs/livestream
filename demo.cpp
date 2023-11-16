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
#define CVUI_IMPLEMENTATION
#include "cvui.h"

#define FRAME_PKT    (1442)
#define FRAME_SEQ    (1280)
#define FRAME_CNT    (1000000)

#define FRAME_WIDTH  (1280)
#define FRAME_HEIGHT ( 720)
#define FRAME_DISP   (1440)
#define FRAME_RATE   (  60)

#define LISTEN_ADDR  "192.168.2.102"
#define LISTEN_PORT  (8001)

#define IMAGE_PATH  "Images/"
#define VIDEO_PATH  "Videos/"

bool running = false;
bool fullscreen = false;

std::mutex           frame_mutex;
std::queue<cv::Mat>  frame_queue;
std::queue<uint16_t> count_queue;

cv::VideoWriter video_writer;

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

        dst[i * 3 + 0] = (((rgb565 & 0x001f) << 3) * 527 + 23) >> 6; // B
        dst[i * 3 + 1] = (((rgb565 & 0x07e0) >> 3) * 259 + 33) >> 6; // G
        dst[i * 3 + 2] = (((rgb565 & 0xf800) >> 8) * 527 + 23) >> 6; // R
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

    std::cout << "T1: 视频接收线程...启动\n";

    sock_addr.sin_family = AF_INET;
    sock_addr.sin_port = htons(LISTEN_PORT);

    inet_pton(sock_addr.sin_family, LISTEN_ADDR, &sock_addr.sin_addr);

#ifdef _WIN32
    WSADATA wsa_data = { 0 };
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        std::cout << "T1: 故障！套接字未初始化\n";
        goto err_t1;
    }
#endif

    if ((sock_fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) < 0) {
        std::cout << "T1: 故障！无法创建套接字\n";
        goto err_t1;
    }

    if ((ret = ::setsockopt(sock_fd, SOL_SOCKET, SO_RCVBUF, (const char *)&sock_opt, sizeof(sock_opt))) < 0) {
        std::cout << "T1: 故障！无法设定缓冲区大小\n";
        goto err_t1s;
    };

    if ((ret = ::bind(sock_fd, (const sockaddr *)&sock_addr, sizeof(sock_addr))) < 0) {
        std::cout << "T1: 故障！无法绑定到指定端口\n";
        goto err_t1s;
    };

    while (running) {
        static uint16_t pkt_idx_prev = 0;

        frame_sync = false;
        count_curr = 0;

        while (true) {
            if ((ret = ::recvfrom(sock_fd, (char *)pkt_buff, FRAME_PKT, 0, (sockaddr *)&sock_addr, &sock_len)) < 0) {
                std::cout << "T1: 注意！无法读取套接字\n";
                goto err_t1s;
            }

            pkt_idx_prev = pkt_idx;
            pkt_idx = pkt_buff[0] << 8 | pkt_buff[1];

            if ((pkt_idx == 0 && pkt_idx_prev != FRAME_SEQ - 1) ||
                (pkt_idx != 0 && pkt_idx_prev != pkt_idx - 1)) {
                std::cout << std::format("\033[33mT1: 注意！跳包：{:04d} => {:04d}\033[0m\n", pkt_idx_prev, pkt_idx);

                if (pkt_idx_prev >= pkt_idx) {
                    break;
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

    std::cout << "T1: 视频接收线程...关闭\n";
}

void t2_showframe(void)
{
    bool ret = false;
    bool video_cap = false;
    uint16_t frame_curr = 0;
    uint16_t count_curr = 0;
    char time_str[64] = { 0 };
    double err = 0.0, fps = 0.0;
    uint64_t err_cnt = 0, fps_cnt = 0, fps_rel = 0;
    std::string image_time = "", video_time = "";
    std::chrono::microseconds duration(0);
    std::chrono::high_resolution_clock::time_point start, finish;
    cv::Mat frame_buff(cv::Size(FRAME_WIDTH, FRAME_HEIGHT), CV_8UC3, cv::Scalar(0, 0, 0));
    cv::Mat frame_disp(cv::Size(FRAME_DISP,  FRAME_HEIGHT), CV_8UC3, cv::Scalar(0, 0, 0));

    std::cout << "T2: 视频显示线程...启动\n";

    cv::namedWindow("Frame", cv::WINDOW_NORMAL);
    cv::resizeWindow("Frame", FRAME_DISP, FRAME_HEIGHT);

    cvui::init("Frame");

    start = std::chrono::high_resolution_clock::now();

    while (running) {
        if (!frame_queue.empty()) {
            frame_mutex.lock();
            count_curr = count_queue.front();
            frame_buff = frame_queue.front();
            count_queue.pop();
            frame_queue.pop();
            frame_mutex.unlock();

            err_cnt += FRAME_SEQ - count_curr;
            fps_cnt += 1;

            if (++frame_curr > 9999) {
                frame_curr = 1;
            }

            if (count_curr != FRAME_SEQ) {
                std::cout << std::format("\033[31mT2: 注意！丢帧：{:04d} == {:04d}\033[0m\n", frame_curr, count_curr);
                continue;
            }

            fps_rel += 1;

            finish    = std::chrono::high_resolution_clock::now();
            duration += std::chrono::duration_cast<std::chrono::microseconds>(finish - start);

            if (duration.count() >= FRAME_CNT) {
                err = 100.0 * err_cnt / (FRAME_SEQ * fps_cnt);
                fps = 1.0 * FRAME_CNT * fps_rel / duration.count();

                if (err > 100.0) {
                    err = 100.0;
                }

                duration = duration.zero();

                err_cnt = 0;
                fps_cnt = 0;
                fps_rel = 0;
            }

            for (int i = 0; i < FRAME_HEIGHT; i++) {
                memcpy(frame_disp.data + i * FRAME_DISP * 3, frame_buff.data + i* FRAME_WIDTH * 3, FRAME_WIDTH * 3);
            }

            cv::rectangle(frame_disp, cv::Point(0, 0), cv::Point(FRAME_WIDTH - 1, FRAME_HEIGHT - 1), cv::Scalar(255, 255, 255), 1);
            cv::rectangle(frame_disp, cv::Point(FRAME_WIDTH, 0), cv::Point(FRAME_DISP - 1, FRAME_HEIGHT - 1), cv::Scalar(0, 0, 0), cv::FILLED);
            cv::rectangle(frame_disp, cv::Point(FRAME_WIDTH - 1, 0), cv::Point(FRAME_DISP - 1, FRAME_HEIGHT / 2 - 1), cv::Scalar(255, 255, 255), 1);
            cv::rectangle(frame_disp, cv::Point(FRAME_WIDTH - 1, FRAME_HEIGHT / 2 - 1), cv::Point(FRAME_DISP - 1, FRAME_HEIGHT - 1), cv::Scalar(255, 255, 255), 1);
            cvui::printf(frame_disp, 1292, 20, 0.5, 0xffffff, "Video Screenshot");
            cvui::printf(frame_disp, 1300, 380, 0.5, 0xffffff, "Video Recorder");

            if (image_time != "") {
                cvui::printf(frame_disp, 1337, 210, 0.5, 0x00ff00, "Saved");
                cvui::printf(frame_disp, 1350, 284, 0.4, 0xffffff, "File:");
                cvui::printf(frame_disp, 1287, 304, 0.4, 0xffffff, "%s", image_time.c_str());
                cvui::printf(frame_disp, 1348, 324, 0.4, 0xffffff, ".jpg");
            }

            if (video_time != "") {
                if (video_cap) {
                    // Top-Left
                    cv::line(frame_disp, cv::Point(2, 2), cv::Point(2, 100), cv::Scalar(0, 255, 0), 2);
                    cv::line(frame_disp, cv::Point(2, 2), cv::Point(100, 2), cv::Scalar(0, 255, 0), 2);
                    // Top-Right
                    cv::line(frame_disp, cv::Point(FRAME_WIDTH - 3, 2), cv::Point(FRAME_WIDTH - 3, 100), cv::Scalar(0, 255, 0), 2);
                    cv::line(frame_disp, cv::Point(FRAME_WIDTH - 3, 2), cv::Point(FRAME_WIDTH - 3 - 100, 2), cv::Scalar(0, 255, 0), 2);
                    // Bottom-Left
                    cv::line(frame_disp, cv::Point(2, FRAME_HEIGHT - 3), cv::Point(2, FRAME_HEIGHT - 1 - 100), cv::Scalar(0, 255, 0), 2);
                    cv::line(frame_disp, cv::Point(2, FRAME_HEIGHT - 3), cv::Point(100, FRAME_HEIGHT - 3), cv::Scalar(0, 255, 0), 2);
                    // Bottom-Right
                    cv::line(frame_disp, cv::Point(FRAME_WIDTH - 3, FRAME_HEIGHT - 3), cv::Point(FRAME_WIDTH - 3, FRAME_HEIGHT - 3 - 100), cv::Scalar(0, 255, 0), 2);
                    cv::line(frame_disp, cv::Point(FRAME_WIDTH - 3, FRAME_HEIGHT - 3), cv::Point(FRAME_WIDTH - 3 - 100, FRAME_HEIGHT - 3), cv::Scalar(0, 255, 0), 2);

                    cvui::printf(frame_disp, 1321, 570, 0.5, 0xffff00, "Recording");
                } else {
                    cvui::printf(frame_disp, 1337, 570, 0.5, 0x00ff00, "Saved");
                }
                cvui::printf(frame_disp, 1350, 644, 0.4, 0xffffff, "File:");
                cvui::printf(frame_disp, 1287, 664, 0.4, 0xffffff, "%s", video_time.c_str());
                cvui::printf(frame_disp, 1345, 684, 0.4, 0xffffff, ".mp4");
            }

            std::string s = std::format("FPS:{:.1f}", fps);
            cv::putText(frame_disp, s.c_str(), cv::Point(10, 35), cv::FONT_HERSHEY_COMPLEX, 1.0, cv::Scalar(0, 255, 0), 2, 8, 0);
            std::string e = std::format("ERR:{:.2f}%", err);
            cv::putText(frame_disp, e.c_str(), cv::Point(10, 70), cv::FONT_HERSHEY_COMPLEX, 1.0, cv::Scalar(0, 0, 255), 2, 8, 0);

        #ifdef _WIN32
            SYSTEMTIME sys;
            GetLocalTime(&sys);
            snprintf(time_str, sizeof(time_str), "%4d%02d%02d.%02d%02d%02d.%03d", sys.wYear, sys.wMonth, sys.wDay, sys.wHour, sys.wMinute, sys.wSecond, sys.wMilliseconds);
        #else
            char time_tmp[64] = {0};
            struct timeval cur_time;
            gettimeofday(&cur_time, NULL);
            int ms = cur_time.tv_usec / 1000;

            struct tm local_time;
            localtime_r(&cur_time.tv_sec, &local_time);
            strftime(time_tmp, sizeof(time_tmp), "%Y%m%d.%H%M%S", &local_time);
            snprintf(time_str, sizeof(time_str), "%s.%03d", time_tmp, ms);
        #endif

            if (cvui::button(frame_disp, 1323, 148, "Save", 0.5)) {
                image_time = std::string(time_str);
                std::string image_path = IMAGE_PATH + image_time + ".jpg";
            #ifdef _WIN32
                ret = _mkdir(IMAGE_PATH);
            #else
                mkdir(IMAGE_PATH, 0777);
            #endif
                cv::imwrite(image_path, frame_buff);
                std::cout << std::format("\033[32mT2: 截屏: {:s}\033[0m\n", image_path);
            }

            if (video_cap) {
                ret = cvui::button(frame_disp, 1323, 503, "Stop", 0.5);
            } else {
                ret = cvui::button(frame_disp, 1323, 503, "Save", 0.5);
            }

            if (ret) {
                static std::string video_path = "";

                if (video_cap) {
                    video_cap = false;

                    video_writer.release();
                    std::cout << std::format("\033[32mT2: 停止录屏: {:s}\033[0m\n", video_path);
                } else {
                    video_cap = true;

                    video_time = std::string(time_str);
                    video_path = VIDEO_PATH + video_time + ".mp4";
                #ifdef _WIN32
                    ret = _mkdir(VIDEO_PATH);
                #else
                    mkdir(VIDEO_PATH, 0777);
                #endif

                    std::cout << std::format("\033[32mT2: 启动录屏: {:s}\033[0m\n", video_path);
                    video_writer.open(video_path, video_writer.fourcc('m', 'p', '4', 'v'), 60.0, cv::Size(FRAME_WIDTH, FRAME_HEIGHT), true);
                }
            }

            cvui::update();
            cvui::imshow("Frame", frame_disp);

            start = std::chrono::high_resolution_clock::now();

            switch (cv::pollKey() & 0xff) {
                case 'q':
                case 'Q':
                case 0x1b:  // ESC
                    running = false;
                    break;
            }

            if (video_cap) {
                video_writer << frame_buff;
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
    }

    cv::destroyWindow("Frame");

    std::cout << "T2: 视频显示线程...关闭\n";
}

int main(void)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#else
    signal(SIGINT, signal_handle);
    signal(SIGTERM, signal_handle);
#endif

    std::cout << "M : 视频处理进程...启动\n";

    running = true;

    std::thread t1(t1_recvframe);
    t1.detach();
    std::thread t2(t2_showframe);
    t2.join();

    if (t1.joinable()) {
        t1.join();
    }

    std::cout << "M : 视频处理进程...关闭\n";

    return 0;
}
