#ifndef LOCKER_H //没定义就定义一个LOCKER_H
#define LOCKER_H

#include <pthread.h>//互斥锁相关
#include <exception>
#include <semaphore.h>//信号量相关
//线程头部机制的封装类
//互斥锁类
class locker{
private:
    pthread_mutex_t m_mutex;

public:
    locker(){
        if(pthread_mutex_init(&m_mutex, NULL) != 0){
            throw std::exception();//抛出异常
        }
    }

    ~locker(){//析构函数，销毁
        pthread_mutex_destroy(&m_mutex);
    }

    bool lock(){//上锁
        return pthread_mutex_lock(&m_mutex) == 0;
    }

    bool unlock(){
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    pthread_mutex_t * get(){//获取互斥量
        return &m_mutex;
    }
};

//条件变量类
//判断队列中有无数据，没有就让线程停着，有就唤醒线程
class cond {
private:
    pthread_cond_t m_cond;

public:
    cond(){//构造函数
        if (pthread_cond_init(&m_cond, NULL) != 0) {
            throw std::exception();
        }
    }
    ~cond() {//析构函数
        pthread_cond_destroy(&m_cond);
    }

    bool wait(pthread_mutex_t *m_mutex) {
        int ret = 0;
        ret = pthread_cond_wait(&m_cond, m_mutex);
        return ret == 0;
    }
    bool timewait(pthread_mutex_t *m_mutex, struct timespec t) {//超时
        int ret = 0;
        ret = pthread_cond_timedwait(&m_cond, m_mutex, &t);
        return ret == 0;
    }
    bool signal() {//唤醒一个或多个线程
        return pthread_cond_signal(&m_cond) == 0;
    }
    bool broadcast() {//唤醒所有线程
        return pthread_cond_broadcast(&m_cond) == 0;
    }
};


//信号量类
class sem{
private:
    sem_t m_sem;
public:
    sem(){
        if(sem_init(&m_sem, 0, 0) != 0){
            throw std:: exception();
        }
    }

    ~sem(){
        sem_destroy(&m_sem);
    }

    //等待信号量
    bool wait(){
        return sem_wait(&m_sem) == 0;
    }

    //增加信号量
    bool post(){
        return sem_post(&m_sem) == 0;
    }


};


#endif