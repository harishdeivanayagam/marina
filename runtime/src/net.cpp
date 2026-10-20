// marina runtime — std module: net (TCP streams).
//
// Stream handles live in a runtime table (not ARC-managed): a stream
// value is just an id. `stream.close()` removes it; unclosed streams are
// closed when the process exits.
#include "runtime_internal.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace marina_rt {

namespace {

struct Streams {
    std::mutex mu;
    std::unordered_map<int64_t, int> fds;
    int64_t next_id = 1;
};

Streams& streams() {
    static Streams* s = new Streams(); // leaked intentionally
    return *s;
}

// Look up a stream and return a dup'd descriptor (Rust's try_clone).
int stream_get(int64_t id) {
    std::lock_guard<std::mutex> lk(streams().mu);
    auto it = streams().fds.find(id);
    if (it == streams().fds.end()) {
        rt_panic("invalid stream handle " + std::to_string(id));
    }
    int c = ::dup(it->second);
    if (c < 0) {
        rt_panic("stream " + std::to_string(id) + ": " + os_error(errno));
    }
    return c;
}

// TcpStream::connect((host, port)): resolve and try each address; the
// last error wins. Returns the fd, or -1 with *err_out set.
int tcp_connect(const std::string& host, uint16_t port, int* err_out) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    std::string service = std::to_string(port);
    int rc = ::getaddrinfo(host.c_str(), service.c_str(), &hints, &res);
    if (rc != 0) {
        *err_out = rc == EAI_SYSTEM ? errno : EIO;
        return -1;
    }
    int fd = -1;
    *err_out = 0;
    for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        int c = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (c < 0) {
            *err_out = errno;
            continue;
        }
        if (::connect(c, ai->ai_addr, ai->ai_addrlen) == 0) {
            fd = c;
            break;
        }
        *err_out = errno;
        ::close(c);
    }
    ::freeaddrinfo(res);
    return fd;
}

void set_read_timeout_30s(int fd) {
    struct timeval tv;
    tv.tv_sec = 30;
    tv.tv_usec = 0;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

// Rust `write_all`: retries on Interrupted, propagates other errors.
void write_all_or_panic(int fd, const std::string& bytes, int64_t id) {
    size_t off = 0;
    while (off < bytes.size()) {
        ssize_t n = ::write(fd, bytes.data() + off, bytes.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            ::close(fd);
            rt_panic("stream " + std::to_string(id) + ": write failed (" +
                     os_error(e) + ")");
        }
        off += (size_t)n;
    }
}

// Rust `read_to_end`: retries on Interrupted, propagates other errors,
// stops at EOF.
std::string read_to_end_or_panic(int fd, int64_t id) {
    std::string buf;
    char tmp[8192];
    for (;;) {
        ssize_t n = ::read(fd, tmp, sizeof tmp);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            // SO_RCVTIMEO expiry (and genuinely non-blocking fds) surface as
            // EAGAIN/EWOULDBLOCK. Rust's read_to_end stops on WouldBlock and
            // keeps what it read, so match that instead of panicking.
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            int e = errno;
            ::close(fd);
            rt_panic("stream " + std::to_string(id) + ": read failed (" +
                     os_error(e) + ")");
        }
        buf.append(tmp, (size_t)n);
    }
    return buf;
}

MValue mv_stream(int64_t id) { return MValue{TAG_STREAM, 0, id, 0.0, nullptr}; }

int64_t stream_register(int fd) {
    std::lock_guard<std::mutex> lk(streams().mu);
    int64_t id = streams().next_id;
    streams().next_id += 1;
    streams().fds.emplace(id, fd);
    return id;
}

MValue do_net_connect(const std::string& host_s, int64_t port_n) {
    int err = 0;
    int fd = tcp_connect(host_s, (uint16_t)port_n, &err);
    if (fd < 0) {
        rt_panic("cannot connect to " + host_s + ":" + std::to_string(port_n) +
                 " (" + os_error(err) + ")");
    }
    set_read_timeout_30s(fd);
    int64_t id = stream_register(fd);
    return mv_stream(id);
}

void do_net_write(int64_t id, const std::string& bytes) {
    int fd = stream_get(id);
    write_all_or_panic(fd, bytes, id);
    ::close(fd);
}

std::string do_net_read_all(int64_t id) {
    int fd = stream_get(id);
    std::string buf = read_to_end_or_panic(fd, id);
    ::close(fd);
    return buf;
}

void do_net_close(int64_t id) {
    std::lock_guard<std::mutex> lk(streams().mu);
    auto it = streams().fds.find(id);
    if (it != streams().fds.end()) {
        ::close(it->second);
        streams().fds.erase(it);
    }
}


} // namespace

} // namespace marina_rt

// ---------------------------------------------------------------------------
// C ABI
// ---------------------------------------------------------------------------

extern "C" void marina_net_connect(const MValue* host, const MValue* port, MValue* out) {
    std::string host_s = marina_rt::mv_to_string(*host);
    int64_t port_n = port->i;
    marina_rt::out_mv(out, marina_rt::do_net_connect(host_s, port_n));
}

extern "C" void marina_net_write(const MValue* s, const MValue* data) {
    int64_t id = s->i;
    std::string bytes = marina_rt::mv_to_string(*data);
    marina_rt::do_net_write(id, bytes);
}

extern "C" void marina_net_read_all(const MValue* s, MValue* out) {
    int64_t id = s->i;
    std::string buf = marina_rt::do_net_read_all(id);
    marina_rt::out_mv(out, marina_rt::mv_str_owned(buf));
}

extern "C" void marina_net_close(const MValue* s) {
    int64_t id = s->i;
    marina_rt::do_net_close(id);
}

extern "C" void* marina_net_connect_async(const MValue* host, const MValue* port) {
    std::string host_s = marina_rt::mv_to_string(*host);
    int64_t port_n = port->i;
    return marina_rt::spawn_promise_io(
        [host_s, port_n] { return marina_rt::do_net_connect(host_s, port_n); }, {});
}

extern "C" void* marina_net_write_async(const MValue* s, const MValue* data) {
    int64_t id = s->i;
    std::string bytes = marina_rt::mv_to_string(*data);
    return marina_rt::spawn_promise_io(
        [id, bytes] {
            marina_rt::do_net_write(id, bytes);
            return MValue{TAG_VOID, 0, 0, 0.0, nullptr};
        },
        {});
}

extern "C" void* marina_net_read_all_async(const MValue* s) {
    int64_t id = s->i;
    return marina_rt::spawn_promise_io(
        [id] { return marina_rt::mv_str_owned(marina_rt::do_net_read_all(id)); }, {});
}

extern "C" void* marina_net_close_async(const MValue* s) {
    int64_t id = s->i;
    return marina_rt::spawn_promise_io(
        [id] {
            marina_rt::do_net_close(id);
            return MValue{TAG_VOID, 0, 0, 0.0, nullptr};
        },
        {});
}