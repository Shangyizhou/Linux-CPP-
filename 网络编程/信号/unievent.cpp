#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <pthread.h>

#define MAX_EVENT_NUMBER 1024
static int pipefd[2]; // 管道

// 设置非阻塞IO
int setnonblocking( int fd )
{
    int old_option = fcntl( fd, F_GETFL );
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option );
    return old_option;
}

// 向epoll例程注册监视对象和事件
void addfd( int epollfd, int fd )
{
    epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET;
    epoll_ctl( epollfd, EPOLL_CTL_ADD, fd, &event );
    setnonblocking( fd );
}

// 信号处理函数
void sig_handler( int sig )
{
    // 保留原来的errno，在函数最后回复，以保证函数的可重入性（errno会覆盖）
    int save_errno = errno;
    int msg = sig;
    send( pipefd[1], ( char* )&msg, 1, 0 );
    errno = save_errno;
}

// 设置信号的处理函数
void addsig( int sig )
{
    struct sigaction sa;
    memset( &sa, '\0', sizeof( sa ) );
    sa.sa_handler = sig_handler;
    sa.sa_flags |= SA_RESTART;
    sigfillset( &sa.sa_mask );
    assert( sigaction( sig, &sa, NULL ) != -1 );
}

int main( int argc, char* argv[] )
{
    if( argc <= 2 )
    {
        printf( "usage: %s ip_address port_number\n", basename( argv[0] ) );
        return 1;
    }
    const char* ip = argv[1];
    int port = atoi( argv[2] );

    int ret = 0;
    struct sockaddr_in address;
    bzero( &address, sizeof( address ) );
    address.sin_family = AF_INET;
    inet_pton( AF_INET, ip, &address.sin_addr );
    address.sin_port = htons( port );

    int listenfd = socket( PF_INET, SOCK_STREAM, 0 );
    assert( listenfd >= 0 );

    //int nReuseAddr = 1;
    //setsockopt( listenfd, SOL_SOCKET, SO_REUSEADDR, &nReuseAddr, sizeof( nReuseAddr ) );
    ret = bind( listenfd, ( struct sockaddr* )&address, sizeof( address ) );
    if( ret == -1 )
    {
        printf( "errno is %d\n", errno );
        return 1;
    }
    //assert( ret != -1 );

    ret = listen( listenfd, 5 );
    assert( ret != -1 );

    // epoll
    epoll_event events[ MAX_EVENT_NUMBER ];
    int epollfd = epoll_create( 5 );
    assert( epollfd != -1 );
    addfd( epollfd, listenfd );

    // 使用socketpair创建管道，注册pipefd[0]上的可读事件
    ret = socketpair( PF_UNIX, SOCK_STREAM, 0, pipefd );
    assert( ret != -1 );
    setnonblocking( pipefd[1] );
    // 向epoll注册pipefd[0]的可读事件
    addfd( epollfd, pipefd[0] );

    // 设置一些信号的处理函数
    addsig( SIGHUP );
    addsig( SIGCHLD );
    addsig( SIGTERM );
    addsig( SIGINT );
    bool stop_server = false;

    int connfd;
    while( !stop_server )
    {
        int number = epoll_wait( epollfd, events, MAX_EVENT_NUMBER, -1 );
        if ( ( number < 0 ) && ( errno != EINTR ) )
        {
            printf( "epoll failure\n" );
            break;
        }
    
        for ( int i = 0; i < number; i++ )
        {
            int sockfd = events[i].data.fd;
            // 如果就绪的文件描述符是lietenfd，则处理信号
            if( sockfd == listenfd )
            {
                struct sockaddr_in client_address;
                socklen_t client_addrlength = sizeof( client_address );
                connfd = accept( listenfd, ( struct sockaddr* )&client_address, &client_addrlength );
                addfd( epollfd, connfd );
                printf("clent %d is connected\n", connfd);
            }
            // 从管道那接收信号值
            else if( ( sockfd == pipefd[0] ) && ( events[i].events & EPOLLIN ) )
            {
                int sig;
                char signals[1024];
                // 每个信号值占1字节，所以按字节逐个接收信号
                ret = recv( pipefd[0], signals, sizeof( signals ), 0 );
                if( ret == -1 )
                {
                    continue;
                }
                else if( ret == 0 )
                {
                    continue;
                }
                else
                {
                    for( int i = 0; i < ret; ++i )
                    {
                        // 在主循环里处理函数而不是使用函数
                        printf( "I caugh the signal %d\n", signals[i] );
                        switch( signals[i] )
                        {
                            case SIGCHLD:
                            case SIGHUP:
                            {
                                continue;
                            }
                            case SIGTERM:
                            case SIGINT:
                            {
                                stop_server = true;
                            }
                        }
                    }
                }
            }
            else
            {

            }
        }
    }

    printf( "close fds\n" );
    close( listenfd );
    close( pipefd[1] );
    close( pipefd[0] );
    return 0;
}
