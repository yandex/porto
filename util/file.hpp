#ifndef __FILE_HPP__
#define __FILE_HPP__

#include <string>
#include <vector>

#include "error.hpp"

class TFile {
    const std::string Path;
    const int Mode; // currently used only by WriteStringNoAppend

    TError Write(int flags, const std::string &str) const;

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

    const std::string &GetPath() const;
    EFileType Type() const;

    TError Touch() const;
    TError Remove() const;

    TError AsString(std::string &value) const;
    TError AsInt(int &value) const;
    TError AsLines(std::vector<std::string> &value) const;
    TError LastStrings(const size_t size, std::string &value) const;
    TError ReadLink(std::string &value) const;

    TError WriteStringNoAppend(const std::string &str) const;
    TError AppendString(const std::string &str) const;
    bool Exists() const;
};

#endif
