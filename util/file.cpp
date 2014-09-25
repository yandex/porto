#include "file.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"

extern "C" {
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <linux/limits.h>
}

using namespace std;

const string &TFile::GetPath() const {
    return Path;
}

TFile::EFileType TFile::Type() const {
    struct stat st;

    if (lstat(Path.c_str(), &st))
        return Unknown;

    if (S_ISREG(st.st_mode))
        return Regular;
    else if (S_ISDIR(st.st_mode))
        return Directory;
    else if (S_ISCHR(st.st_mode))
        return Character;
    else if (S_ISBLK(st.st_mode))
        return Block;
    else if (S_ISFIFO(st.st_mode))
        return Fifo;
    else if (S_ISLNK(st.st_mode))
        return Link;
    else if (S_ISSOCK(st.st_mode))
        return Socket;
    else
        return Unknown;
}

TError TFile::Remove() const {
    TLogger::Log() << "unlink " << Path << endl;

    int ret = RetryBusy(10, 100, [&]{ return unlink(Path.c_str()); });

    if (ret && (errno != ENOENT))
        return TError(EError::Unknown, errno, "unlink(" + Path + ")");

    return TError::Success();
}

TError TFile::AsString(string &value) const {
    int fd = open(Path.c_str(), O_RDONLY);
    if (fd < 0)
        return TError(EError::Unknown, errno, "open(" + Path + ")");

    int n;
    do {
        char buf[256];
        n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            close(fd);
            return TError(EError::Unknown, errno, "read(" + Path + ")");
        }

        value.append(buf, n);
    } while (n != 0);

    close(fd);

    return TError::Success();
}

TError TFile::AsInt(int &value) const {
    string s;
    auto ret = AsString(s);
    if (ret)
        return ret;

    return StringToInt(s, value);
}

TError TFile::AsLines(vector<string> &value) const {
    FILE *f = fopen(Path.c_str(), "r");
    if (!f)
        return TError(EError::Unknown, errno, "fopen(" + Path + ")");

    char *line = nullptr;
    size_t len;
    ssize_t n;
    while ((n = getline(&line, &len, f)) != -1)
        value.push_back(string(line, n - 1));

    fclose(f);

    return TError::Success();
}

TError TFile::LastStrings(const size_t size, std::string &value) const {
    int fd = open(Path.c_str(), O_RDONLY);
    if (fd < 0)
        return TError(EError::Unknown, errno, "open(" + Path + ")");

    size_t end = lseek(fd, 0, SEEK_END);
    size_t copy = end < size ? end : size;

    lseek(fd, -copy, SEEK_END);

    vector<char> s;
    s.resize(copy);

    int n = read(fd, s.data(), copy);
    close(fd);
    if (n < 0)
        return TError(EError::Unknown, errno, "read(" + Path + ")");

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

TError TFile::ReadLink(std::string &value) const {
    char buf[PATH_MAX];
    ssize_t len;

    len = readlink(Path.c_str(), buf, sizeof(buf) - 1);
    if (len < 0)
        return TError(EError::Unknown, errno, "readlink(" + Path + ")");

    buf[len] = '\0';

    value.assign(buf);
    return TError::Success();
}

TError TFile::Write(int flags, const string &str) const {
    TError error = TError::Success();

    int fd = open(Path.c_str(), flags, Mode);
    if (fd < 0)
        return TError(EError::Unknown, errno, "open(" + Path + ")");

    ssize_t ret = write(fd, str.c_str(), str.length());
    if (ret != (ssize_t)str.length())
        error = TError(EError::Unknown, errno, "write(" + Path + ", " + str + ")");

    close(fd);

    return error;
}

TError TFile::WriteStringNoAppend(const string &str) const {
    return Write(O_CREAT | O_TRUNC | O_WRONLY, str);
}

TError TFile::AppendString(const string &str) const {
    return Write(O_APPEND | O_WRONLY, str);
}

bool TFile::Exists() const {
    return access(Path.c_str(), F_OK) == 0;
}
