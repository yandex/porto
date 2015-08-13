#include "file.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/cred.hpp"

extern "C" {
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
}

using std::string;
using std::vector;

TError TFile::Touch() const {
    L_ACT() << "Touch " << Path << std::endl;

    int fd = open(Path.ToString().c_str(), O_CREAT | O_WRONLY, Mode);
    if (fd < 0)
        return TError(EError::Unknown, errno, "open(" + Path.ToString() + ")");

    if (close(fd) < 0)
        return TError(EError::Unknown, errno, "close(" + Path.ToString() + ")");

    return TError::Success();
}

TError TFile::Remove(bool silent) const {
    if (!silent)
        L_ACT() << "Unlink " << Path << std::endl;

    int ret = RetryBusy(10, 100, [&]{ return unlink(Path.ToString().c_str()); });

    if (ret && (errno != ENOENT))
        return TError(EError::Unknown, errno, "unlink(" + Path.ToString() + ")");

    return TError::Success();
}

TError TFile::AsString(string &value) const {
    int fd = open(Path.ToString().c_str(), O_RDONLY);
    if (fd < 0)
        return TError(EError::Unknown, errno, "open(" + Path.ToString() + ")");

    int n;
    value.clear();
    do {
        char buf[256];
        n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            close(fd);
            return TError(EError::Unknown, errno, "read(" + Path.ToString() + ")");
        }

        value.append(buf, n);
    } while (n != 0);

    if (close(fd) < 0)
        return TError(EError::Unknown, errno, "close(" + Path.ToString() + ")");

    return TError::Success();
}

TError TFile::AsInt(int &value) const {
    string s;
    auto ret = AsString(s);
    if (ret)
        return ret;

    return StringToInt(s, value);
}

TError TFile::AsUint64(uint64_t &value) const {
    string s;
    auto ret = AsString(s);
    if (ret)
        return ret;

    return StringToUint64(s, value);
}

TError TFile::AsLines(vector<string> &value) const {
    FILE *f = fopen(Path.ToString().c_str(), "r");
    if (!f)
        return TError(EError::Unknown, errno, "fopen(" + Path.ToString() + ")");

    char *line = nullptr;
    size_t len = 0;
    ssize_t n;
    while ((n = getline(&line, &len, f)) != -1)
        value.push_back(string(line, n - 1));

    fclose(f);
    free(line);

    return TError::Success();
}

TError TFile::LastStrings(const size_t size, std::string &value) const {
    int fd = open(Path.ToString().c_str(), O_RDONLY);
    if (fd < 0)
        return TError(EError::Unknown, errno, "open(" + Path.ToString() + ")");

    size_t end = lseek(fd, 0, SEEK_END);
    size_t copy = end < size ? end : size;

    lseek(fd, -copy, SEEK_END);

    vector<char> s;
    s.resize(copy);

    int n = read(fd, s.data(), copy);
    if (close(fd) < 0)
        return TError(EError::Unknown, errno, "close(" + Path.ToString() + ")");
    if (n < 0)
        return TError(EError::Unknown, errno, "read(" + Path.ToString() + ")");

    if (end > size) {
        auto iter = s.begin();

        for (; iter != s.end(); iter++)
            if (*iter == '\n') {
                iter++;
                break;
            }

        value.assign(iter, s.end());
    } else {
        value.assign(s.begin(), s.begin() + copy);
    }

    return TError::Success();
}

TError TFile::Write(int flags, const string &str) const {
    TError error = TError::Success();

    int fd = open(Path.ToString().c_str(), flags, Mode);
    if (fd < 0)
        return TError(EError::Unknown, errno, "open(" + Path.ToString() + ")");

    ssize_t ret = write(fd, str.c_str(), str.length());
    if (ret != (ssize_t)str.length())
        error = TError(EError::Unknown, errno, "write(" + Path.ToString() + ", " + str + ")");

    if (close(fd) < 0)
        return TError(EError::Unknown, errno, "close(" + Path.ToString() + ")");

    return error;
}

TError TFile::WriteStringNoAppend(const string &str) const {
    return Write(O_CREAT | O_TRUNC | O_WRONLY, str);
}

TError TFile::AppendString(const string &str) const {
    return Write(O_CREAT | O_APPEND | O_WRONLY, str);
}

TError TFile::Truncate(size_t size) const {
    if (truncate(Path.ToString().c_str(), size) < 0)
        return TError(EError::Unknown, errno, "truncate(" + Path.ToString() + ")");

    return TError::Success();
}

off_t TFile::GetSize() const {
    return Path.GetSize();
}

#ifndef FALLOC_FL_COLLAPSE_RANGE
#define FALLOC_FL_COLLAPSE_RANGE        0x08
#endif

TError TFile::RotateLog(off_t max_disk_usage) const {
    struct stat st;
    off_t hole_len;
    TError error;
    int fd;

    if (lstat(Path.ToString().c_str(), &st))
        return TError(EError::Unknown, errno, "lstat(" + Path.ToString() + ")");

    if (!S_ISREG(st.st_mode) || (off_t)st.st_blocks * 512 <= max_disk_usage)
        return TError::Success();

    fd = open(Path.ToString().c_str(), O_RDWR | O_NOCTTY);
    if (fd < 0)
        return TError(EError::Unknown, errno, "open(" + Path.ToString() + ")");

    /* Keep half of allowed size */
    hole_len = st.st_size - max_disk_usage / 2;
    hole_len -= hole_len % st.st_blksize;

    if (fallocate(fd, FALLOC_FL_COLLAPSE_RANGE, 0, hole_len) &&
            ftruncate(fd, 0))
        error = TError(EError::Unknown, errno, "truncate(" + Path.ToString() + ")");
    else
        error = TError::Success();

    close(fd);
    return error;
}

TError TFile::Chmod(mode_t mode) const {
    if (chmod(Path.ToString().c_str(), mode) == -1)
        return TError(EError::Unknown, errno, "chmod(" + Path.ToString() + ")");
    return TError::Success();
}

TError TFile::Chown(const TCred &cred) const {
    if (chown(Path.ToString().c_str(), cred.Uid, cred.Gid) == -1)
        return TError(EError::Unknown, errno, "chmod(" + Path.ToString() + ")");
    return TError::Success();
}
