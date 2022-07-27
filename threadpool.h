#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <stdio.h>
#include <list>
#include "locker.h"

template <typename T> //定义模板类
class threadpool{
public:
    threadpool(int actor_model = 0, int thread_number = 8, int max_work_number = 10000);
    ~threadpool();
    bool append(T* request);

private:
    // 工作线程运行的函数，不断从工作队列中取出任务并执行之
    // worker 需要设置为静态函数，原因：
    // pthread_create的函数原型中第三个参数的类型为函数指针，指向的线程处理函数参数类型为(void *),
    // 若线程函数为类成员函数，则this指针会作为默认参数被传进函数中，和线程函数参数(void*)不能匹配，不能通过编译。
    static void* worker(void* arg);
    void run();

private:
    // 线程的数量
    int m_thread_number; 

    // 线程池数组，大小为 m_thread_number
    pthread_t* m_threads;

    // 最大请求数量
    int m_max_work_number;

    // 请求队列
    std::list<T*> m_work_queue;

    // 保护请求队列的互斥锁
    locker m_queuelocker;

    // 信号量，通知有任务
    sem m_queuestat;

    // 模型切换
    int m_actor_model;
};

//创建线程池，分配线程池空间
template <typename T>
threadpool<T> :: threadpool(int actor_model, int thread_number, int max_work_number) :
m_thread_number(thread_number), m_max_work_number(max_work_number), m_actor_model(actor_model), m_threads(NULL)
{
    //如果申请的参数非法，抛出异常
    if (thread_number <= 0 || max_work_number <= 0) 
    {
        throw std::exception();
    }
    
    //创建线程数组，如果创建失败抛出异常
    m_threads = new pthread_t[thread_number]; 
    if (! m_threads)
    {
        throw std::exception();
    }

    //创建thread_number个线程，它们都去执行 worker 部分的代码，并设置线程分离自行销毁
    for (int i = 0; i < thread_number; i++)
    {
        printf( "create the %dth thread\n", i);
        if (pthread_create(m_threads + i, NULL, worker, this) != 0) //worker为静态，被所有线程共享
        {
            delete [] m_threads;
            throw std::exception();
        }

        if (pthread_detach(m_threads[i]) != 0)
        {
            delete [] m_threads;
            throw std::exception();
        }
    }
    
};

//析构函数
template< typename T >
threadpool<T> :: ~threadpool(){
    delete [] m_threads;
}

//append函数, 将工作添加到工作队列
template < typename T >
bool threadpool<T> :: append( T* request ) //int state
{
    //访问共享的工作队列，需要加锁
    m_queuelocker.lock();

    //如果当前工作队列中的任务数已经大于最大的任务数，添加工作失败
    if (m_work_queue.size() > m_max_work_number)
    {
        m_queuelocker.unlock();
        return false;
    }

    //将任务放入任务队列
    m_work_queue.push_back(request);

    //解锁
    m_queuelocker.unlock();

    //通知消费者进行消费
    m_queuestat.post();
    return true;
}

//所有子线程调用worker，worker会执行run函数
template < typename T >
void* threadpool<T> :: worker( void* arg )
{
    threadpool * pool = (threadpool * ) arg;
    pool -> run();
    return pool;
} 

//取出工作队列最前端的任务执行 process 函数
template< typename T >
void threadpool<T> :: run()
{
    while(true){
        //消费者等待消费
        m_queuestat.wait();

        //访问共享资源，加锁
        m_queuelocker.lock();

        if ( m_work_queue.empty() ) {
            m_queuelocker.unlock();
            continue;
        }
        
        //取出队列最前端的任务执行
        T* request = m_work_queue.front();
        m_work_queue.pop_front();
        m_queuelocker.unlock();

        if (!request) continue;

        if (m_actor_model == 1) // Reactor 模型
        {
            if (request->m_state == 0)
            {
                if (request->read())
                {
                    request->m_finish = 1;
                    request->process();
                }
                else
                {
                    request->m_finish = 1;
                    request->m_timerflag = 1;
                }
            }
            else 
            {
                if (request->write())
                {
                    request->m_finish = 1;
                }
                else
                {
                    request->m_finish = 1;
                    request->m_timerflag = 1;
                }
            }
        }
        else // Proactor 模型
        {
            request->process();
        }
    } 
}




#endif