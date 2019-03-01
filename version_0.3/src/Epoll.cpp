//
// Created by marvinle on 2019/2/26 8:08 PM.
//

#include "../include/Epoll.h"
#include "../include/Util.h"
#include <iostream>
#include <vector>
#include <sys/epoll.h>
#include <cstdio>



std::unordered_map<int, std::shared_ptr<HttpData>> Epoll::httpDataMap;
const int Epoll::MAX_EVENTS = 10000;
epoll_event *Epoll::events;

// 可读 | ET模 | 保证一个socket连接在任一时刻只被一个线程处理
const __uint32_t Epoll::DEFAULT_EVENTS =  EPOLLIN | EPOLLET | EPOLLONESHOT;

TimerManager Epoll::timerManager;




int Epoll::init(int max_events) {
    int epoll_fd = ::epoll_create(max_events);
    if (epoll_fd == -1) {
        std::cout << "epoll create error" << std::endl;
        exit(-1);
    }
    events = new epoll_event[max_events];
    return epoll_fd;
}


int Epoll::addfd(int epoll_fd, int fd, __uint32_t events, std::shared_ptr<HttpData> httpData) {
    epoll_event event;
    event.events = (EPOLLIN|EPOLLET);
    event.data.fd = fd;
    httpDataMap[fd] = httpData;
    int ret = ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event);
    if (ret < 0) {
        std::cout << "epoll add error" << endl;
        // 释放httpData
        httpDataMap[fd].reset();
        return -1;
    }
    return 0;
}

int Epoll::modfd(int epoll_fd, int fd, __uint32_t events, std::shared_ptr<HttpData> httpData) {
    epoll_event event;
    event.events = events;
    event.data.fd = fd;
    httpDataMap[fd] = httpData;
    int ret = ::epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event);
    if (ret < 0) {
        std::cout << "epoll mod error" << endl;
        // 释放httpData
        httpDataMap[fd].reset();
        return -1;
    }
    return 0;
}

int Epoll::delfd(int epoll_fd, int fd, __uint32_t events) {
    epoll_event event;
    event.events = events;
    event.data.fd = fd;
    int ret = epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &event);
    if (ret < 0) {
        std::cout << "epoll del error" << endl;
        return -1;
    }
    auto it = httpDataMap.find(fd);
    if (it != httpDataMap.end()) {
        httpDataMap.erase(it);
    }
    return 0;
}

void Epoll::handleConnection(const ServerSocket &serverSocket) {

    std::shared_ptr<ClientSocket> tempClient(new ClientSocket);
    // epoll 是ET模式，循环接收连接
    while(serverSocket.accept(*tempClient) > 0) {
        int ret = setnonblocking(tempClient->fd);
        if (ret < 0) {
            std::cout << "setnonblocking error" << std::endl;
            tempClient->close();
            continue;
        }

        // 在这里做限制并发, 暂时未完成

        std::shared_ptr<HttpData> sharedHttpData(new HttpData);
        sharedHttpData->request_ = std::shared_ptr<HttpRequest>(new HttpRequest());
        sharedHttpData->response_ = std::shared_ptr<HttpResponse>(new HttpResponse());

        std::shared_ptr<ClientSocket> sharedClientSocket(new ClientSocket());
        sharedClientSocket.swap(tempClient);
        sharedHttpData->clientSocket_ =  sharedClientSocket;
        sharedHttpData->epoll_fd = serverSocket.epoll_fd;


        addfd(serverSocket.epoll_fd, sharedClientSocket->fd, DEFAULT_EVENTS, sharedHttpData);
        // 添加定时器
        timerManager.addTimer(sharedHttpData, TimerManager::DEFAULT_TIME_OUT);
    }

}

std::vector<std::shared_ptr<HttpData>> Epoll::poll(const ServerSocket &serverSocket, int max_event, int timeout) {
    int event_num = epoll_wait(serverSocket.epoll_fd, events, max_event, timeout);
    if (event_num < 0) {
        std::cout << "epoll_num=" << event_num << std::endl;
        std::cout << "epoll_wait error" << std::endl;
        exit(-1);
    }

    std::vector<std::shared_ptr<HttpData>> httpDatas;
    // 遍历events集合
    for(int i = 0; i < event_num; i++) {
        int fd = events[i].data.fd;

        // 监听描述符
        if (fd == serverSocket.listen_fd) {
            handleConnection(serverSocket);
        } else {
             // 出错的描述符，移除定时器， 关闭文件描述符
            if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLRDHUP) || (events[i].events & EPOLLHUP)) {
                auto it = httpDataMap.find(fd);
                if (it != httpDataMap.end()) {
                    // 将HttpData节点和TimerNode的关联分开，这样HttpData会立即析构，在析构函数内关闭文件描述符等资源
                    it->second->closeTimer();
                    httpDataMap.erase(it);
                }
                continue;
            }

            auto it = httpDataMap.find(fd);
            if (it != httpDataMap.end()) {
                if ((events[i].events & EPOLLIN) || (events[i].events & EPOLLPRI)) {
                    httpDatas.push_back(it->second);
                    // 清除定时器
                    it->second->closeTimer();
                    // 删除掉当前unorder_map持有的HttpData
                    httpDataMap.erase(it);
                }
            }
            // 这里有个问题是 TimerNode正常超时释放时

        }
    }
    return httpDatas;
}