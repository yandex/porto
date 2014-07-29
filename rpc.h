#ifndef __RPC_H__
#define __RPC_H__

#include "porto.h"

#define RPC_SOCK_PATH "/tmp/porto.sock"

string HandleRpcRequest(TContainerHolder &cholder, const string req);
int HandleRpcFromStream(TContainerHolder &cholder, istream &in, ostream &out);
int HandleRpcFromSocket(TContainerHolder &cholder);

#endif
