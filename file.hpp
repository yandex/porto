#ifndef __FILE_HPP__
#define __FILE_HPP__

#include <string>
#include <vector>

class TFile {
    std::string path;

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

    TFile(std::string path);

    std::string Path();
    EFileType Type();

    void Remove();

    std::string AsString();
    std::vector<std::string> AsLines();

    void WriteStringNoAppend(std::string str);
    int AppendString(std::string str);
};

#endif
