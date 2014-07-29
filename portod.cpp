#include "portod.h"

/* Example:
 * nc -U /run/porto.socket
 * create: { name: "test" }
 * list: { }
 *
 */

int main(int argc, const char *argv[])
{
    TContainerHolder cholder;

    //return HandleRpcFromStream(cholder, std::cin, std::cout);
    return HandleRpcFromSocket(cholder, RPC_SOCK_PATH);
}
