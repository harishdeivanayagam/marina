// marina runtime — std module: fs (filesystem). Sync + async variants
// with identical panic message formats; recursive mkdir/remove; sorted
// readDir.
#include "runtime_internal.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace marina_rt {

namespace {


// ---------------------------------------------------------------------------
// File reading / writing (std::fs::read / std::fs::write)
// ---------------------------------------------------------------------------

std::string read_file_or_panic(const std::string& p, const char* what) {
    int fd = ::open(p.c_str(), O_RDONLY);
    if (fd < 0) {
        rt_panic(std::string(what) + " '" + p + "': " + os_error(errno));
    }
    std::string data;
    char buf[65536];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            ::close(fd);
            rt_panic(std::string(what) + " '" + p + "': " + os_error(e));
        }
        data.append(buf, (size_t)n);
    }
    ::close(fd);
    return data;
}

void write_file_or_panic(const std::string& p, const std::string& data,
                         const char* what) {
    int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        rt_panic(std::string(what) + " '" + p + "': " + os_error(errno));
    }
    size_t off = 0;
    while (off < data.size()) {
        ssize_t n = ::write(fd, data.data() + off, data.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            ::close(fd);
            rt_panic(std::string(what) + " '" + p + "': " + os_error(e));
        }
        off += (size_t)n;
    }
    ::close(fd);
}

// ---------------------------------------------------------------------------
// readDir (sorted names)
// ---------------------------------------------------------------------------

std::vector<std::string> read_dir_or_panic(const std::string& p, const char* what) {
    DIR* d = ::opendir(p.c_str());
    if (d == nullptr) {
        rt_panic(std::string(what) + " '" + p + "': " + os_error(errno));
    }
    std::vector<std::string> names;
    for (;;) {
        errno = 0;
        struct dirent* e = ::readdir(d);
        if (e == nullptr) {
            if (errno != 0) {
                int err = errno;
                ::closedir(d);
                rt_panic(std::string(what) + " '" + p + "': " + os_error(err));
            }
            break;
        }
        std::string name(e->d_name);
        // Rust's fs::read_dir does not yield "." or ".."
        if (name == "." || name == "..") continue;
        names.push_back(utf8_lossy(reinterpret_cast<const uint8_t*>(name.data()),
                                   name.size()));
    }
    ::closedir(d);
    // Rust: names.sort() — bytewise string sort.
    std::sort(names.begin(), names.end());
    return names;
}

void* read_dir_array(const std::string& p, const char* what) {
    std::vector<std::string> names = read_dir_or_panic(p, what);
    void* arr = marina_array_new(TAG_ARRAY);
    for (const std::string& n : names) {
        MValue mv = mv_str_owned(n);
        marina_array_push(arr, &mv);
        drop_mv(mv);
    }
    return arr;
}

// ---------------------------------------------------------------------------
// mkdir (recursive, like std::fs::create_dir_all)
// ---------------------------------------------------------------------------

void mkdir_all(const std::string& path, const char* what, const std::string& orig) {
    if (::mkdir(path.c_str(), 0777) == 0) return;
    int e = errno;
    if (e == EEXIST) {
        struct stat st;
        if (::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return;
        rt_panic(std::string(what) + " '" + orig + "': " + os_error(e));
    }
    if (e == ENOENT) {
        // create the parent directory first, then retry
        size_t pos = path.find_last_of('/');
        if (pos != std::string::npos && pos > 0) {
            mkdir_all(path.substr(0, pos), what, orig);
            if (::mkdir(path.c_str(), 0777) == 0) return;
            int e2 = errno;
            if (e2 == EEXIST) {
                struct stat st;
                if (::stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return;
            }
            rt_panic(std::string(what) + " '" + orig + "': " + os_error(e2));
        }
    }
    rt_panic(std::string(what) + " '" + orig + "': " + os_error(e));
}

// ---------------------------------------------------------------------------
// remove (recursive for dirs, like std::fs::remove_dir_all)
// ---------------------------------------------------------------------------

void remove_dir_all_or_panic(const std::string& path) {
    DIR* d = ::opendir(path.c_str());
    if (d == nullptr) {
        rt_panic("fs.remove '" + path + "': " + os_error(errno));
    }
    for (;;) {
        errno = 0;
        struct dirent* e = ::readdir(d);
        if (e == nullptr) {
            if (errno != 0) {
                int err = errno;
                ::closedir(d);
                rt_panic("fs.remove '" + path + "': " + os_error(err));
            }
            break;
        }
        std::string name(e->d_name);
        if (name == "." || name == "..") continue;
        std::string child = path + "/" + name;
        struct stat st;
        if (::lstat(child.c_str(), &st) != 0) {
            int err = errno;
            ::closedir(d);
            rt_panic("fs.remove '" + path + "': " + os_error(err));
        }
        if (S_ISDIR(st.st_mode)) {
            remove_dir_all_or_panic(child);
        } else if (::unlink(child.c_str()) != 0) {
            int err = errno;
            ::closedir(d);
            rt_panic("fs.remove '" + path + "': " + os_error(err));
        }
    }
    ::closedir(d);
    if (::rmdir(path.c_str()) != 0) {
        rt_panic("fs.remove '" + path + "': " + os_error(errno));
    }
}

void fs_remove_or_panic(const std::string& p) {
    struct stat st;
    if (::lstat(p.c_str(), &st) != 0) {
        rt_panic("fs.remove '" + p + "': " + os_error(errno));
    }
    // Rust: if path.is_dir() -> remove_dir_all else remove_file. Note
    // is_dir() follows symlinks; a symlink-to-dir is removed via unlink
    // by Rust's remove_file after is_dir() is true... Rust's is_dir()
    // uses metadata (follows symlinks), so a symlink pointing at a dir
    // goes through remove_dir_all on the target path. Match that.
    if (::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
        remove_dir_all_or_panic(p);
    } else if (::unlink(p.c_str()) != 0) {
        rt_panic("fs.remove '" + p + "': " + os_error(errno));
    }
}

} // namespace

} // namespace marina_rt

