#ifndef __QDISC_H__
#define __QDISC_H__

#include <memory>
#include <string>

#include "error.hpp"

class TQdisc;

class TTclass {
    const std::shared_ptr<TQdisc> ParentQdisc;
    const std::shared_ptr<TTclass> ParentTclass;

public:
    TTclass(const std::shared_ptr<TQdisc> qdisc) : ParentQdisc(qdisc) { }
    TTclass(const std::shared_ptr<TTclass> tclass) : ParentTclass(tclass) { }
    ~TTclass() { Remove(); }

    TError Create();
    TError Remove();
};

class TQdisc {
    const std::string Device;

public:
    TQdisc(const std::string &device) : Device(device) { }
    ~TQdisc() { Remove(); }

    TError Create();
    TError Remove();
};

#endif
