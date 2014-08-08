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

    std::vector<std::string> v;
    api.GetData("t", "root_pid", v);
    assert(v[0] != "0");

    api.GetData("t", "exit_status", v);

    usleep(1000000);

    std::vector<std::string> s;
    api.GetData("t", "stdout", s);
    std::cout << s[0] << endl;

    return 0;
}
