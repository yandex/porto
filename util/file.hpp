#ifndef __FILE_HPP__
#define __FILE_HPP__

#include <string>
#include <vector>

#include "error.hpp"

class TFile {
    const std::string Path;
    const int Mode; // currently used only by WriteStringNoAppend

public:
    enum EFileType {
        Regular,
        Directory,
        Block,
        Character,
        Fifo,
        Link,
        Socket,
        Unknown,
        Any
    };

    TFile(const std::string &path, const int mode = 0600) : Path(path), Mode(mode) { };

    std::string GetPath();
    EFileType Type();

    TError Remove();

    TError AsString(std::string &value);
    TError AsInt(int &value);
    TError AsLines(std::vector<std::string> &value);
    TError LastStrings(const size_t size, std::string &value);
    TError ReadLink(std::string &value);

    TError WriteStringNoAppend(const std::string &str);
    TError AppendString(const std::string &str);
    bool Exists();
};

#endif
