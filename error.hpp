#ifndef __ERROR_HPP__
#define __ERROR_HPP__

class TError {
    int error;

public:
    TError() : error(0) {
    }

    TError(int error) : error(error) {
    }

    bool Ok() {
        return error == 0;
    }

    int GetError() {
        return error;
    }
};

#endif
