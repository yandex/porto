#ifndef __RPC_H__
#define __RPC_H__

// TODO: /run/porto.socket
#define RPC_SOCK_PATH "/tmp/porto.socket"

#include "container.h"

int HandleRpcFromStream(TContainerHolder &cholder, istream &in, ostream &out);

#endif
