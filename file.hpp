#ifndef __FILE_HPP__
#define __FILE_HPP__

#include <string>

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
};

#endif
