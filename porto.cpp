#include <iostream>
#include <iomanip>
#include <sstream>

#include "rpc.hpp"
#include "protobuf.hpp"

extern "C" {
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
}

class ICmd {
public:
    virtual string GetName() = 0;
    virtual string GetUsage() = 0;
    virtual string GetDescription() = 0;
    virtual int Execute(int argc, char *argv[]) = 0;

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

    int Rpc(rpc::TContainerRequest &req, rpc::TContainerResponse &rsp)
    {
        int fd = ConnectToRpcServer(RPC_SOCK_PATH);

        if (fd < 0)
            throw "Can't connect to RPC server";

        int ret = SendReceive(fd, req, rsp);

        close(fd);
        return ret;
    }
};

static vector<ICmd *> commands;

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
            cout << " " << left << setw(16) << cmd->GetName() << cmd->GetDescription() << endl;
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

        argv += 2;
        argc -= 2;

        std::vector<std::string> args(argv, argv + argc);
        copy(args.begin(), args.end(), ostream_iterator<string>(msg, " "));

        rpc::TContainerRequest req;
        rpc::TContainerResponse rsp;
        if (!google::protobuf::TextFormat::ParseFromString(msg.str(), &req) ||
            !req.IsInitialized())
            return -1;

        int ret = Rpc(req, rsp);
        if (!ret)
            cout << rsp.ShortDebugString() << endl;

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
        return "create container";
    }

    int Execute(int argc, char *argv[])
    {
        string s;
        stringstream msg;

        if (NeedHelp(argc, argv, false)) {
            Usage(argv[0], GetName().c_str());
            return EXIT_FAILURE;
        }

        string name = string(argv[2]);

        rpc::TContainerRequest req;
        rpc::TContainerResponse rsp;

        req.mutable_create()->set_name(name);

        int ret = Rpc(req, rsp);
        if (ret)
            cerr << "Can't create container, error = " << ret << endl;

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
        return "destroy container";
    }

    int Execute(int argc, char *argv[])
    {
        string s;
        stringstream msg;

        if (NeedHelp(argc, argv, false)) {
            Usage(argv[0], GetName().c_str());
            return EXIT_FAILURE;
        }

        string name = string(argv[2]);

        rpc::TContainerRequest req;
        rpc::TContainerResponse rsp;

        req.mutable_destroy()->set_name(name);

        int ret = Rpc(req, rsp);
        if (ret)
            cerr << "Can't destroy container, error = " << ret << endl;

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

        rpc::TContainerRequest req;
        rpc::TContainerResponse rsp;

        auto *list = new ::rpc::TContainerListRequest();
        req.set_allocated_list(list);

        int ret = Rpc(req, rsp);
        if (ret) {
            cerr << "Can't list containers, error = " << ret << endl;
        } else {
            for (int i = 0; i < rsp.list().name_size(); i++)
                cout << rsp.list().name(i) << endl;
        }

        return ret;
    }
};

class TGetPropertyCmd : public ICmd {
public:
    string GetName()
    {
        return "get";
    }

    string GetUsage()
    {
        return "<name> <property>";
    }

    string GetDescription()
    {
        return "get container property";
    }

    int Execute(int argc, char *argv[])
    {
        string s;
        stringstream msg;

        if (NeedHelp(argc, argv, false) || argc < 4) {
            Usage(argv[0], GetName().c_str());
            return EXIT_FAILURE;
        }

        string name = string(argv[2]);
        string property = string(argv[3]);

        rpc::TContainerRequest req;
        rpc::TContainerResponse rsp;

        req.mutable_getproperty()->set_name(name);
        req.mutable_getproperty()->add_property(property);

        int ret = Rpc(req, rsp);
        if (ret) {
            cerr << "Can't get property, error = " << ret << endl;
        } else {
            for (int i = 0; i < rsp.getproperty().value_size(); i++)
                cout << rsp.getproperty().value(i) << endl;
        }

        return ret;
    }
};

class TSetPropertyCmd : public ICmd {
public:
    string GetName()
    {
        return "set";
    }

    string GetUsage()
    {
        return "<name> <property> <value>";
    }

    string GetDescription()
    {
        return "set container property";
    }

    int Execute(int argc, char *argv[])
    {
        string s;
        stringstream msg;

        if (NeedHelp(argc, argv, false) || argc < 5) {
            Usage(argv[0], GetName().c_str());
            return EXIT_FAILURE;
        }

        string name = string(argv[2]);
        string property = string(argv[3]);
        string value = string(argv[4]);

        rpc::TContainerRequest req;
        rpc::TContainerResponse rsp;

        req.mutable_setproperty()->set_name(name);
        req.mutable_setproperty()->set_property(property);
        req.mutable_setproperty()->set_value(value);

        int ret = Rpc(req, rsp);
        if (ret)
            cerr << "Can't get property, error = " << ret << endl;

        return ret;
    }
};

