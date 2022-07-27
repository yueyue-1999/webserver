#include "http_conn.h"

// 定义HTTP响应的一些信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char*error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

// 定义服务器的根目录
const char* doc_root = "/home/yueyue/webserver/resources";

//初始化静态成员
int http_conn::m_user_count = 0;
int http_conn::m_epollfd = -1;

//对文件描述符设置非阻塞
int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl(fd, F_SETFL, new_option);
    return old_option;
}

// 向epoll中添加需要监听的文件描述符
void addfd( int epollfd, int fd, bool one_shot, int TRIGMODE) {
    epoll_event event;
    event.data.fd = fd;
    if (TRIGMODE == 1) // ET模式
        event.events = EPOLLIN | EPOLLRDHUP | EPOLLET;
    else // LT模式
        event.events = EPOLLIN | EPOLLRDHUP;
    
    if(one_shot) // 防止同一个通信被不同的线程处理
        event.events |= EPOLLONESHOT;
    
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);    
    setnonblocking(fd);
}

// 从epoll中移除监听的文件描述符
void removefd( int epollfd, int fd ) {
    epoll_ctl( epollfd, EPOLL_CTL_DEL, fd, 0 );
    close(fd);
}

// 修改文件描述符，重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发
void modfd(int epollfd, int fd, int ev, int TRIGMODE) {
    epoll_event event;
    event.data.fd = fd;
    if (TRIGMODE == 1) // ET模式
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP | EPOLLET;
    else // LT模式
        event.events = ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, fd, &event );
}

