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
protected:
    string name, usage, desc;
    int need_args;

    rpc::TContainerRequest req;
    rpc::TContainerResponse rsp;

public:
    ICmd(const string& name, int args, const string& usage, const string& desc) :
        name(name), usage(usage), desc(desc), need_args(args) {}

    string& GetName() { return name; }
    string& GetUsage() { return usage; }
    string& GetDescription() { return desc; }

    bool ValidArgs(int argc, char *argv[]) {
        if (need_args < argc)
            return false;

        return true;
    }

    virtual int Execute(int argc, char *argv[]) = 0;

    static int SendReceive(int fd,
                           rpc::TContainerRequest &req,
                           rpc::TContainerResponse &rsp)
    {
        google::protobuf::io::FileInputStream pist(fd);
        google::protobuf::io::FileOutputStream post(fd);

        WriteDelimitedTo(req, &post);
        post.Flush();

        if (ReadDelimitedFrom(&pist, &rsp))
            return (int)rsp.error();
        else
            return -1;
    }

    int Rpc(rpc::TContainerRequest &req, rpc::TContainerResponse &rsp)
    {
        int fd;
        TError error = ConnectToRpcServer(RPC_SOCK_PATH, fd);
        if (error)
            throw "Can't connect to RPC server: " + error.GetMsg();

        int ret = SendReceive(fd, req, rsp);

        close(fd);
        return ret;
    }
};

static vector<ICmd *> commands;

class THelpCmd : public ICmd {
public:
    THelpCmd() : ICmd("help", 0, "[command]", "print help message for command") {}

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

class TRawCmd : public ICmd {
public:
    TRawCmd() : ICmd("raw", 2, "<message>", "send raw protobuf message") {}

    int Execute(int argc, char *argv[])
    {
        stringstream msg;

        argv += 2;
        argc -= 2;

        std::vector<std::string> args(argv, argv + argc);
        copy(args.begin(), args.end(), ostream_iterator<string>(msg, " "));

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
    TCreateCmd() : ICmd("create", 1, "<name>", "create container") {}

    int Execute(int argc, char *argv[])
    {
        string s;

        string name = string(argv[2]);

        req.mutable_create()->set_name(name);

        int ret = Rpc(req, rsp);
        if (ret)
            cerr << "Can't create container, error = " << ret << endl;

        return ret;
    }
};

class TDestroyCmd : public ICmd {
public:
    TDestroyCmd() : ICmd("destroy", 1, "<name>", "destroy container") {}

    int Execute(int argc, char *argv[])
    {
        string s;

        string name = string(argv[2]);

        req.mutable_destroy()->set_name(name);

        int ret = Rpc(req, rsp);
        if (ret)
            cerr << "Can't destroy container, error = " << ret << endl;

        return ret;
    }
};

class TListCmd : public ICmd {
public:
    TListCmd() : ICmd("list", 0, "", "list created containers") {}

