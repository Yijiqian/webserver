#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>       
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include "http_conn.h"
#include <signal.h>
#include <assert.h>

#define MAX_FD 65535 // 最大的文件描述符的个数
#define MAX_EVENT_NUMBER 10000   // 监听的最大事件数量

#define TIMESLOT 5

static int pipefd[2];
static sort_timer_lst timer_lst;

void sig_handler( int sig )
{
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

// 添加信号捕捉
void addsig(int sig, void(handler)(int))
{
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);
    assert(sigaction(sig, &sa, NULL) != -1);
    // sigaction(sig, &sa, NULL);

}

void timer_handler()
{
    // 定时处理任务，实际上就是调用tick()函数
    timer_lst.tick();
    // 因为一次 alarm 调用只会引起一次SIGALARM 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}

/*
    extern置于变量前表示声明，提示编译器该变量定义在其他文件或类中。需要注意的是在声明变量时不要赋值
    ，否则编译器会认为这不仅是声明，还是定义行为，会出现重复定义变量的错误
*/

// 添加文件描述符到epoll对象中
extern void addfd(int epollfd, int fd, bool one_shot);
// 从epoll中删除文件描述符
extern void removefd(int epollfd, int fd);
// 修改文件描述符
extern void modfd(int epollfd, int fd, int ev);

extern int setnonblocking(int fd);

int main(int argc, char* argv[])
{
    if(argc <= 1)
    {
        printf("usage: %s port_number\n", basename(argv[0]));
        exit(-1);
    }

    // 获取端口号
    int port = atoi(argv[1]);

    int ret = 0;
    // 对SIGPIE信号进行处理
    addsig(SIGPIPE, SIG_IGN);   // SIGPIPE：连接断开    SIG_ICN：忽略操作
    
    // 创建线程池，初始化线程池
    threadpool<http_conn> * pool = NULL;
    try{
        pool = new threadpool<http_conn>;
    } 
    catch(...){
        exit(-1);
    }

    // 创建数组，用于保存所有的客户端信息
    http_conn * users = new http_conn[ MAX_FD ];

    // 创建监听的套接字
    int listenfd = socket(PF_INET, SOCK_STREAM, 0);
    assert( listenfd >= 0 );

    // 设置端口复用
    int reuse = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 绑定
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    bind(listenfd, (struct sockaddr *)& address, sizeof(address));

    // 监听
    listen(listenfd, 5);
    

    // 创建epoll对象，时间数组，添加
    epoll_event events[MAX_EVENT_NUMBER];   //  MAX_EVENT_NUMBER = 10000
    int epollfd = epoll_create(5);

    // 将监听的文件描述符添加到epoll对象中
    addfd(epollfd, listenfd, false);

    // 创建管道
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    assert( ret != -1 );
    setnonblocking( pipefd[1] );
    addfd( epollfd, pipefd[0] , false);

    http_conn::m_epollfd = epollfd;

    // 设置信号处理函数
    addsig( SIGALRM , sig_handler);
    addsig( SIGTERM , sig_handler);

    bool stop_server = false;

    bool timeout = false;
    alarm(TIMESLOT);  // 定时,5秒后产生SIGALARM信号

    while(!stop_server)
    {
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUMBER, -1);
        if((num<0) && (errno != EINTR))
        {
            printf("epoll failure\n");
            break;
        }

        // 循环遍历事件数组
        for(int i=0; i<num;i++)
        {
            int sockfd = events[i].data.fd;
            if(sockfd == listenfd)
            {
                // 有客户端连接进来
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                int connfd = accept(listenfd, (struct sockaddr *)&client_address, &client_addrlen);

                if(connfd < 0)  // 接收失败，输出错误并跳出之后操作，重新开始循环
                {
                    printf("errno is: %d\n", errno);
                    continue;
                }
                if(http_conn::m_user_count >= MAX_FD)   // 已连接的客户端数超过最大可连接数，拒绝并关闭当前连接
                {
                    // 目前连接数满了
                    // 给客户端写一个信息：服务器正忙
                    close(connfd);
                    continue;
                }
                // 将新的客户的数据初始化，放到数组中
                users[connfd].init(connfd, client_address);

                util_timer* timer = new util_timer;
                timer->user_data = &users[connfd];
                time_t cur = time( NULL );
                timer->expire = cur + 3 * TIMESLOT;
                users[connfd].timer = timer;
                timer_lst.add_timer( timer );
            }
            else if( ( sockfd == pipefd[0] ) && ( events[i].events & EPOLLIN ) ) {
                // 操作系统产生SIGALRM或SIGTERM信号，pipefd[0]接收到这两个信号
                int sig;
                char signals[1024];
                ret = recv( pipefd[0], signals, sizeof( signals ), 0 );
                if( ret == -1 ) {
                    continue;
                } else if( ret == 0 ) {
                    continue;
                } else  {
                    for( int i = 0; i < ret; ++i ) {
                        switch( signals[i] )  {
                            case SIGALRM:
                            {
                                // 用timeout变量标记有定时任务需要处理，但不立即处理定时任务
                                // 这是因为定时任务的优先级不是很高，我们优先处理其他更重要的任务。
                                timeout = true;
                                break;
                            }
                            case SIGTERM:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }
            else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
            {
                // 对方异常断开或者错误等事件
                util_timer* timer = users[sockfd].timer;
                if(timer)
                {
                    printf("客户端异常断开或错误, 删除定时器\n");
                    timer_lst.del_timer(timer);
                }
                users[sockfd].close_conn();
            }
            else if(events[i].events & EPOLLIN)   // 接收到对方的请求，更新对应定时器的超时时间
            {
                util_timer* timer = users[sockfd].timer;  // timer为指针，指向对应定时器的内存地址
                if(users[sockfd].read())   // 读取客户端请求数据成功
                {
                    // 一次性把所有数据读完
                    pool->append(users + sockfd);

                    // 如果某个客户端上有数据可读，则我们要调整该连接对应的定时器，以延迟该连接被关闭的时间。
                    if( timer ) {
                        time_t cur = time( NULL );
                        timer->expire = cur + 3 * TIMESLOT;
                        printf( "adjust timer once\n" );
                        timer_lst.adjust_timer( timer );
                    }
                }
                else{  // 读取失败
                    if( timer )  // 删除定时器
                    {
                        timer_lst.del_timer( timer );
                    }
                    users[sockfd].close_conn(); // 关闭连接
                    
                }
            }
            else if(events[i].events & EPOLLOUT)
            {
                util_timer* timer = users[sockfd].timer;
                // 一次性写完所有数据
                if(!users[sockfd].write())
                {
                    if( timer )
                    {
                        timer_lst.del_timer( timer );
                    }
                    users[sockfd].close_conn();
                }
            }
        }
        // 最后处理定时事件，因为I/O事件有更高的优先级。当然，这样做将导致定时任务不能精准的按照预定的时间执行。
        if( timeout ) {
            timer_handler();
            timeout = false;
        }
    }

    close(epollfd);
    close(listenfd);
    close( pipefd[1] );
    close( pipefd[0] );
    delete [] users;
    delete pool;

    return 0;
}