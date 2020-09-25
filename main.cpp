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
#include <fcntl.h>

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

    bool fetch_link(int, int*, LINK**);
    int on_connected(int);
    void on_data_in(int);
    void on_data_out(int);
    void on_break(int);

    static int do_tcp_listen(sockaddr_in*);
    static int do_tcp_accept(int);
    static int do_tcp_connect(sockaddr_in*);
    static int do_tcp_recv(int, LINK*);
    static int do_tcp_send(int, LINK*);
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

// 查找连接
bool StreamSocketProxy::fetch_link(int fd, int *fd_, LINK **link) {
    std::map<int, LINK*>::iterator it = links.find(fd);

    if ( it != links.end() ) {
        *link = it->second;
        if ( fd == it->second->src_fd ) {
            *fd_ = it->second->target_fd;
        }
        else {
            *fd_ = it->second->src_fd;
        }

        return true;
    }

    return false;
}

// 当有新链接进来
int StreamSocketProxy::on_connected(int fd) {
    LINK *link = new LINK();
    link->offset = 0;
    link->length = 0;
    link->src_fd = do_tcp_accept(fd);
    link->target_fd = do_tcp_connect(&tar_addr);

    int flags;
    // 设置非阻塞
    flags = fcntl(link->src_fd, F_GETFL, 0);
    fcntl(link->src_fd, F_SETFL, flags | O_NONBLOCK);
    flags = fcntl(link->target_fd, F_GETFL, 0);
    fcntl(link->target_fd, F_SETFL, flags | O_NONBLOCK);

    links.insert(std::make_pair(link->src_fd, link));
    links.insert(std::make_pair(link->target_fd, link));

    // epoll 事件注册
    epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = link->src_fd;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, link->src_fd, &ev);
    ev.events = EPOLLIN;
    ev.data.fd = link->target_fd;
    epoll_ctl(epollfd, EPOLL_CTL_ADD, link->target_fd, &ev);

    std::stringstream sstr;
    sstr << "src: " << src_ << ":" << s_port_ << " connected success, fd " << link->src_fd;
    sstr << ", target: " << target_ << ":" << t_port_ << " connect success, fd: " << link->target_fd;
    log("debug", sstr.str());

    return 1;
}

// 接受新连接
int StreamSocketProxy::do_tcp_accept(int fd) {
    sockaddr_in addr;
    int len = sizeof(addr);
    int fd_ = accept(fd, (sockaddr*) &addr, (socklen_t *) &len);

    if ( DEBUG ) {
        std::stringstream sstr;
        sstr << "accept from fd: " << fd << ", accept fd:" << fd_;
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

// epoll有事件可读
void StreamSocketProxy::on_data_in(int recv_fd) {
    LINK *link;
    int send_fd;
    std::stringstream sstr;
    if ( !fetch_link(recv_fd, &send_fd, &link) ) {
        sstr << "link not found";
        log("error", sstr.str());
        return;
    }

    if ( link->length != 0 ) {
        // 等待buff 清空
        return ;
    }

    int ret = do_tcp_recv(recv_fd, link);
    if ( ret == 0 ) {
        return ;
    }
    else if ( ret < 0 ) {
        // 连接可能断开了
        sstr << "link already disconnect";
        log("info", sstr.str());
        on_break(recv_fd);
    }

    // 连接正常， 发送数据
    ret = do_tcp_send(send_fd, link);
    if ( ret == 0 ) {
        // 发送未完成
        epoll_event ev;
        ev.data.fd = send_fd;
        ev.events = EPOLLIN | EPOLLOUT;
        epoll_ctl(epollfd, EPOLL_CTL_MOD, send_fd, &ev);
    }
    else {
        if ( ret < 0 ) {
            // FIXME: error
        }
    }

    return ;
}

// 当有事件可写
void StreamSocketProxy::on_data_out(int send_fd) {
    LINK *link;
    int recv_fd;
    if ( !fetch_link(send_fd, &recv_fd, &link) ) {
        return ;
    }

    // 转发数据
    int ret = do_tcp_send(recv_fd, link);
    if ( ret == 0 ){
        return ;
    }
    else {
        epoll_event ev;
        ev.data.fd = send_fd;
        ev.events = EPOLLIN;
        epoll_ctl(epollfd, EPOLL_CTL_MOD, send_fd, &ev);
    }

    return ;
}

// 关闭连接
void StreamSocketProxy::on_break(int fd) {
    LINK *link;
    int fd_;

    if ( !fetch_link(fd, &fd_, &link) ) {
        return ;
    }

    // close socket
    close(fd);
    close(fd_);

    links.erase(fd);
    links.erase(fd_);
    delete link;

    // remove epoll ctl
    epoll_event ev;
    std::stringstream sstr;
    ev.data.fd = fd;
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, &ev);
    ev.data.fd = fd_;
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd_, &ev);
    sstr << "close fd:" << fd << ", " << fd_;
    log("info", sstr.str());

    return ;
}

// 读取数据, 1 成功 ,0 未完成, -1 失败
int StreamSocketProxy::do_tcp_recv(int fd, LINK *link) {
    link->length = 0;
    link->offset = 0;

    int ret = recv(fd, link->buff, PACKET_BUFF_SIZE, 0);
    if ( ret < 0 ) {
        if ( errno == EAGAIN ) {
            return 0;
        }
        else {
            return -1;
        }
    }
    else if ( ret == 0 ) {
        return -1;
    }

    link->length = ret;

    return 1;
}

// 发送数据 1 发送成功， 0 发送未完成 -1发送失败
int StreamSocketProxy::do_tcp_send(int fd, LINK *link) {
    int ret;
    std::stringstream sstr;
    sstr << "send content to fd: " << fd << ", buff: " << link->buff;
    log("debug", sstr.str());
    while ( link->length > 0 ) {
        ret = send(fd, link->buff + link->offset, link->length, 0);
        if ( ret < 0 ) {
            if ( errno == EAGAIN ) {
                return 0;
            }
            else {
                return -1;
            }
        }
        else {
            link->length -= ret;
            link->offset += ret;
        }
    }

    return 1;
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
                // 有事件可读
                if ( events[i].events & EPOLLIN ) {
                    std::cout << "epoll data in, events[i].events & EPOLLIN: " << (events[i].events & EPOLLIN) << std::endl;
                    on_data_in(events[i].data.fd);
                }

                // 有事件可写
                if ( events[i].events & EPOLLOUT ) {
                    std::cout << "epoll data out, events[i].events & EPOLLOUT: " << (events[i].events & EPOLLOUT) << std::endl;
                    on_data_out(events[i].data.fd);
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
    pm.add("127.0.0.1", 6666, "127.0.0.1", 22);
    pm.add("127.0.0.1", 7777, "127.0.0.1", 22);
    pm.run();
    return 0;
}