    int Execute(int argc, char *argv[])
    {
        string s;

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

class TPropertyListCmd : public ICmd {
public:
    TPropertyListCmd() : ICmd("plist", 1, "", "list supported container properties") {}

    int Execute(int argc, char *argv[])
    {
        string s;

        auto *list = new ::rpc::TContainerPropertyListRequest();
        req.set_allocated_propertylist(list);

        int ret = Rpc(req, rsp);
        if (ret) {
            cerr << "Can't list properties, error = " << ret << endl;
        } else {
            auto list = rsp.propertylist();

            for (int i = 0; i < list.list_size(); i++) {
                cout << list.list(i).name() << " - " << list.list(i).desc() << endl;
            }
        }

        return ret;
    }
};

class TDataListCmd : public ICmd {
public:
    TDataListCmd() : ICmd("dlist", 1, "", "list supported container data") {}

    int Execute(int argc, char *argv[])
    {
        string s;

        auto *list = new ::rpc::TContainerDataListRequest();
        req.set_allocated_datalist(list);

        int ret = Rpc(req, rsp);
        if (ret) {
            cerr << "Can't list data, error = " << ret << endl;
        } else {
            auto list = rsp.datalist();

            for (int i = 0; i < list.list_size(); i++) {
                cout << list.list(i).name() << " - " << list.list(i).desc() << endl;
            }
        }

        return ret;
    }
};

class TGetPropertyCmd : public ICmd {
public:
    TGetPropertyCmd() : ICmd("get", 2, "<name> <property>", "get container property") {}

    int Execute(int argc, char *argv[])
    {
        string s;

        string name = string(argv[2]);
        string property = string(argv[3]);

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
    TSetPropertyCmd() : ICmd("set", 3, "<name> <property>", "set container property") {}

    int Execute(int argc, char *argv[])
    {
        string s;

        string name = string(argv[2]);
        string property = string(argv[3]);
        string value = string(argv[4]);

        req.mutable_setproperty()->set_name(name);
        req.mutable_setproperty()->set_property(property);
        req.mutable_setproperty()->set_value(value);

        int ret = Rpc(req, rsp);
        if (ret)
            cerr << "Can't set property, error = " << ret << endl;

        return ret;
    }
};

class TGetDataCmd : public ICmd {
public:
    TGetDataCmd() : ICmd("data", 2, "<name> <data>", "get container data") {}

    int Execute(int argc, char *argv[])
    {
        string s;

        string name = string(argv[2]);
        string data = string(argv[3]);

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
    TStartCmd() : ICmd("start", 1, "<name>", "start container") {}

    int Execute(int argc, char *argv[])
    {
        string s;

        string name = string(argv[2]);

        req.mutable_start()->set_name(name);

        int ret = Rpc(req, rsp);
        if (ret)
            cerr << "Can't start container, error = " << ret << endl;

        return ret;
    }
};

class TStopCmd : public ICmd {
public:
    TStopCmd() : ICmd("stop", 1, "<name>", "stop container") {}

    int Execute(int argc, char *argv[])
    {
        string s;

        string name = string(argv[2]);

        req.mutable_stop()->set_name(name);

        int ret = Rpc(req, rsp);
        if (ret)
            cerr << "Can't stop container, error = " << ret << endl;

        return ret;
    }
};

class TPauseCmd : public ICmd {
public:
    TPauseCmd() : ICmd("pause", 1, "<name>", "pause container") {}

    int Execute(int argc, char *argv[])
    {
        string s;

        string name = string(argv[2]);

        req.mutable_pause()->set_name(name);

        int ret = Rpc(req, rsp);
        if (ret)
            cerr << "Can't pause container, error = " << ret << endl;

        return ret;
    }
};

class TResumeCmd : public ICmd {
public:
    TResumeCmd() : ICmd("resume", 1, "<name>", "resume container") {}

    int Execute(int argc, char *argv[])
    {
        string s;

        string name = string(argv[2]);

        req.mutable_resume()->set_name(name);

        int ret = Rpc(req, rsp);
        if (ret)
            cerr << "Can't resume container, error = " << ret << endl;

        return ret;
    }
};

int main(int argc, char *argv[])
{
    commands = {
        new THelpCmd(),
        new TCreateCmd(),
        new TDestroyCmd(),
        new TListCmd(),
        new TPropertyListCmd(),
        new TDataListCmd(),
        new TStartCmd(),
        new TStopCmd(),
        new TPauseCmd(),
        new TResumeCmd(),
        new TGetPropertyCmd(),
        new TSetPropertyCmd(),
        new TGetDataCmd(),
        new TRawCmd()
    };

    if (argc <= 1) {
        Usage(argv[0], NULL);
        return EXIT_FAILURE;
    }

    string name = argv[1];
    if (name == "-h" || name == "--help") {
        Usage(argv[0], NULL);
        return EXIT_FAILURE;
    }

    try {
        for (ICmd *cmd : commands)
            if (cmd->GetName() == name) {
                if (!cmd->ValidArgs(argc - 2, &argv[2])) {
                    Usage(argv[0], cmd->GetName().c_str());
                    return EXIT_FAILURE;
                }
                return cmd->Execute(argc, argv);
            }
    } catch (const char *err) {
        cerr << err << endl;
    }

    return EXIT_FAILURE;
}
