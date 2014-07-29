#ifndef __RPC_H__
#define __RPC_H__

#include "container.h"

string HandleRpcRequest(TContainerHolder &cholder, const string req);
int HandleRpcFromStream(TContainerHolder &cholder, istream &in, ostream &out);
int HandleRpcFromSocket(TContainerHolder &cholder, const char *path);

#endif
