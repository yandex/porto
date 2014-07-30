#include <iostream>
#include <sstream>

#include "rpc.hpp"
#include "protobuf.hpp"

extern "C" {
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
}

class ICmd {
protected:
    int fd;

public:
    void SetFd(int fd) { this->fd = fd; }
    virtual string GetName() = 0;
    virtual string GetUsage() = 0;
    virtual string GetDescription() = 0;
    virtual int Execute(int argc, char *argv[]) = 0;
};

static vector<ICmd *> commands;

static int ConnectToRpcServer(const char *path)
{
    int sfd;
    struct sockaddr_un peer_addr;
    socklen_t peer_addr_size;

    memset(&peer_addr, 0, sizeof(struct sockaddr_un));

    sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) {
        std::cerr<<"socket() error: "<<strerror(errno)<<std::endl;
        return -1;
    }

    peer_addr.sun_family = AF_UNIX;
    strncpy(peer_addr.sun_path, path, sizeof(peer_addr.sun_path) - 1);

    peer_addr_size = sizeof(struct sockaddr_un);
    if (connect(sfd, (struct sockaddr *) &peer_addr, peer_addr_size) < 0) {
        std::cerr<<"connect() error: "<<strerror(errno)<<std::endl;
        return -2;
    }

    return sfd;
}

class THelpCmd : public ICmd {
public:
    string GetName()
    {
        return "help";
    }

    string GetUsage()
    {
        return "[command]";
    }

    string GetDescription()
    {
        return "print help message for command";
    }

    void Usage(const char *name)
    {
        cout << "usage: " << name << " <command> [<args>]" << endl;
        cout << endl;
        cout << "list of commands:" << endl;
        for (ICmd *cmd : commands)
            cout << " " << cmd->GetName() <<
                "\t\t" << cmd->GetDescription()<<endl;
    }

    int Execute(int argc, char *argv[])
    {
        if (argc <= 2) {
            Usage(argv[0]);
            return EXIT_FAILURE;
        }

        string name = argv[2];
        for (ICmd *cmd : commands) {
            if (cmd->GetName() == name) {
                cout << "usage: " << argv[0] << " " << name << " " << cmd->GetUsage() << endl;
                cout << endl;
                cout << cmd->GetDescription() << endl;

                return EXIT_SUCCESS;
            }
        }

        Usage(argv[0]);
        return EXIT_FAILURE;
    }
};

static void Usage(char *name, const char *command)
{
    ICmd *cmd = new THelpCmd();
    char *argv[] = { name, (char *)"help", (char *)command, NULL };

    cmd->Execute(command ? 3 : 1, argv);
}

static bool NeedHelp(int argc, char *argv[], bool canBeEmpty)
{
    if (argc >= 3) {
        string arg = argv[2];
        if (arg == "-h" || arg == "--help" || arg == "help")
            return true;
        else
            return false;
    }
    return canBeEmpty ? false : true;
}

static int SendReceive(int fd,
                       rpc::TContainerRequest &req,
                       rpc::TContainerResponse &rsp)
{
    google::protobuf::io::FileInputStream pist(fd);
    google::protobuf::io::FileOutputStream post(fd);

    writeDelimitedTo(req, &post);
    post.Flush();

    if (readDelimitedFrom(&pist, &rsp))
        return (int)rsp.error();
    else
        return -1;
}

class TSendCmd : public ICmd {
public:
    string GetName()
    {
        return "send";
    }

    string GetUsage()
    {
        return "<message>";
    }

    string GetDescription()
    {
        return "send raw protobuf message";
    }

    int Execute(int argc, char *argv[])
    {
        stringstream msg;

        if (NeedHelp(argc, argv, false)) {
            Usage(argv[0], GetName().c_str());
            return EXIT_FAILURE;
        }

        int fd = ConnectToRpcServer(RPC_SOCK_PATH);
        if (fd < 0) {
            std::cerr<<"Can't connect to RPC server"<<std::endl;
            return fd;
        }

        argv += 2;
        argc -= 2;

        std::vector<std::string> args(argv, argv + argc);
        copy(args.begin(), args.end(), ostream_iterator<string>(msg, " "));

        rpc::TContainerRequest req;
        rpc::TContainerResponse rsp;
        if (!google::protobuf::TextFormat::ParseFromString(msg.str(), &req) ||
            !req.IsInitialized()) {
            close(fd);
            return -1;
        }

        int ret = SendReceive(fd, req, rsp);
        if (!ret)
            cout << rsp.ShortDebugString() << endl;

        close(fd);
        return 0;
    }
};

