#include <fstream>

#include "file.hpp"
#include "log.hpp"
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
    TLogger::Log("unlink " + Path);

    int ret = RetryBusy(10, 100, [&]{ return unlink(Path.c_str()); });

    if (ret && (errno != ENOENT))
        return TError(EError::Unknown, errno, "unlink(" + Path + ")");

    return TError::Success();
}

TError TFile::AsString(string &value) const {
    ifstream in(Path);
    if (!in.is_open())
        return TError(EError::Unknown, string(__func__) + ": Cannot open " + Path);

    try {
        in.seekg(0, std::ios::end);
        value.reserve(in.tellg());
        in.seekg(0, std::ios::beg);
    } catch (...) {
        // for /proc files we can't reserve space because there is no
        // seek() support
    }

    value.assign((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());

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
    ifstream in(Path);
    string line;

    if (!in.is_open())
        return TError(EError::Unknown, string(__func__) + ": Cannot open " + Path);

    while (getline(in, line))
        value.push_back(line);

    return TError::Success();
}

TError TFile::LastStrings(const size_t size, std::string &value) const {
    ifstream f(Path);
    if (!f.is_open())
        return TError(EError::Unknown, string(__func__) + ": Cannot open " + Path);

    try {
        size_t end = f.seekg(0, ios_base::end).tellg();
        size_t copy = end < size ? end : size;

        f.seekg(-copy, ios_base::end);

        vector<char> s;
        s.resize(copy);
        f.read(s.data(), copy);

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
    } catch (...) {
        return TError(EError::Unknown, string(__func__) + ": Uncaught exception");
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

TError TFile::WriteStringNoAppend(const string &str) const {
    TError error = TError::Success();

    int fd = open(Path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, Mode);
    if (!fd)
        return TError(EError::Unknown, errno, "open(" + Path + ")");

    ssize_t ret = write(fd, str.c_str(), str.length());
    if (ret != (ssize_t)str.length())
        error = TError(EError::Unknown, errno, "write(" + str + ")");

    close(fd);

    return error;
}

TError TFile::AppendString(const string &str) const {
    ofstream out(Path, ofstream::app);
    if (out.is_open()) {
        out << str;
        return TError::Success();
    } else {
        return TError(EError::Unknown, errno, "append(" + Path + ", " + str + ")");
    }
}

bool TFile::Exists() const {
    return access(Path.c_str(), F_OK) == 0;
}