class TGetDataCmd : public ICmd {
public:
    string GetName()
    {
        return "data";
    }

    string GetUsage()
    {
        return "<name> <data>";
    }

    string GetDescription()
    {
        return "get container data";
    }

    int Execute(int argc, char *argv[])
    {
        string s;
        stringstream msg;

        if (NeedHelp(argc, argv, false) || argc < 4) {
            Usage(argv[0], GetName().c_str());
            return EXIT_FAILURE;
        }

        string name = string(argv[2]);
        string data = string(argv[3]);

        rpc::TContainerRequest req;
        rpc::TContainerResponse rsp;

        req.mutable_getdata()->set_name(name);
        req.mutable_getdata()->add_data(data);

        int ret = Rpc(req, rsp);
        if (ret) {
            cerr << "Can't get data, error = " << ret << endl;
        } else {
            for (int i = 0; i < rsp.getdata().value_size(); i++)
                cout << rsp.getdata().value(i) << endl;
        }

        return ret;
    }
};

class TStartCmd : public ICmd {
public:
    string GetName()
    {
        return "start";
    }

    string GetUsage()
    {
        return "<name>";
    }

    string GetDescription()
    {
        return "start container";
    }

    int Execute(int argc, char *argv[])
    {
        string s;
        stringstream msg;

        if (NeedHelp(argc, argv, false)) {
            Usage(argv[0], GetName().c_str());
            return EXIT_FAILURE;
        }

        string name = string(argv[2]);

        rpc::TContainerRequest req;
        rpc::TContainerResponse rsp;

        req.mutable_start()->set_name(name);

        int ret = Rpc(req, rsp);
        if (ret)
            cerr << "Can't start container, error = " << ret << endl;

        return ret;
    }
};

class TStopCmd : public ICmd {
public:
    string GetName()
    {
        return "stop";
    }

    string GetUsage()
    {
        return "<name>";
    }

    string GetDescription()
    {
        return "stop container";
    }

    int Execute(int argc, char *argv[])
    {
        string s;
        stringstream msg;

        if (NeedHelp(argc, argv, false)) {
            Usage(argv[0], GetName().c_str());
            return EXIT_FAILURE;
        }

        string name = string(argv[2]);

        rpc::TContainerRequest req;
        rpc::TContainerResponse rsp;

        req.mutable_stop()->set_name(name);

        int ret = Rpc(req, rsp);
        if (ret)
            cerr << "Can't stop container, error = " << ret << endl;

        return ret;
    }
};

class TPauseCmd : public ICmd {
public:
    string GetName()
    {
        return "pause";
    }

    string GetUsage()
    {
        return "<name>";
    }

    string GetDescription()
    {
        return "pause container";
    }

    int Execute(int argc, char *argv[])
    {
        string s;
        stringstream msg;

        if (NeedHelp(argc, argv, false)) {
            Usage(argv[0], GetName().c_str());
            return EXIT_FAILURE;
        }

        string name = string(argv[2]);

        rpc::TContainerRequest req;
        rpc::TContainerResponse rsp;

        req.mutable_pause()->set_name(name);

        int ret = Rpc(req, rsp);
        if (ret)
            cerr << "Can't pause container, error = " << ret << endl;

        return ret;
    }
};

class TResumeCmd : public ICmd {
public:
    string GetName()
    {
        return "resume";
    }

    string GetUsage()
    {
        return "<name>";
    }

    string GetDescription()
    {
        return "resume container";
    }

    int Execute(int argc, char *argv[])
    {
        string s;
        stringstream msg;

        if (NeedHelp(argc, argv, false)) {
            Usage(argv[0], GetName().c_str());
            return EXIT_FAILURE;
        }

        string name = string(argv[2]);

        rpc::TContainerRequest req;
        rpc::TContainerResponse rsp;

        req.mutable_resume()->set_name(name);

        int ret = Rpc(req, rsp);
        if (ret)
            cerr << "Can't resume container, error = " << ret << endl;

        return ret;
    }
};

int main(int argc, char *argv[])
{
    commands.push_back(new THelpCmd());
    commands.push_back(new TCreateCmd());
    commands.push_back(new TDestroyCmd());
    commands.push_back(new TListCmd());
    commands.push_back(new TStartCmd());
    commands.push_back(new TStopCmd());
    commands.push_back(new TPauseCmd());
    commands.push_back(new TResumeCmd());
    commands.push_back(new TGetPropertyCmd());
    commands.push_back(new TSetPropertyCmd());
    commands.push_back(new TGetDataCmd());
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
