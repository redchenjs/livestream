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

#define FPS_SET      (20)

#define FRAME_CNT    (3)
#define FRAME_FPS    (20)
#define FRAME_PKT    (1442)
#define FRAME_SEQ    (2880)

#define FRAME_WIDTH  (1920)
#define FRAME_HEIGHT (1080)

bool running = false;

mutex          frame_mutex;
queue<cv::Mat> frame_queue;

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

void bgr888_rgb565(uint8_t *dst, uint8_t *src, int size)
{
    for (int i = 0; i < size; i++) {
        uint8_t b = *src++;
        uint8_t g = *src++;
        uint8_t r = *src++;

        dst[i * 2 + 0] = (r << 3) | (g >> 5);
        dst[i * 2 + 1] = (g << 5) | (b >> 3);
    }
}

void t1_genframe(void)
{
    cv::Mat frame_buff(cv::Size(FRAME_WIDTH, FRAME_HEIGHT), CV_8UC3, cv::Scalar(0, 0, 0));

    cout << "T1: 视频生成线程...启动" << endl;

    while (running) {
        vector<cv::Point> contour;
        cv::Rect rec = cv::Rect(cv::Point(500,500), cv::Point(1000,1000));

        contour.push_back(rec.tl());
        contour.push_back(cv::Point(rec.tl().x + rec.width , rec.tl().y ) );
        contour.push_back(cv::Point(rec.tl().x + rec.width , rec.tl().y + rec.height));
        contour.push_back(cv::Point(rec.tl().x , rec.tl().y + rec.height ));

        cv::fillConvexPoly(frame_buff, contour, cv::Scalar(255, 255, 255));

        frame_mutex.lock();
        if (frame_queue.empty()) {
            frame_queue.push(frame_buff);
        }
        frame_mutex.unlock();

        usleep(1000 * 50);
    }

    cout << "T1: 视频生成线程...关闭" << endl;
}

void t2_sendframe(void)
{
    int ret = 0;
    int sock_fd = 0;
    struct sockaddr_in dst_addr = { 0 };

    uint16_t pkt_idx = 0;
    uint8_t  pkt_buff[FRAME_PKT] = {0};

    cv::Mat frame_buff(cv::Size(FRAME_WIDTH, FRAME_HEIGHT), CV_8UC3, cv::Scalar(0, 0, 0));

    dst_addr.sin_family = AF_INET;
    dst_addr.sin_addr.s_addr = inet_addr("192.168.1.102");
    dst_addr.sin_port = htons(8001);

    cout << "T2: 视频发送线程...启动" << endl;

    if ((sock_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        printf("T2: 故障！无法创建套接字\n");
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

                if ((ret = sendto(sock_fd, pkt_buff, FRAME_PKT, 0, (const sockaddr *)&dst_addr, sizeof(dst_addr))) < 0) {
                    printf("T2: 故障: %s\n", strerror(errno));
                }
            }
        }

        usleep(1000);
    }

    close(sock_fd);

err_t2:
    running = false;

    cout << "T2: 视频发送线程...关闭" << endl;
}

int main()
{
    signal(SIGINT, signal_handle);
    signal(SIGTERM, signal_handle);

    cout << "M : 视频生成进程...启动" << endl;

    running = true;

    thread t1(t1_genframe);
    t1.detach();
    thread t2(t2_sendframe);
    t2.join();
    t1.join();

    cout << "M : 视频生成进程...关闭" << endl;

    return 0;
}
