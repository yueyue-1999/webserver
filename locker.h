#ifndef LOCKER_H
#define LOCKER_H

#include <pthread.h>
#include <semaphore.h>
#include <exception>

class locker{
public:

    locker() //初始化锁
    {
        //为什么要对初始化进行异常检查：
        if (pthread_mutex_init(&m_mutex, NULL) != 0)
        {
            throw std::exception();
        }
    }

    ~locker() //销毁锁
    {
        pthread_mutex_destroy(&m_mutex);
    }

    bool lock()
    {
        return pthread_mutex_lock(&m_mutex) == 0;
    }

    bool unlock()
    {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    pthread_mutex_t* get()
    {
        return &m_mutex;
    }

private:
    pthread_mutex_t m_mutex;
}; 

class sem{
public:

    sem()
    {
        if (sem_init(&m_sem, 0, 0) != 0) // 解决同步问题，因而信号量初始值为0
            throw std::exception();
    }
    ~sem()
    {
        sem_destroy(&m_sem);
    }
    bool wait()
    {
        sem_wait(&m_sem);
    }
    bool post()
    {
        sem_post(&m_sem);
    }

private:
    sem_t m_sem;
};

#endif