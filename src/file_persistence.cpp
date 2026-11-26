#include "file_persistence.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

/// Write all of data to fd, retrying on EINTR and short writes.
bool write_all(int fd, const uint8_t* data, size_t len) {
    while (len > 0) {
        ssize_t n = ::write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

FilePersistence::FilePersistence(std::string dir) : dir_(std::move(dir)) {}

bool FilePersistence::init() {
    std::error_code ec;
    if (std::filesystem::create_directories(dir_, ec)) {
        // Newly created by us: restrict it, since it will hold private keys.
        ::chmod(dir_.c_str(), 0700);
    } else if (ec) {
        fprintf(stderr, "Cannot create state directory %s: %s\n", dir_.c_str(),
                ec.message().c_str());
        return false;
    }
    if (!std::filesystem::is_directory(dir_, ec)) {
        fprintf(stderr, "State directory %s is not a directory\n", dir_.c_str());
        return false;
    }
    return true;
}

std::string FilePersistence::path_for(std::string_view key) const {
    // The library's keys are fixed names such as "keypair" or "rec_3".  Refuse
    // anything else so a key can never escape the state directory.
    if (key.empty() || key.size() > 32) return {};
    for (char c : key) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '_';
        if (!ok) return {};
    }
    std::string path = dir_;
    path += '/';
    path += key;
    return path;
}

std::optional<size_t> FilePersistence::read_blob(std::string_view key, std::span<uint8_t> out) {
    const std::string path = path_for(key);
    if (path.empty()) return std::nullopt;

    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno != ENOENT) {
            fprintf(stderr, "FilePersistence: cannot read %s: %s\n", path.c_str(),
                    strerror(errno));
        }
        return std::nullopt;
    }

    struct stat st {};
    if (::fstat(fd, &st) != 0 || st.st_size < 0 ||
        static_cast<size_t>(st.st_size) > out.size()) {
        ::close(fd);
        return std::nullopt;
    }

    const size_t want = static_cast<size_t>(st.st_size);
    size_t got = 0;
    while (got < want) {
        ssize_t n = ::read(fd, out.data() + got, want - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += static_cast<size_t>(n);
    }
    ::close(fd);
    if (got != want) return std::nullopt;
    return got;
}

bool FilePersistence::write_blob(std::string_view key, std::span<const uint8_t> data) {
    const std::string path = path_for(key);
    if (path.empty()) return false;
    const std::string tmp = path + ".tmp";

    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        fprintf(stderr, "FilePersistence: cannot write %s: %s\n", tmp.c_str(), strerror(errno));
        return false;
    }
    // The mode above only applies to a newly created file; tighten a stale one.
    bool ok = ::fchmod(fd, 0600) == 0 && write_all(fd, data.data(), data.size()) &&
              ::fsync(fd) == 0;
    ok = (::close(fd) == 0) && ok;
    if (!ok || ::rename(tmp.c_str(), path.c_str()) != 0) {
        fprintf(stderr, "FilePersistence: cannot write %s: %s\n", path.c_str(), strerror(errno));
        ::unlink(tmp.c_str());
        return false;
    }

    // Make the rename itself durable across a power loss.
    int dfd = ::open(dir_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        ::fsync(dfd);
        ::close(dfd);
    }
    return true;
}
