#ifndef HTTP_CONN_H
#define HTTP_CONN_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include "locker.h"
#include <sys/uio.h>

#include "lst_timer.h"

using namespace std;

class http_conn
{
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 2048;

    // 请求方法，这里只支持GET
    enum METHOD {GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};
    
    /*
        解析客户端请求时，主状态机的状态
        CHECK_STATE_REQUESTLINE:当前正在分析请求行
        CHECK_STATE_HEADER:当前正在分析头部字段
        CHECK_STATE_CONTENT:当前正在解析请求体
    */
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };

    // 从状态机的三种可能状态，即行的读取状态，分别表示
    // 1.读取到一个完整的行 2.行出错 3.行数据尚且不完整
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };
    
    /*
        服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST          :   请求不完整，需要继续读取客户数据
        GET_REQUEST         :   表示获得了一个完成的客户请求
        BAD_REQUEST         :   表示客户请求语法错误
        NO_RESOURCE         :   表示服务器没有资源
        FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
        FILE_REQUEST        :   文件请求,获取文件成功
        INTERNAL_ERROR      :   表示服务器内部错误
        CLOSED_CONNECTION   :   表示客户端已经关闭连接了
    */
    enum HTTP_CODE { NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION };
    


    
public:
    http_conn(){}
    ~http_conn(){}

    void init(int sockfd, const sockaddr_in& addr, int TRIGMODE);
    void close_conn();
    bool read();
    bool write();
    void process();

private:
    void init();
    HTTP_CODE process_read(); //解析HTTP请求
    bool process_write( HTTP_CODE ret ); //填充HTTP应答
    
    //以下一组函数被process_read调用以分析HTTP请求
    LINE_STATUS parse_line(); //解析一行
    char* get_line(){ return m_read_buf + m_start_line; } // 返回一行数据
    HTTP_CODE parse_request_line( char* text ); // 解析请求行
    HTTP_CODE parse_headers( char* text ); //解析请求头
    HTTP_CODE parse_content( char* text ); //解析请求体
    HTTP_CODE do_request(); //响应函数

    //这一组函数被process_write调用以填充HTTP应答
    void unmap();
    bool add_response( const char* format, ... ); //按照format写一行应答
    bool add_content( const char* content ); //写错误信息
    bool add_status_line( int status, const char* title ); //写状态行
    bool add_headers( int content_length ); //写头部
    bool add_content_length( int content_length );
    bool add_content_type(); 
    bool add_linger(); //添加是否keep-alive的信息
    bool add_blank_line(); //写空行 


public:
    //所有的客户端共享的epollfd和链接进来的客户端数
    static int m_epollfd;
    static int m_user_count;

    // 为当前客户连接添加定时器
    util_timer* m_timer;

    // Reactor模式下变量
    int m_state; // 当前所处读/写状态，0表示读，1表示写
    int m_finish; // 工作线程是否读完/写完，1表示完成，0表示未完成
    int m_timerflag; // 是否需要删除定时器，1表示需要删除，0表示不需要

private:
    //当前客户端占用的socketfd以及客户端的地址
    int m_sockfd;
    int m_fd;
    sockaddr_in m_sockaddr;
    //将这个socketfd中的内容读到m_read_buf缓冲区中，m_read_idx(偏移量)代表当前已经读到缓冲区的数据结束位置的下一个字节
    char m_read_buf[ READ_BUFFER_SIZE ];
    int m_read_idx;
    
    int m_checked_idx; //当前正在解析的字符在读缓冲区中的位置
    int m_start_line; //当前正在解析的行的起始位置

    CHECK_STATE m_check_state; //主状态机当前所属的状态

    //请求行的三个信息
    char* m_url;
    char* m_version;
    METHOD m_method;

    //请求头部的信息
    char* m_host;
    bool m_linger; //是否保持连接
    int m_content_length;

    //要发回的文件信息
    char m_real_file[ FILENAME_LEN ]; //文件名
    struct stat m_file_stat; 
    char* m_file_address; //内存映射区的地址
    
    //写缓冲区
    char m_write_buf[ WRITE_BUFFER_SIZE ];
    int m_write_idx;

    //使用writev来执行写操作，将多个缓冲区中的数据写到一个fd中
    struct iovec m_iv[2]; 
    int m_iv_count;

    int bytes_to_send;
    int bytes_have_send;

    // 触发模式，ET:1, LT:0
    int m_TRIGMode;
};

#endif