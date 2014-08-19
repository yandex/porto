#include <fstream>

#include "file.hpp"
#include "log.hpp"
#include "util/string.hpp"

extern "C" {
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <linux/limits.h>
}

using namespace std;

string TFile::Path() {
    return path;
}

TFile::EFileType TFile::Type() {
    struct stat st;

    if (lstat(path.c_str(), &st))
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

TError TFile::Remove() {
    TLogger::Log("rm " + path);

    int ret = unlink(path.c_str());

    if (ret && (errno != ENOENT))
        return TError(EError::Unknown, errno, "unlink(" + path + ")");

    return TError::Success();
}

TError TFile::AsString(string &value) {
    ifstream in(Path());
    if (!in.is_open())
        return TError(EError::Unknown, string(__func__) + ": Cannot open " + Path());

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

TError TFile::AsInt(int &value) {
    string s;
    auto ret = AsString(s);
    if (ret)
        return ret;

    return StringToInt(s, value);
}

TError TFile::AsLines(vector<string> &value) {
    ifstream in(path);
    string line;

    if (!in.is_open())
        return TError(EError::Unknown, string(__func__) + ": Cannot open " + path);

    while (getline(in, line))
        value.push_back(line);

    return TError::Success();
}

TError TFile::ReadLink(std::string &value) {
    char buf[PATH_MAX];
    ssize_t len;

    len = readlink(path.c_str(), buf, sizeof(buf) - 1);
    if (len < 0)
        return TError(EError::Unknown, errno, "readlink(" + path + ")");

    buf[len] = '\0';

    value.assign(buf);
    return TError::Success();
}

TError TFile::WriteStringNoAppend(const string &str) {
    TError error = TError::Success();

    int fd = open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, mode);
    if (!fd)
        return TError(EError::Unknown, errno, "open(" + path + ")");

    ssize_t ret = write(fd, str.c_str(), str.length());
    if (ret != (ssize_t)str.length())
        error = TError(EError::Unknown, errno, "write(" + str + ")");

    close(fd);

    return error;
}

TError TFile::AppendString(const string &str) {
    ofstream out(path, ofstream::app);
    if (out.is_open()) {
        out << str;
        return TError::Success();
    } else {
        return TError(EError::Unknown, errno, "append(" + path + ", " + str + ")");
    }
}