//关闭一个链接, 取消I/O监听，客户数-1
void http_conn::close_conn()
{
    if (m_sockfd != -1)
    {
        printf("close %d\n", m_sockfd);
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

//初始化链接
void http_conn::init(int sockfd, const sockaddr_in& addr, int TRIGMODE )
{
    m_sockaddr = addr;
    m_sockfd = sockfd;
    //设置端口复用
    int reuse = 1;
    setsockopt(m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    addfd( m_epollfd, sockfd, true, m_TRIGMode ); // 注册读事件
    m_user_count++;

    m_TRIGMode = TRIGMODE;

    init();
}

void http_conn::init()
{
    m_check_state = CHECK_STATE_REQUESTLINE; //初始化为正在读取请求行
    m_checked_idx = 0;
    m_start_line = 0;
    m_read_idx = 0;
    m_write_idx = 0;

    //初始化请求行信息
    m_url = 0;
    m_version = 0;
    m_method = GET;

    //初始化头部信息
    m_host = 0;
    m_content_length = 0;
    m_linger = false;

    bytes_to_send = 0;
    bytes_have_send = 0;

    //    
    int m_state = 0; 
    int m_finish = 0; 
    int m_timerflag = 0;

    bzero(m_read_buf, READ_BUFFER_SIZE);
    bzero(m_write_buf, WRITE_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}

//一次性将所有socketfd中的数据读取到m_read_buf缓冲区中
bool http_conn::read()
{
    // 如果当前需要读取的下一个字节的偏移量已经超过缓冲区大小，返回 false
    if (m_read_idx > READ_BUFFER_SIZE) return false;

    int bytes_read = 0;
    //LT读取数据
    if (m_TRIGMode == 0)
    {
        bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes_read;

        if (bytes_read <= 0)
        {
            return false;
        }

        return true;
    }
    //ET读数据
    else
    {
        while (true)
        {
            bytes_read = recv(m_sockfd, m_read_buf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes_read == -1)
            {
                if (errno == EAGAIN || errno == EWOULDBLOCK)
                    break; // 数据读完了
                return false;
            }
            else if (bytes_read == 0)
            {
                return false; // 对方断开了连接
            }
            m_read_idx += bytes_read;
        }
        return true;
    }
}

// 解析一行数据，依据 \r\n
// 从 m_read_buf 中解析数据
// 将 \r\n 变为 \0\0，这样在getline时就可以只读出一行数据了
http_conn::LINE_STATUS http_conn :: parse_line(){
    char temp;
    for (; m_checked_idx < m_read_idx; m_checked_idx++)
    {
        temp = m_read_buf[m_checked_idx];
        if (temp == '\r')
        {
            if (m_checked_idx + 1 == m_read_idx) return LINE_OPEN;
            else if (m_read_buf[m_checked_idx + 1] == '\n')
            {
                m_read_buf[m_checked_idx++] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            else return LINE_BAD;
        }
        else if (temp == '\n')
        {
            if (m_checked_idx >= 1 && m_read_buf[m_checked_idx - 1] == '\r')
            {
                m_read_buf[m_checked_idx - 1] = '\0';
                m_read_buf[m_checked_idx++] = '\0';
                return LINE_OK;
            }
            else return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

http_conn::HTTP_CODE http_conn :: parse_request_line( char* text ){
    // GET /index.html HTTP/1.1
    m_url = strpbrk(text, " \t"); // 判断第二个参数中的字符哪个在text中最先出现
    if (! m_url) { 
        return BAD_REQUEST;
    }
    // GET\0/index.html HTTP/1.1
    *m_url++ = '\0';    // 置位空字符，字符串结束符
    char* method = text;
    if ( strcasecmp(method, "GET") == 0 ) { // 忽略大小写比较
        m_method = GET;
    } else {
        return BAD_REQUEST;
    }
    // /index.html HTTP/1.1
    // 检索字符串 str1 中第一个不在字符串 str2 中出现的字符下标。
    m_version = strpbrk( m_url, " \t" );
    if (!m_version) {
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    if (strcasecmp( m_version, "HTTP/1.1") != 0 ) {
        return BAD_REQUEST;
    }
    /**
     * http://192.168.110.129:10000/index.html
    */
    if (strncasecmp(m_url, "http://", 7) == 0 ) {   
        m_url += 7;
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
        m_url = strchr( m_url, '/' );
    }
    if ( !m_url || m_url[0] != '/' ) {
        return BAD_REQUEST;
    }
    m_check_state = CHECK_STATE_HEADER; // 检查状态变成检查头
    return NO_REQUEST; // 请求数据不完整，还需要继续获取客户端数据
}

http_conn::HTTP_CODE http_conn :: parse_headers( char* text ){
    if (text[0] == '\0') //如果遇到空行，说明头部解析完毕
    {
        //如果body部分有内容，则需要将当前状态改为解析body，并返回headers的解析结果
        if (m_content_length != 0)
        {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST; //未解析完
        }
        return GET_REQUEST; //如果没有body，返回已经解析完
    }
    else if (strncasecmp(text, "Connection:", 11) == 0) //strncasecmp(s1,s2,n)如果s1的n个字符与s2匹配，则返回0
    {
        text += 11;
        text += strspn(text, " \t"); //strspn(s1,s2)返回s1初始部分中s2中字符的个数
        if (strncasecmp(text, "keep-alive", 10) == 0) //如果要保持连接，设置m_linger
        {
            m_linger = true;
        }
    }
    else if (strncasecmp(text, "Content-Length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text); //将char转化为long
    }
    else if (strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text; //将char转化为long
    }
    else printf("oop! unknow header %s\n", text);
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn :: parse_content( char* text ){
    if (m_checked_idx + m_content_length <= m_read_idx)
    {
        text[m_content_length] = '\0';
        return GET_REQUEST;
    }
    //否则，说明缓冲区中的报文不完整
    else return NO_REQUEST;
}

//主状态机，解析请求
http_conn::HTTP_CODE http_conn::process_read(){
    LINE_STATUS line_status = LINE_OK;
    HTTP_CODE ret = NO_REQUEST;
    char* text = 0;
    while(((m_check_state == CHECK_STATE_CONTENT) && line_status == LINE_OK) || ((line_status = parse_line()) == LINE_OK))
    {
        text = get_line(); //获取一行数据
        m_start_line = m_checked_idx;
        printf("got 1 http line : %s\n", text);

        switch ( m_check_state ) //以下内容没有考虑所有情况，简单版本
        {
            case CHECK_STATE_REQUESTLINE:{
                ret = parse_request_line( text );
                if (ret == BAD_REQUEST) return BAD_REQUEST;
                break;
            }
            case CHECK_STATE_HEADER:{
                ret = parse_headers( text );
                if (ret == BAD_REQUEST) return BAD_REQUEST;
                else if (ret == GET_REQUEST) return do_request(); //如果获取到了完整的客户端请求，解析具体请求信息
                break;
            }
            case CHECK_STATE_CONTENT:{
                ret = parse_content( text ); // 用于解析POST请求 
                if (ret == GET_REQUEST) return do_request();
                line_status = LINE_OPEN; // 解析完消息体即完成报文解析，防止再次进入循环
                break;
            }
            default:{
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

// 如果得到了一个完整的，正确的HTTP请求，则分析目标文件的属性
// 如果目标文件存在，对others可读，且不是目录，
// 则使用mmap将其映射到内存地址m_file_address处，并告知调用者获取文件成功(FILE_REQUEST)
http_conn::HTTP_CODE http_conn::do_request()
{
    //m_real_file = "/home/yueyue/webserver/resources" + "/index.html"
    strcpy(m_real_file, doc_root);
    int len = strlen( doc_root );
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1); //strncpy(dest, src, n) 最多有n个src中的字符被复制了

    //stat函数获取m_real_file文件的统计信息，并传给m_file_stat。成功返回0，失败返回-1
    if ( stat(m_real_file, &m_file_stat) < 0) 
    {
        return NO_RESOURCE;
    }

    //如果others没有读权限
    if ( !(m_file_stat.st_mode & S_IROTH) )
    {
        return FORBIDDEN_REQUEST;
    }

    //请求的资源不能是目录
    if ( m_file_stat.st_mode & S_IFDIR )
    {
        return BAD_REQUEST;
    }

    //以只读方式打开文件，将文件映射到内存中
    int fd = open(m_real_file, O_RDONLY);
    m_file_address = (char*)mmap(NULL, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST; //获取文件成功
}

void http_conn::unmap(){
    if ( m_file_address ){
        munmap( m_file_address, m_file_stat.st_size );
        m_file_address = 0;
    }
}

//将要添加的内容写入到write_buf中
bool http_conn::add_response( const char* format, ... ){
    //如果写入内容超出m_write_buf大小则报错
    if (m_write_idx >= WRITE_BUFFER_SIZE) return false;

    //定义可变参数列表
    va_list arg_list; 

    //将变量arg_list初始化为传入参数
    va_start( arg_list, format );

    //将数据format从可变参数列表写入写缓冲区，返回写入数据的长度
    int len = vsnprintf( m_write_buf + m_write_idx, WRITE_BUFFER_SIZE - m_write_idx - 1, format, arg_list);
    
    //如果写入的数据长度超过缓冲区剩余空间，则报错
    if (len >= (WRITE_BUFFER_SIZE - m_write_idx - 1)){
        return false;
    }
    //更新m_write_idx位置
    m_write_idx += len;
    
    //清空可变参列表
    va_end( arg_list );
    return true;
}

//添加应答行
bool http_conn::add_status_line( int status, const char* title ){
    return add_response("%s %d %s\r\n", m_version, status, title);
}

//添加头部信息
bool http_conn::add_headers( int content_length ){
    return add_content_length(content_length) && 
    add_content_type() && add_linger() && add_blank_line();
}

bool http_conn::add_content_length( int content_length ){
    return add_response("Content-length: %d\r\n", content_length);
}

bool http_conn::add_content_type(){
    return add_response("Content-Type: %s\r\n", "text/html");
}

bool http_conn::add_linger(){
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive":"close");
}

bool http_conn::add_blank_line(){
    return add_response("%s", "\r\n");
}

//帮助打印错误信息
bool http_conn::add_content( const char* content ){
    return add_response("%s", content);
}

// 依据服务器处理HTTP的结果，决定返回给客户端的内容
// 将 header 的内容写入到 m_write_buf 中
// 如果要发回文件，要把文件所在的位置，以及 m_write_buf 的位置告诉主线程
bool http_conn::process_write(HTTP_CODE ret)
{
    switch(ret)
    {
        case INTERNAL_ERROR:{
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ));
            if (!add_content( error_500_form ))
            {
                return false;
            }
            break;
        }
        case NO_RESOURCE:{
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ));
            if (! add_content( error_404_form )) return false;
            break;
        }
        case BAD_REQUEST:{
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ));
            if (!add_content( error_400_form ))
            {
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST:{
            add_status_line( 403, error_403_title );
            add_headers( strlen( error_403_form ));
            if (!add_content( error_403_form ))
            {
                return false;
            }
            break;
        }
        case FILE_REQUEST:{
            add_status_line( 200, ok_200_title );
            if (m_file_stat.st_size != 0)
            {
                // 初始化 m_iv 信息，以及 bytes_to_send 的值 
                add_headers(m_file_stat.st_size);
                m_iv[0].iov_base = m_write_buf;
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            }
            else
            {
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                    return false;
            }
        }
        default: return false;
    }
    
    // 如果出现错误，返回错误信息（将 write_buf 中的内容返回）
    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_idx;
    m_iv_count = 1;
    bytes_to_send = m_write_idx;
    return true;

}

//处理http请求的入口函数
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) //请求不完整，需要继续读取客户端数据
    {
        modfd( m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode ); //重新注册可读与EPOLLONESHOT
        return;
    }

    bool write_ret = process_write( read_ret );
    if ( !write_ret )
    {
        close_conn();
    }
    modfd( m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode );
}

//将m_write_buf中的报文内容和m_file_address处的文件内容一起写到客户端 socket
bool http_conn::write()
{
    int temp = 0;

    if (bytes_to_send == 0){
        modfd( m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode );
        init();
        return true;
    }
    while(true)
    {
        // writev将m_iv中多块缓冲区的信息写入同一块fd
        temp = writev( m_sockfd, m_iv, m_iv_count );
        if (temp <= -1)
        {
            // 如果TCP写缓冲区的资源暂时不可用，则监听等待写事件
            if ( errno == EAGAIN ){
                modfd( m_epollfd, m_sockfd, EPOLLOUT, m_TRIGMode );
                return true; 
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;

        // iv[0]缓冲区内容已经发送完毕了
        if (bytes_have_send >= m_iv[0].iov_len){
            m_iv[0].iov_len = 0;
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        }
        // iv[0]缓冲区内容还没有发送完
        else{
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            m_iv[0].iov_len = m_iv[0].iov_len - temp;
        }

        if (bytes_to_send <= 0)
        {
            unmap();
            modfd( m_epollfd, m_sockfd, EPOLLIN, m_TRIGMode );

            if (m_linger)
            {
                init();
                return true;
            }
            else return false;
        }
    }
}
