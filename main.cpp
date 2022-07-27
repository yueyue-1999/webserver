#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <assert.h>
#include <signal.h>
#include "webserver.h"
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include "lst_timer.h"
#include "config.h"

int main(int argc, char* argv[])
{
    //检验命令行，获取端口号
    Config config;
    config.parse_arg(argc, argv);

    Webserver webserver;
    webserver.init(config.PORT, config.ActorMode, config.TrigMode);

    webserver.thread_pool();

    webserver.eventlisten();

    webserver.eventloop();

    return 0;
}