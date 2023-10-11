#include <iostream>
#include <opencv2/opencv.hpp>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <csignal>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>

using namespace std;

#define FPS_SET (20)
#define PKT_SEG (1442)

#define FRAME_WIDTH  (1920)
#define FRAME_HEIGHT (1080)

bool running = false;
bool t1_running = false;
bool t2_running = false;

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

void bgr888_rgb565(int *src, char *dest, int size)
{
    for (int i = 0; i < size; i++) {
        unsigned int RGB24 = *src++;

        dest[i*3+2] = (RGB24 & 0xf00) >> 16;
        dest[i*3+1] = (RGB24 & 0x0f0) >> 8;
        dest[i*3+0] = (RGB24 & 0x00f);
    }
}

void t1_getframe(void)
{
    cv::Mat frame;

    cout << "T1: 视频生成线程...启动" << endl;

    while (running) {
        while (frame_queue.size() > 3) {
            usleep(1000 * 1000 / FPS_SET);
        }

        frame = cv::Mat(cv::Size(FRAME_WIDTH, FRAME_HEIGHT), CV_8UC3, cv::Scalar(0xaa, 0xbb, 0xcc));

        printf("%02x%02x%02x%02x\n", frame.data[0], frame.data[1], frame.data[2], frame.data[3]);

        frame_mutex.lock();
        frame_queue.push(frame);
        frame_mutex.unlock();

        usleep(1000 * 1000 / FPS_SET);
    }

    cout << "T1: 视频生成线程...关闭" << endl;
}

void t2_sendframe(void)
{
    int size = 0;
    int remain = 0;
    uint8_t *data = NULL;
    cv::Mat frame;
    cv::Mat convert;
    int sock_fd;
    struct sockaddr_in dest_addr;

    cout << "T2: 数据发送线程...启动" << endl;

     if ((sock_fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
         printf("create socket fail!\n");
         return;
     }

    dest_addr.sin_family = AF_INET;
    dest_addr.sin_addr.s_addr = inet_addr("192.168.1.102");
    dest_addr.sin_port = htons(8001);

    while (running) {
        if (!frame_queue.empty()) {
            frame_mutex.lock();
            frame = frame_queue.front();
            frame_queue.pop();
            frame_mutex.unlock();

            cv::cvtColor(frame, convert, cv::COLOR_BGR2BGR565);

            data   = convert.data;
            remain = convert.total() * convert.elemSize();

            while (remain) {
                size = remain > PKT_SEG ? PKT_SEG : remain;

                int ret = sendto(sock_fd, data, size, 0, (const sockaddr *)&dest_addr, sizeof(dest_addr));
                if (ret < 0) {
                    printf("T2: 故障: %s\n", strerror(errno));
                }

                data   += size;
                remain -= size;
            }
        }

        usleep(1000 * 1000 / FPS_SET);
    }

    close(sock_fd);

    cout << "T2: 数据发送线程...关闭" << endl;
}

int main()
{
    signal(SIGINT, signal_handle);
    signal(SIGTERM, signal_handle);

    cout << "M : 视频发送进程...启动" << endl;

    running = true;

    thread t1(t1_getframe);
    t1.detach();
    thread t2(t2_sendframe);
    t2.join();
    t1.join();

    cout << "M : 视频发送进程...关闭" << endl;

    return 0;
}
