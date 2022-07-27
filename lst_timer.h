#ifndef LST_TIMER
#define LST_TIMER

#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <time.h>
#include <arpa/inet.h>
#include <signal.h>
#include <assert.h>
#include <sys/epoll.h>
#include <errno.h>

#define BUFFER_SIZE 64

class http_conn; //前向声明取代互相引用头文件

// 定时器类
class util_timer {
public:
    util_timer() : prev(nullptr), next(nullptr){}

public:
   time_t m_expire;   // 任务超时时间，这里使用绝对时间
   void (*m_cbfunc)( http_conn* ); // 任务回调函数，回调函数处理的客户数据，由定时器的执行者传递给回调函数
   http_conn* m_user_data; 
   util_timer* prev;    // 指向前一个定时器
   util_timer* next;    // 指向后一个定时器
};

// 定时器链表，它是一个升序、双向链表，且带有头节点和尾节点。
// 按照过期时间从小到大排列
class sort_timer_lst {
private:
    util_timer* head;   // 头结点
    util_timer* tail;   // 尾结点
public:
    sort_timer_lst()
    {
        head = new util_timer();
        tail = new util_timer();
        head -> next = tail;
        tail -> prev = head;
    }
    
    // 链表被销毁时，删除其中所有的定时器
    // 从前遍历删除
    ~sort_timer_lst() {
        util_timer* tmp = head;
        while( tmp ) {
            head = tmp->next;
            delete tmp;
            tmp = head;
        }
    }

    // 将目标定时器timer添加到链表中
    void push_back( util_timer* timer )
    {
        if (!timer) return;
        //插入到最后面
        timer -> prev = tail -> prev;
        tail -> prev = timer;
        timer -> prev -> next = timer;
        timer -> next = tail;
    }
    
    /* 当某个定时任务发生变化时，将该定时器挪动到末尾 */
    void adjust_timer( util_timer* timer )
    {
        if( !timer )  {
            return;
        }
        del_timer( timer );
        push_back( timer );
    }

    // 将目标定时器 timer 从链表中移除
    void del_timer( util_timer* timer )
    {
        if (!timer) return;
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev; 
    }

    /* SIGALARM 信号每次被触发就在其信号处理函数中执行一次 tick() 函数，以处理链表上到期任务。*/
    void tick() {
        if( head -> next == tail ) {
            return;
        }
        time_t cur = time( NULL );  // 获取当前系统时间
        util_timer* tmp = head -> next;
        util_timer* tmp2;
        // 从头节点开始依次处理每个定时器，直到遇到一个尚未到期的定时器
        while( tmp != tail ) {
            /* 因为每个定时器都使用绝对时间作为超时值，所以可以把定时器的超时值和系统当前时间，
            比较以判断定时器是否到期*/
            if( cur < tmp->m_expire ) {
                break;
            }

            // 调用定时器的回调函数，以执行定时任务
            // 注意：前向声明的类指针不能去操纵自己的对象，因而这里不可以用user_data指针去调用close_conn
            tmp->m_cbfunc( tmp->m_user_data );
            // 执行完定时器中的定时任务之后，就将它从链表中删除
            tmp2 = tmp -> next;
            del_timer( tmp );
            tmp = tmp2;
        }
    }
};

#endif