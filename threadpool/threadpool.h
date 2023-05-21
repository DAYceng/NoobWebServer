#ifndef THREADPOOL_H 
#define THREADPOOL_H

#include <pthread.h>
#include <list>
#include <stdio.h>
#include "../locker/locker.h"

//线程池类，将其定义为模板类是为了代码的复用
//模板参数T就是任务类
template<typename T>
class threadpool {
private:
    //线程数量
    int m_thread_number;
    //线程池数组，大小为m_thread_number
    pthread_t * m_threads;//使用pthread_t一是为了性能，二是为了线程安全（相对于vector来说）
    //请求队列中最多允许的待处理请求数
    int m_max_requests;
    //请求队列
    std::list<T*> m_workqueue;
    //互斥锁
    locker m_queuelocker;
    //信号量，用于判断是否有任务需要处理
    sem m_queuestat;
    //是否结束线程
    bool m_stop;

private:
    //子线程中要执行的代码
    static void* worker(void* arg);
    void run();

public:
    threadpool(int thread_number = 8, int max_request = 10000);
    ~threadpool();

    bool append(T* request);

};
//模板外实现线程池构造函数
template<typename T>
threadpool<T>::threadpool(int thread_number, int max_requests):
    m_thread_number(thread_number),m_max_requests(max_requests),
    m_stop(false), m_threads(NULL){
        //异常判断，线程数和最大请求数小于0，报错
        if((thread_number <= 0) || (max_requests <= 0)){
            throw std:: exception();
        }

        m_threads = new pthread_t[m_thread_number];//创建线程池数组
        if(!m_threads){
            throw std:: exception();
        }
        //创建thread_number个线程，并将它们设置为线程脱离
        //线程脱离指的是在一个多线程程序中，某个线程完成了它原本需要执行的任务之后，
        //并不立即结束自己的执行，而是继续保持运行状态，直到其他线程也完成了它们的任务之后才退出。
        //这种情况下，该线程被称为“脱离线程”（detached thread）
        /*线程脱离通常用于需要长时间运行的后台任务，通过将这些任务单独分配给脱离线程来处理，可以避免阻塞主线程和其他相关线程的运行。*/
        for(int i = 0; i < thread_number; ++i){
            printf("创建第 %d 个线程\n", i);
            //C++里面的woker是静态的，所以要传入this来访问类里变量
            if(pthread_create(m_threads + i, NULL, worker, this) != 0){//为了让worker访问非静态成员，传入this
                delete[] m_threads;
                throw std::exception();//创建失败
            }
            if(pthread_detach(m_threads[i])){//在调用pthread_detach()函数之后，线程将进入“分离”状态，这意味着它不能再被其他线程或主线程等待和加入。
            }
        }
    }

//实现析构函数  
template<typename T>
threadpool<T>::~threadpool(){
    delete[] m_threads;//用完之后就把线程池数组删除
    m_stop = true;//执行析构函数时将其置为true，供线程判断是否要停止
}

//实现append
template<typename T>
bool threadpool<T>::append(T* request){//往队列中添加任务，要保证线程同步
    m_queuelocker.lock();//添加互斥锁
    if(m_workqueue.size() > m_max_requests){//任务队列大小大于最大请求数
        m_queuelocker.unlock();//解锁并报错，此时的任务数已经超出上限
        return false;
    }

    m_workqueue.push_back(request);//往队列中增加一个请求
    m_queuelocker.unlock();//解锁
    //将请求加入工作队列的操作是需要保证其原子性的，因此需要互斥锁保证多个进程不会争抢
    m_queuestat.post();//增加信号量，通知线程池中的线程，有新任务需要处理
    return true;
    /*当一个新的任务被添加到队列中时，会调用 m_queuestat.post() 增加信号量。
    在线程池初始化时，每个工作线程都被创建并阻塞在 m_queuestat.wait() 上等待信号量的触发。
    一旦 m_queuestat 的值大于 0，其中的一个线程就会从阻塞状态唤醒并开始处理队列中的请求。*/
}

template<typename T>//线程池的工作函数，其中模板参数T未被使用。该函数是作为新线程启动时调用的入口函数
void* threadpool<T>::worker(void* arg){
    // 传入void 类型指针 arg 
    /*arg 是在启动线程时传递给该线程函数的参数。
    以下代码中，它被转换为 threadpool* 类型，因为它实际上是一个指向 threadpool 结构体的指针。
    然后，将这个指针赋值给名为 pool 的变量，以便在该函数中访问和操作 threadpool 结构体的成员。*/
    threadpool* pool = (threadpool* ) arg;//在pthread_create中传入worker
    pool->run();//启动线程池中的一个或多个线程，并将待处理任务提交给线程池进行处理
    return pool;
}

template<typename T>
void threadpool<T>::run(){
    while(!m_stop){
        m_queuestat.wait();//等待append函数传过来的信号量，收到表示需要运行线程池，使用其中的线程处理来处理任务
        //可能有数据到了，上锁
        /*关于为什么这里要上锁：
            收到信号量时，任务队列 m_workqueue 可能为空，也可能不为空，这取决于在等待信号量之前是否有新任务被添加到了队列中。
            如果没有新任务被添加，那么 m_workqueue 仍然为空。如果有新任务被添加，那么 m_workqueue 将不为空。
            需要注意的是，在多线程编程中，一个线程在等待信号量时，另一个线程可能会往任务队列中添加新任务，因此需要通过加锁（比如互斥锁）来保证对任务队列的访问是线程安全的。
            这样可以避免出现竞态条件（race condition，也就是线程不同步），从而确保程序的正确性。
        */
        m_queuelocker.lock();
        /*在等待信号量的线程执行之前，如果没有任何其他线程向任务队列中添加新的任务，那么收到信号量时 m_workqueue 可能为空。这种情况可以出现在以下几种情况下：
            在初始化程序时，创建了一个空的任务队列并等待信号量，此时 m_workqueue 为空。
            所有的任务都已经被处理完毕，并且等待信号量的线程尚未收到新的任务添加进来。
            等待信号量的线程刚刚完成了处理该任务队列中的所有任务，然后又立即等待信号量，此时 m_workqueue 为空。
        */
        if(m_workqueue.empty()){
            m_queuelocker.unlock();//解锁
            continue;//继续循环，查看队列中是否有数据
        }
        //取出队列顶部的请求，并将其弹出队列
        T* request = m_workqueue.front();
        m_workqueue.pop_front();
        //取完请求后，解锁
        m_queuelocker.unlock();

        if(!request){
            continue;//没获取到就继续循环
        }
        //调用任务函数
        request->process();
    }
}



#endif