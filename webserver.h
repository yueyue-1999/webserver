#ifndef WEBSERVER_H
#define WEBSERVER_H

#include "http_conn.h"
#include "threadpool.h"
#include "lst_timer.h"

#define MAX_FD 65536 //最多可以链接进来的客户端数
#define MAX_EVENT_NUMBER 10000 //最大的监听事件数
#define TIMESLOT 5

class Webserver{
private:
    int m_port;

    // 线程池
    threadpool<http_conn> *m_pool;
    // int m_threadnum;

    // 客户端数组
    http_conn* m_users;

    // 定时器相关
    sort_timer_lst m_timer_lst;

    // epoll相关
    int m_listenfd;
    epoll_event m_events[ MAX_EVENT_NUMBER ];
    int m_epollfd;

    int m_TrigMode;
    int m_ListenTrigMode;
    int m_ConnTrigMode;
    int m_ActorMode;

public:
    Webserver();
    ~Webserver();

    void init(int port, int ActorMode, int TrigMode);
    void initTrigMode();

    void thread_pool();

    void eventlisten();

    void init_timer(int connfd, const sockaddr_in& saddr);
    void adjust_timer(util_timer* timer);
    void del_timer(util_timer* timer, int sockfd);

    void dealwithread(int sockfd);
    void dealwithwrite(int sockfd);
    void dealwithclient();
    void dealwithsignal(bool& timeout, bool& stopserver);
    void eventloop();

};

#endif