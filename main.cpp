// std
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <map>
#include <ctime>
#include <sstream>

// system
#include <signal.h>
#include <sys/epoll.h>
#include <unistd.h>

// net
#include <arpa/inet.h>
#include <sys/socket.h>

#define PACKET_BUFF_SIZE 8192
#define EPOLL_BUFF_SIZE 1024
#define DEBUG true

typedef struct {
    int src_fd;
    int target_fd;
    int length;
    int offset;
    char buff[PACKET_BUFF_SIZE];
} LINK;

void log(const std::string &level, const std::string &content) {
    time_t now = time(0);
    tm *tm_ = localtime(&now);
    std::cout << tm_->tm_year + 1900 << "-";
    std::cout << tm_->tm_mon + 1 << "-";
    std::cout << tm_->tm_mday << " ";
    std::cout << tm_->tm_hour << ":";
    std::cout << tm_->tm_min << ":";
    std::cout << tm_->tm_sec << " [" << level << "]:";
    std::cout << content << std::endl;
}

class ProxyManager;

class StreamSocketProxy {
public:
    StreamSocketProxy() = default;
    ~StreamSocketProxy() = default;


    void serve();
    int getFd();
    bool startup(const std::string &src, const unsigned int s_port, const std::string &target, const unsigned int t_port);
private:
    friend ProxyManager;

    int sockfd;
    int epollfd;
    int pipefd[2];
    unsigned int s_port_, t_port_;
    sockaddr_in src_addr;
    sockaddr_in tar_addr;

    std::thread thread_;
    std::string src_, target_;
    std::map<int, LINK*> links;

    int on_connected(int);
    static int do_tcp_listen(sockaddr_in*);
    static int do_tcp_accept(int);
    static int do_tcp_connect(sockaddr_in*);
};

