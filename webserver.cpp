#include"webserver.h"


extern void addfd( int epollfd, int fd, bool one_shot, int TRIGMODE );
extern void removefd( int epollfd, int fd );
extern void setnonblocking( int fd );

static int pipefd[2];

//把信号发送到了管道中
void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

void addsig(int signum, void (handler)(int))
{
    //对signum信号执行handler操作
    struct sigaction sig_act;
    sig_act.sa_handler = handler;
    sig_act.sa_flags = 0;
    sigemptyset(&sig_act.sa_mask);
    assert( sigaction( signum, &sig_act, NULL ) != -1 );
}

void cb_func( http_conn* user_data ){
    user_data -> close_conn();
    printf("close connection for timeout\n");
}

Webserver::Webserver(){
    m_users = new http_conn[ MAX_FD ];
}

Webserver::~Webserver(){
    close(pipefd[0]);
    close(pipefd[1]);
    close(m_epollfd);
    close(m_listenfd);
    delete[] m_users;
    delete[] m_pool;
}

void Webserver::initTrigMode(){
    //LT + LT
    if (m_TrigMode == 0)
    {
        m_ListenTrigMode = 0;
        m_ConnTrigMode = 0;
    }
    //LT + ET
    else if (m_TrigMode == 1)
    {
        m_ListenTrigMode = 0;
        m_ConnTrigMode = 1;
    }
    //ET + LT
    else if (m_TrigMode == 2)
    {
        m_ListenTrigMode = 1;
        m_ConnTrigMode = 0;
    }
    //ET + ET
    else if (m_TrigMode == 3)
    {
        m_ListenTrigMode = 1;
        m_ConnTrigMode = 1;
    }
}

void Webserver::init(int port, int ActorMode, int TrigMode){
    m_ActorMode = ActorMode;
    m_TrigMode = TrigMode;
    m_port = port;
    initTrigMode();
}

void Webserver::thread_pool(){
    m_pool = new threadpool<http_conn>(m_ActorMode);
}

void Webserver::eventlisten(){
    // 监听流程
    m_listenfd = socket(AF_INET, SOCK_STREAM, 0);
    assert(m_listenfd >= 0);

    struct sockaddr_in addr;
    int ret = 0;
    bzero(&addr, sizeof(addr)); //将m_addr清0
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY); //绑定本机的所有IP地址

    int opt = 1;
    setsockopt(m_listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    ret = bind(m_listenfd, (struct sockaddr *) &addr, sizeof(struct sockaddr));
    assert(ret >= 0);
    ret = listen(m_listenfd, 5); // 第二个参数为backlog，代表全连接队列最大长度
    assert(ret >= 0);

    // 利用工具包设置epoll
    m_epollfd = epoll_create(5);
    assert(m_epollfd >= 0);
    addfd(m_epollfd, m_listenfd, false, m_ListenTrigMode);
    http_conn::m_epollfd = m_epollfd;

    // 创建管道，pipefd[0]是读，pipefd[1]是写
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert( ret != -1 );
    setnonblocking( pipefd[1] );
    addfd( m_epollfd, pipefd[0], false, 0);

    // 设置信号处理函数
    addsig(SIGPIPE, SIG_IGN);
    addsig(SIGALRM, sig_handler);
    addsig(SIGTERM, sig_handler);

    // 发送alarm信号
    alarm(TIMESLOT);
}

void Webserver::init_timer( int connfd, const sockaddr_in& saddr ){
    printf("connecting %d\n", connfd);
    // 初始化客户端，设置定时器放入定时器链表
    m_users[connfd].init( connfd, saddr, m_ConnTrigMode );
    util_timer* timer = new util_timer();
    timer->m_user_data = &m_users[connfd];
    timer->m_cbfunc = cb_func;
    time_t cur = time(NULL);
    timer->m_expire = cur + 3 * TIMESLOT;
    m_users[connfd].m_timer = timer;
    m_timer_lst.push_back( timer );
}

void Webserver::adjust_timer(util_timer* timer){
    if (timer){
        time_t cur = time(NULL);
        timer -> m_expire = cur + 3*TIMESLOT;
        m_timer_lst.adjust_timer(timer);

        printf("adjust timer once\n");
    }
}

void Webserver::del_timer(util_timer* timer, int sockfd){
    timer -> m_cbfunc(&m_users[sockfd]); // 回调函数，关闭客户端连接
    if (timer){
        m_timer_lst.del_timer(timer);
    }
    printf("close fd: %d\n", sockfd);
}