// ---------------------------------------------------------------------------
// C ABI (sync)
// ---------------------------------------------------------------------------

extern "C" void marina_fs_read_file(const MValue* path, MValue* out) {
    std::string p = marina_rt::mv_to_string(*path);
    std::string data = marina_rt::read_file_or_panic(p, "fs.readFile");
    marina_rt::out_mv(out, marina_rt::mv_str_owned(data));
}

extern "C" void marina_fs_write_file(const MValue* path, const MValue* data) {
    std::string p = marina_rt::mv_to_string(*path);
    std::string d = marina_rt::mv_to_string(*data);
    marina_rt::write_file_or_panic(p, d, "fs.writeFile");
}

extern "C" void marina_fs_exists(const MValue* path, MValue* out) {
    std::string p = marina_rt::mv_to_string(*path);
    struct stat st;
    bool exists = ::stat(p.c_str(), &st) == 0;
    marina_rt::out_mv(out, marina_rt::mv_bool(exists));
}

extern "C" void marina_fs_read_dir(const MValue* path, MValue* out) {
    std::string p = marina_rt::mv_to_string(*path);
    void* arr = marina_rt::read_dir_array(p, "fs.readDir");
    marina_rt::out_mv(out, MValue{TAG_ARRAY, 0, 0, 0.0, arr});
}

extern "C" void marina_fs_mkdir(const MValue* path) {
    std::string p = marina_rt::mv_to_string(*path);
    marina_rt::mkdir_all(p, "fs.mkdir", p);
}

extern "C" void marina_fs_remove(const MValue* path) {
    std::string p = marina_rt::mv_to_string(*path);
    marina_rt::fs_remove_or_panic(p);
}

// ---------------------------------------------------------------------------
// C ABI (async — each resolves to the same value as its sync sibling)
// ---------------------------------------------------------------------------

extern "C" void* marina_fs_read_file_async(const MValue* path) {
    std::string p = marina_rt::mv_to_string(*path);
    return marina_rt::spawn_promise_io(
        [p] { return marina_rt::mv_str_owned(marina_rt::read_file_or_panic(p, "fs.readFile")); },
        {});
}

extern "C" void* marina_fs_write_file_async(const MValue* path, const MValue* data) {
    std::string p = marina_rt::mv_to_string(*path);
    std::string d = marina_rt::mv_to_string(*data);
    return marina_rt::spawn_promise_io(
        [p, d] {
            marina_rt::write_file_or_panic(p, d, "fs.writeFile");
            return MValue{TAG_VOID, 0, 0, 0.0, nullptr};
        },
        {});
}

extern "C" void* marina_fs_exists_async(const MValue* path) {
    std::string p = marina_rt::mv_to_string(*path);
    return marina_rt::spawn_promise_io(
        [p] {
            struct stat st;
            bool exists = ::stat(p.c_str(), &st) == 0;
            return marina_rt::mv_bool(exists);
        },
        {});
}

extern "C" void* marina_fs_read_dir_async(const MValue* path) {
    std::string p = marina_rt::mv_to_string(*path);
    return marina_rt::spawn_promise_io(
        [p] { return MValue{TAG_ARRAY, 0, 0, 0.0, marina_rt::read_dir_array(p, "fs.readDir")}; },
        {});
}

extern "C" void* marina_fs_mkdir_async(const MValue* path) {
    std::string p = marina_rt::mv_to_string(*path);
    return marina_rt::spawn_promise_io(
        [p] {
            marina_rt::mkdir_all(p, "fs.mkdir", p);
            return MValue{TAG_VOID, 0, 0, 0.0, nullptr};
        },
        {});
}

extern "C" void* marina_fs_remove_async(const MValue* path) {
    std::string p = marina_rt::mv_to_string(*path);
    return marina_rt::spawn_promise_io(
        [p] {
            marina_rt::fs_remove_or_panic(p);
            return MValue{TAG_VOID, 0, 0, 0.0, nullptr};
        },
        {});
}