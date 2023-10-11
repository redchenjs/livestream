#include <time.h>
#include <thread>
#include <csignal>
#include <netdb.h>
#include <unistd.h>
#include <iostream>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <opencv2/opencv.hpp>

using namespace std;

#define FRAME_CNT    (3)
#define FRAME_FPS    (20)
#define FRAME_PKT    (1442)
#define FRAME_SEQ    (2880)

#define FRAME_WIDTH  (1920)
#define FRAME_HEIGHT (1080)

bool running = false;

mutex           frame_mutex;
queue<cv::Mat>  frame_queue;
queue<uint16_t> count_queue;

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

void rgb565_bgr888(uint8_t *dst, uint8_t *src, int size)
{
    for (int i = 0; i < size; i++) {
        uint16_t rgb565 = *src++ | (*src++ << 8);

        dst[i * 3 + 0] = (rgb565 & 0x001f) << 3; // B
        dst[i * 3 + 1] = (rgb565 & 0x07e0) >> 3; // G
        dst[i * 3 + 2] = (rgb565 & 0xf800) >> 8; // R
    }
}

void t1_recvframe(void)
{
    int ret = 0;
    int sock_fd = 0;
    socklen_t src_len = 0;
    socklen_t rcv_opt = 16 * 1024 * 1024;
    struct sockaddr_in src_addr = { 0 };

    uint16_t pkt_idx = 0;
    uint8_t  pkt_buff[FRAME_PKT] = {0};

    uint16_t count_curr = 0;
    uint8_t  frame_curr = 0;
    bool     frame_sync = false;
    cv::Mat  frame_buff[FRAME_CNT];

    src_addr.sin_family = AF_INET;
    src_addr.sin_addr.s_addr = inet_addr("192.168.1.102");
    src_addr.sin_port = htons(8001);

    cout << "T1: 视频接收线程...启动" << endl;

    if ((sock_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        printf("T1: 故障！无法创建套接字\n");
        goto err_t1;
    }

    if ((ret = setsockopt(sock_fd, SOL_SOCKET, SO_RCVBUF, (const char *)&rcv_opt, sizeof(rcv_opt))) < 0) {
        printf("T1: 故障！无法设定缓冲区大小\n");
        goto err_t1s;
    };

    if ((ret = bind(sock_fd, (sockaddr *)&src_addr, sizeof(src_addr))) < 0) {
        printf("T1: 故障！无法绑定到指定端口\n");
        goto err_t1s;
    };

    for (int i = 0; i < FRAME_CNT; i++) {
        frame_buff[i] = cv::Mat(cv::Size(FRAME_WIDTH, FRAME_HEIGHT), CV_8UC3, cv::Scalar(0, 0, 0));
    }

    while (running) {
        static uint16_t pkt_idx_prev = 0;

        frame_sync = false;
        count_curr = 0;

        while (true) {
            if ((ret = recvfrom(sock_fd, pkt_buff, FRAME_PKT, 0, (sockaddr *)&src_addr, &src_len)) < 0) {
                printf("T1: 故障！%s\n", strerror(errno));
            }

            pkt_idx_prev = pkt_idx;
            pkt_idx = pkt_buff[0] << 8 | pkt_buff[1];

            if ((pkt_idx == 0 && pkt_idx_prev != FRAME_SEQ - 1) ||
                (pkt_idx != 0 && pkt_idx_prev != pkt_idx - 1)) {
                printf("T1: 注意！数据包丢失：%d => %d\n", pkt_idx_prev, pkt_idx);

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
                rgb565_bgr888(frame_buff[frame_curr].data + pkt_idx * (FRAME_PKT - 2) * 3 / 2, pkt_buff + 2, (FRAME_PKT - 2) / 2);
            } else {
                printf("T1: 错误！数据包无效：%d\n", pkt_idx);
            }

            if (frame_sync && (pkt_idx == FRAME_SEQ - 1)) {
                break;
            }
        }

        frame_mutex.lock();
        count_queue.push(count_curr);
        frame_queue.push(frame_buff[frame_curr]);
        frame_mutex.unlock();

        frame_curr = (++frame_curr) % FRAME_CNT;
        frame_buff[frame_curr].setTo(cv::Scalar(0, 0, 0));
    }

err_t1s:
    close(sock_fd);

err_t1:
    running = false;

    cout << "T1: 视频接收线程...关闭" << endl;
}

void t2_showframe(void)
{
    uint16_t update = 0;
    uint16_t count_curr = 0;
    uint32_t fps = 0, fps_sum = 0;
    uint32_t err = 0, err_sum = 0;
    struct timespec start  = {0, 0};
    struct timespec finish = {0, 0};
    cv::Mat frame_buff(cv::Size(FRAME_WIDTH, FRAME_HEIGHT), CV_8UC3, cv::Scalar(0, 0, 0));

    cout << "T2: 视频显示线程...启动" << endl;

    cv::namedWindow("Frame", cv::WINDOW_NORMAL);
    cv::setWindowProperty("Frame", cv::WND_PROP_FULLSCREEN, cv::WINDOW_FULLSCREEN);

    while (running) {
        if (!frame_queue.empty()) {
            clock_gettime(CLOCK_REALTIME, &finish);

            frame_mutex.lock();
            count_curr = count_queue.front();
            frame_buff = frame_queue.front();
            count_queue.pop();
            frame_queue.pop();
            frame_mutex.unlock();

            fps_sum += (finish.tv_sec - start.tv_sec) * 1000 + (finish.tv_nsec - start.tv_nsec) / 1000000;
            err_sum += FRAME_SEQ - count_curr;

            if (update++ == FRAME_FPS + 2) {
                update = 0;

                fps = 10000.0 * FRAME_FPS / fps_sum;
                err = err_sum * 10000 / FRAME_FPS / FRAME_SEQ;

                fps_sum = 0;
                err_sum = 0;
            }

            string s = format("FPS:{:.1f}", fps / 10.0);
            cv::putText(frame_buff, s.c_str(), cv::Point(10, 30), cv::FONT_HERSHEY_COMPLEX, 1.0, cv::Scalar(0, 255, 0), 2, 8, 0);
            string e = format("ERR:{:.2f}%", err / 100.0);
            cv::putText(frame_buff, e.c_str(), cv::Point(10, 60), cv::FONT_HERSHEY_COMPLEX, 1.0, cv::Scalar(0, 0, 255), 2, 8, 0);

            cv::waitKey(1);
            cv::imshow("Frame", frame_buff);
        } else {
            usleep(1000);
            continue;
        }

        clock_gettime(CLOCK_REALTIME, &start);
    }

    cv::destroyWindow("Frame");

    cout << "T2: 视频显示线程...关闭" << endl;
}

int main()
{
    signal(SIGINT, signal_handle);
    signal(SIGTERM, signal_handle);

    cout << "M : 视频处理进程...启动" << endl;

    running = true;

    thread t1(t1_recvframe);
    t1.detach();
    thread t2(t2_showframe);
    t2.join();
    t1.join();

    cout << "M : 视频处理进程...关闭" << endl;

    return 0;
}