bool StreamSocketProxy::startup(const std::string &src, const unsigned int s_port, const std::string &target, const unsigned int t_port) {
    src_ = std::move(src);
    target_ = std::move(target);
    s_port_ = s_port;
    t_port_ = t_port;

    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(s_port);
    if ( src.empty() ) {
        src_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    else {
        if ( inet_pton(AF_INET, src.c_str(), &src_addr.sin_addr.s_addr) <= 0) { return false; }
    }

    tar_addr.sin_family = AF_INET;
    tar_addr.sin_port = htons(t_port);
    if ( target.empty() ) {
        tar_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    else {
        if ( inet_pton(AF_INET, target.c_str(), &tar_addr.sin_addr.s_addr) <= 0) { return false; }
    }

    // listen
    sockfd = do_tcp_listen(&src_addr);
    if ( sockfd < 0 ) { return false; }

    // pipe
    if ( pipe(pipefd) < 0 ) { return false; }

    // epoll
    epollfd = epoll_create1(0);

    // epoll 关联pipe
    epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = sockfd;
    // 注册epoll事件
    epoll_ctl(epollfd, EPOLL_CTL_ADD, sockfd, &ev);

    epoll_ctl(epollfd, EPOLL_CTL_ADD, pipefd[0], &ev);
    printf("sockfd: %d, epollfd: %d\n", sockfd, epollfd);
    printf("pipe[0]: %d, pipe[1]: %d, %s:%d\n", pipefd[0], pipefd[1], __FILE__, __LINE__);
    return true;
}

int StreamSocketProxy::getFd() {
    return sockfd;
}

// 当有新链接进来
int StreamSocketProxy::on_connected(int fd) {
    LINK *link = new LINK();
    link->offset = 0;
    link->length = 0;
    link->src_fd = do_tcp_accept(fd);
    link->target_fd = do_tcp_connect(&tar_addr);

    if ( DEBUG ) {
        if ( link->target_fd > 0 ) {
            std::stringstream sstr;
            sstr << "target: " << target_ << ":" << t_port_ << " connect success, fd: " << link->target_fd;
            log("debug", sstr.str());
        }
    }

}

// 接受新连接
int StreamSocketProxy::do_tcp_accept(int fd) {
    sockaddr_in addr;
    int len = sizeof(addr);
    int fd_ = accept(fd, (sockaddr*) &addr, (socklen_t *) &len);

    if ( DEBUG ) {
        std::stringstream sstr;
        sstr << "accept from fd: " << fd << "accept fd:" << fd_;
        log("debug", sstr.str());
    }

    return fd_;
}

// 监听端口
int StreamSocketProxy::do_tcp_listen(sockaddr_in *src) {
    const int flag = 1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    // SO_REUSEADDR 端口释放后立即可用
    if ( setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0 ) { return -1; }
    // 绑定端口
    if ( bind(fd, (const  sockaddr*) src, sizeof(sockaddr_in) ) < 0 ) { return -1; }
    // 监听端口
    if ( listen(fd, SOMAXCONN) < 0 ) { return -1; }

    return fd;
}

// 创建tcp连接
int StreamSocketProxy::do_tcp_connect(sockaddr_in *target) {
    const int flag = 1;
    int cfd = socket(AF_INET, SOCK_STREAM, 0);

    // SO_REUSEADDR 端口释放后立即可用
    if ( setsockopt(cfd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) < 0 ) { return -1; }
    if ( connect(cfd, (const struct sockaddr*) target, (socklen_t ) (sizeof(sockaddr))) < 0 ) { return -1; }
    return cfd;
}

void StreamSocketProxy::serve() {
    int i, count;
    epoll_event events[EPOLL_BUFF_SIZE];

    while ( true ) {
        // epoll_wait 当没有事件发生时会阻塞在此
        count = epoll_wait(epollfd, events, EPOLL_BUFF_SIZE, -1);

        if ( count < 0 ) {
            // 中断信号
            if ( errno == EINTR ) {
                continue;
            }
            else {
                printf("epoll error\n");
                return ;
            }
        }

        // 通常遍历events[0]~events[返回值-1]的元素。这些都是就绪的。并且保存了就绪的事件。
        for ( i = 0; i < count; ++i ) {
            if ( events[i].data.fd == pipefd[0] ) {
                return;
            }
            // 接受新的连接
            else if ( events[i].data.fd == sockfd ) {
                printf("accept new connection, fd: %d\n", sockfd);
                on_connected(sockfd);
            }
            else {
                if ( events[i].events & EPOLLIN ) {
                    std::cout << "epoll data in, events[i].events & EPOLLIN: " << (events[i].events & EPOLLIN) << std::endl;
                }

                if ( events[i].events & EPOLLOUT ) {
                    std::cout << "epoll data out, events[i].events & EPOLLOUT: " << (events[i].events & EPOLLOUT) << std::endl;
                }
            }
        }
    }
}



class ProxyManager {
public:
    StreamSocketProxy* add(const std::string &src, const unsigned int s_port, const std::string &target, const unsigned int t_port);
    void destroy();
    void run();
    static void proc(StreamSocketProxy*);
private:
    std::vector<StreamSocketProxy*> proxis;
};

void ProxyManager::proc(StreamSocketProxy *sp) {
    std::cout << "running on " << sp->src_ << ":"<< sp->s_port_ << std::endl;
    sp->serve();
}

StreamSocketProxy* ProxyManager::add(const std::string &src, const unsigned int s_port, const std::string &target, const unsigned int t_port) {
    StreamSocketProxy* sp = new StreamSocketProxy();
    sp->startup(src, s_port, target, t_port);
    sp->thread_ = std::thread(&proc, sp);
    proxis.push_back(sp);

    return sp;
}

void ProxyManager::run() {
    for ( auto &it : proxis ) {
//        close(it->pipefd[1]);
        it->thread_.join();
    }
}

int main() {
    ProxyManager pm;
    pm.add("127.0.0.1", 7777, "127.0.0.1", 22);
    pm.run();
    return 0;
}
