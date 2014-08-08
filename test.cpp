#include <iostream>
#include <iomanip>
#include <sstream>

#include "libporto.hpp"

extern "C" {
#include <unistd.h>
}

int main(int argc, char *argv[])
{
    TPortoAPI api;

    (void)api.Destroy("t");
    assert(api.Create("t") == 0);
    assert(api.Start("t") == 0);

    std::string v;
    api.GetData("t", "root_pid", v);
    assert(v != "0");

    api.GetData("t", "exit_status", v);

    usleep(1000000);

    api.GetData("t", "stdout", v);
    std::cout << v << endl;

    return 0;
}