class TCreateCmd : public ICmd {
public:
    string GetName()
    {
        return "create";
    }

    string GetUsage()
    {
        return "<name>";
    }

    string GetDescription()
    {
        return "create container with given name";
    }

    int Execute(int argc, char *argv[])
    {
        string s;
        stringstream msg;

        if (NeedHelp(argc, argv, false)) {
            Usage(argv[0], GetName().c_str());
            return EXIT_FAILURE;
        }

        int fd = ConnectToRpcServer(RPC_SOCK_PATH);
        if (fd < 0) {
            std::cerr<<"Can't connect to RPC server"<<std::endl;
            return fd;
        }

        string name = string(argv[2]);

        rpc::TContainerRequest req;
        rpc::TContainerResponse rsp;
        ::rpc::TContainerCreateRequest *create = req.mutable_create();

        create->set_name(name);

        int ret = SendReceive(fd, req, rsp);
        if (ret)
            cerr << "Can't create container, error = " << ret << endl;

        close(fd);
        return ret;
    }
};

class TDestroyCmd : public ICmd {
public:
    string GetName()
    {
        return "destroy";
    }

    string GetUsage()
    {
        return "<name>";
    }

    string GetDescription()
    {
        return "destroy container with given name";
    }

    int Execute(int argc, char *argv[])
    {
        string s;
        stringstream msg;

        if (NeedHelp(argc, argv, false)) {
            Usage(argv[0], GetName().c_str());
            return EXIT_FAILURE;
        }

        int fd = ConnectToRpcServer(RPC_SOCK_PATH);
        if (fd < 0) {
            std::cerr<<"Can't connect to RPC server"<<std::endl;
            return fd;
        }

        string name = string(argv[2]);

        rpc::TContainerRequest req;
        rpc::TContainerResponse rsp;
        ::rpc::TContainerDestroyRequest *destroy = req.mutable_destroy();

        destroy->set_name(name);

        int ret = SendReceive(fd, req, rsp);
        if (ret)
            cerr << "Can't destroy container, error = " << ret << endl;

        close(fd);
        return ret;
    }
};

class TListCmd : public ICmd {
public:
    string GetName()
    {
        return "list";
    }

    string GetUsage()
    {
        return "";
    }

    string GetDescription()
    {
        return "list created containers";
    }

    int Execute(int argc, char *argv[])
    {
        string s;
        stringstream msg;

        if (NeedHelp(argc, argv, true)) {
            Usage(argv[0], GetName().c_str());
            return EXIT_FAILURE;
        }

        int fd = ConnectToRpcServer(RPC_SOCK_PATH);
        if (fd < 0) {
            std::cerr<<"Can't connect to RPC server"<<std::endl;
            return fd;
        }

        rpc::TContainerRequest req;
        rpc::TContainerResponse rsp;

        auto *list = new ::rpc::TContainerListRequest();
        req.set_allocated_list(list);

        int ret = SendReceive(fd, req, rsp);
        if (ret) {
            cerr << "Can't list containers, error = " << ret << endl;
        } else {
            for (int i = 0; i < rsp.list().name_size(); i++)
                cout << rsp.list().name(i) << endl;
        }

        close(fd);
        return ret;
    }
};

int main(int argc, char *argv[])
{
    commands.push_back(new THelpCmd());
    commands.push_back(new TCreateCmd());
    commands.push_back(new TDestroyCmd());
    commands.push_back(new TListCmd());
    commands.push_back(new TSendCmd());

    if (argc <= 1) {
        Usage(argv[0], NULL);
        return EXIT_FAILURE;
    }

    string name = argv[1];
    if (name == "-h" || name == "--help") {
        Usage(argv[0], NULL);
        return EXIT_FAILURE;
    }

    for (ICmd *cmd : commands)
        if (cmd->GetName() == name)
            return cmd->Execute(argc, argv);

    return EXIT_FAILURE;
}
