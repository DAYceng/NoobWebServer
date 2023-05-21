#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include "./locker/locker.h"
#include "./threadpool/threadpool.h"
#include <signal.h>
#include "./httpdealer/http_conn.h"

#define MAX_FD 65535 //最大的文件描述符个数
#define MAX_EVENT_NUMBER 10000 //一次监听的最大事件数

//添加信号捕捉
void addsig(int sig, void(handler)(int)){//信号处理函数
    struct sigaction sa;//创建信号量
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);//设置信号临时阻塞等级
    sigaction(sig, &sa, NULL);//注册信号
}

//模拟proactor模式，主线程监听事件
//当有读事件产生，在主线程中一次性读出来，封装成一个任务对象（用任务类）
//然后交给子线程（线程池队列中的工作线程），线程池再去取任务做任务

//添加文件描述符到epoll中
extern void addfd(int epollfd, int fd, bool one_shot);
//从epoll删除文件描述符
extern void removefd(int epollfd, int fd);

//修改文件描述符
extern void modfd(int epollfd, int fd, int ev);


int main(int argc, char* argv[]){
    
    //判断参数个数，至少要传递一个端口号
    if(argc <= 1){
        printf("按照如下格式运行: %s port_number\n", basename(argv[0]));
        exit(-1);
    }

    //获取端口号，转换成整数
    int port = atoi(argv[1]);

    //对SIGPIPE信号进行处理
    addsig(SIGPIPE, SIG_IGN);

    //创建线程池，并初始化
    //任务类：http_conn
    //来一个任务之后，要封装成一个任务对象，交给线程池去处理
    threadpool<http_conn>* pool = NULL;
    try{
        pool = new threadpool<http_conn>; 
    }catch(...){
        exit(-1);
    }

    //创建一个数组用于保存所有的客户端信息
    //users 数组是一个存储 http_conn 对象的数组，每个 http_conn 对象代表一个客户端连接。
    http_conn* users  = new http_conn[MAX_FD];
    
    //写网路通信的代码
    //创建监听的套接字
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    //tcp服务端代码
    //设置端口复用（一定要在绑定之前设置）
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    //绑定
    //绑定的作用是将服务器的端口号和 IP 地址与一个套接字绑定，
    //使得客户端可以通过相应的 IP 地址和端口号访问到服务器。
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);//大端小端转换为网络字节序
    bind(listenfd, (struct sockaddr*)&address, sizeof(address));

    //监听
    //将listenfd这个socket的状态设置为监听状态，等待客户端的连接请求。
    listen(listenfd, 5);
    //listenfd会触发一个可读事件，从而被加入到epoll对象中，并由events数组保存。

    // 创建epoll对象，事件数组，添加监听的文件描述符
    // events 数组的作用是存储 epoll_wait() 函数返回的事件
    epoll_event events[MAX_EVENT_NUMBER];
    // 用于事件管理的epoll对象的文件描述符
    int epollfd = epoll_create(5);//创建epoll对象

    //将监听的文件描述符添加到epoll对象中
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;//赋值
    
    while(true){//主线程不断循环检测有无事件发生
        /*epoll_wait()会等待事件的发生，一旦事件发生，
        会将该事件的相关信息存储到events数组中，
        主线程会遍历该数组并处理所有发生的事件。*/
        //num代表检测到几个事件
        //调用 epoll_wait() 函数时，我们需要将一个用于存储事件的数组传递给该函数，
        //函数会将检测到的事件存储到该数组中。
        //遍历该数组可以获取到每个事件对应的文件描述符以及该事件所对应的事件类型，根据不同的事件类型，我们可以采取不同的处理方式。
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if(num < 0 && errno != EINTR){
            printf("epoll failure\n");
            break;
        }

        //循环遍历事件数组
        //注意，此时num>0，意味着事件数组events中肯定有元素在，即检测到有事件发生
        for(int i = 0; i < num; i++){
            //从事件数组中取出epoll_wait()检测到的事件，即客户端的连接请求
            /*当events中存储的事件为sockfd可读事件时，表示该socket有数据可读，
            此时应该将读事件交由工作线程去处理。
            当events中存储的事件为sockfd可写事件时，表示该socket可以写入数据，
            此时应该将写事件交由工作线程去处理*/
            int sockfd = events[i].data.fd;//sockfd只是一个名称，表示由epoll_wait()等待并检测到的事件
            /*在服务器中，通常会使用一个监听socket（listenfd）来接受客户端的连接请求，
            当有新的客户端连接到来时，服务器会使用accept函数创建一个新的连接socket（connfd），
            这个新的socket会与客户端的socket建立起通信连接。*/
            if(sockfd == listenfd){
                //有新的客户端连接进来
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);

                int connfd = accept(listenfd, (struct sockaddr*)&client_address, &client_addrlen);

                if(http_conn::m_user_count >= MAX_FD){
                    //目前连接满了
                    printf("服务器正忙...\n");
                    close(connfd);
                    continue;
                }
                //将新的客户的数据初始化，放到数组中
                /*每当有一个新的客户端连接请求到来时，
                服务器会创建一个新的 http_conn 对象，并将该对象添加到 users 数组中，
                以管理这个客户端连接。*/
                users[connfd].init(connfd, client_address);
            }else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                //对方异常断开或错误异常
                users[sockfd].close_conn();
            }else if(events[i].events & EPOLLIN){
                //判断是否有读事件发生
                if(users[sockfd].read()){//一次性读出数据, read()
                    //成功读完后要交给工作线程处理
                    //调用线程池，追加任务
                    //线程池执行run函数，不断从队列去取
                    //取到就做业务处理，解析、生成响应数据
                    pool->append(users + sockfd);//将 users + sockfd 所指向的 http_conn 对象追加到线程池的任务队列中。
                    /*users 数组中的每个元素都代表一个客户端连接，
                    数组的下标是该客户端的文件描述符 fd。
                    users + sockfd 就是获取到该客户端连接的 http_conn 对象的指针。
                    然后将该指针作为参数，调用线程池对象的 append 函数，
                    将该指针所指向的 http_conn 对象添加到线程池的任务队列中，
                    等待线程池的工作线程来处理。*/
                }else{//读失败，关闭
                    users[sockfd].close_conn();
                }
            }else if(events[i].events & EPOLLOUT){
                if(!users[sockfd].write()){
                    users[sockfd].close_conn();
                }
            }
        }
    }
    close(epollfd);
    close(listenfd);
    delete[] users;
    delete pool;

    return 0;
}