void Webserver::dealwithclient(){
    // 接收新的客户端连接
    struct sockaddr_in saddr;
    socklen_t saddrlen = sizeof(saddr);
    if (m_ListenTrigMode == 0){ // LT
        int connfd = accept( m_listenfd, (sockaddr*)&saddr, &saddrlen );
        if ( connfd == -1 ){
            printf("errno is %d, accept error\n", errno);
            return;
        }
        if ( http_conn::m_user_count >= MAX_FD ){
            const char* message = "Internel server busys";
            send(connfd, message, strlen(message), 0);
            close(connfd);
            return;
        }
        init_timer( connfd, saddr );
    }
    else{ // ET
        while(1){ // 读完返回值为-1, 且errno为EAGIN
            int connfd = accept(m_listenfd, (sockaddr*)&saddr, &saddrlen);
            if (connfd == -1){
                if (errno != EAGAIN || errno != EWOULDBLOCK)
                    printf("errno is %d, accept error\n", errno);
                return;
            }
            if (http_conn::m_user_count >= MAX_FD){
                const char* message = "Internel server busy";
                send(connfd, message, strlen(message), 0);
                close(connfd);
                return;
            }
            init_timer( connfd, saddr );
        }
    }
    return;
}

void Webserver::dealwithread(int sockfd){
    util_timer* timer = m_users[sockfd].m_timer;
    if (m_ActorMode == 0){
        // Proactor 
        if (m_users[sockfd].read()){ 
            adjust_timer(timer);
            m_pool -> append(&m_users[sockfd]);
        }
        else{
            del_timer(timer, sockfd);
        }
    }
    else{
        // Reactor: 等工作线程读完判断是否成功，如果没有成功则删除定时器
        adjust_timer(timer);
        m_pool -> append(&m_users[sockfd]);
        while(1){
            if (m_users[sockfd].m_finish == 1){
                if (m_users[sockfd].m_timerflag == 1){
                    del_timer(timer, sockfd);
                    m_users[sockfd].m_timerflag = 0;
                }
                m_users[sockfd].m_finish = 0;
                break;
            }
        }
    }
}

void Webserver::dealwithwrite(int sockfd){
    util_timer* timer = m_users[sockfd].m_timer;
    if (m_ActorMode == 0){
        // Proactor 
        if (m_users[sockfd].write()){
            adjust_timer(timer);
        }
        else{
            del_timer(timer, sockfd);
        }
    }
    else{
        // Reactor: 等工作线程写完判断是否成功，如果没有成功则删除定时器
        adjust_timer(timer);
        m_pool -> append(&m_users[sockfd]);
        while(1){
            if (m_users[sockfd].m_finish == 1){
                if (m_users[sockfd].m_timerflag == 1){
                    del_timer(timer, sockfd);
                    m_users[sockfd].m_timerflag = 0;
                }
                m_users[sockfd].m_finish = 0;
                break;
            }
        }
    }
}   

void Webserver::dealwithsignal(bool& timeout, bool& stopserver){

    int ret = 0;
    char signals[1024];
    ret = recv(pipefd[0], signals, sizeof(signals) , 0);
    if (ret <= 0){
        printf("errno is %d, signal recv error\n", errno); 
        return;
    }
    else{
        for (int i = 0; i < ret; ++i){
            switch(signals[i]){
                case SIGALRM:{
                    timeout = true;
                    break;
                }
                case SIGTERM:{
                    stopserver = true;
                    break;
                }
            }
        }
    }
}

void Webserver::eventloop(){
    bool timeout = false;
    bool stopserver = false;

    while( !stopserver ){
        int eventnum = epoll_wait(m_epollfd, m_events, MAX_EVENT_NUMBER, -1);
        if (eventnum < 0 && errno != EINTR)
        {
            printf("%s", "epoll failure");
            break;
        }
        for (int i = 0; i < eventnum; i++){
            int sockfd = m_events[i].data.fd;
            if (sockfd == m_listenfd){
                dealwithclient();
            }
            else if (sockfd == pipefd[0] && (m_events[i].events & EPOLLIN)){
                dealwithsignal(timeout, stopserver);
            }
            else if (m_events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                util_timer* timer = m_users[sockfd].m_timer;
                del_timer(timer, sockfd);
            }
            else if (m_events[i].events & EPOLLIN){
                dealwithread(sockfd);
            }
            else if (m_events[i].events & EPOLLOUT){
                dealwithwrite(sockfd);
            }
            if (timeout){
                m_timer_lst.tick();
                printf("timer tick\n");
                alarm(TIMESLOT);
                timeout = false;
            }
        }
    }
}
