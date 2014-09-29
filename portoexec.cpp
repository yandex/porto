#include <iostream>
#include <map>
#include <string>

#include "libporto.hpp"

using namespace std;

class ICmd {
protected:
    int NeedArgs;
    TPortoAPI Api;

public:
    ICmd(int args) : NeedArgs(args) {}

    bool ValidArgs(int argc) {
        if (argc < NeedArgs)
            return false;
        return true;
    }

    string GetMsg() {
        string msg;
        int err;
        Api.GetLastError(err, msg);
        return msg;
    }

    virtual int Execute(int argc, char *argv[]) = 0;
};

static map<string, ICmd *> commands;

class TRunCmd : public ICmd {
public:
    TRunCmd() : ICmd(2) {}

    int Parser(string property, map<string, string> &properties) {
        string propertyKey, propertyValue;
        string::size_type n;
        n = property.find('=');
        if (n == string::npos) {
            cerr << "Can't parse property: " << property << endl;
            return EXIT_FAILURE;
        }
        propertyKey = property.substr(0, n);
        propertyValue = property.substr(n+1, property.size());
        if (propertyKey == "" || propertyValue == "") {
            cerr << "Can't parse property: " << property << endl;
            return EXIT_FAILURE;
        }
        properties[propertyKey] = propertyValue;
        return EXIT_SUCCESS;
    }

    int Execute(int argc, char *argv[]) {
        string containerName = argv[0];
        map<string, string> properties;
        int ret;

        for (int i = 1; i < argc; i++) {
            ret = Parser(argv[i], properties);
            if (ret)
                return ret;
        }

        ret = Api.Create(containerName);
        if (ret) {
            cerr << "Can't create container: " << GetMsg() << endl;
            return EXIT_FAILURE;
        }
        for (auto iter: properties) {
            ret = Api.SetProperty(containerName, iter.first, iter.second);
            if (ret) {
                 cerr << "Can't set property: "  << GetMsg() << endl;
                 (void)Api.Destroy(containerName);
                 return EXIT_FAILURE;
            }
        }
        ret = Api.Start(containerName);
        if (ret) {
            cerr << "Can't start container: " << GetMsg() << endl;
            (void)Api.Destroy(containerName);
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
};

class TDestroyCmd : public ICmd {
public:
    TDestroyCmd() : ICmd(1) {}
    int Execute(int argc, char *argv[]) {
        string container_name = argv[0];
        int ret = Api.Destroy(container_name);
        if (ret) {
            cerr << "Can't destroy container" << GetMsg() << endl;
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
};

class THelpCmd : public ICmd {
public:
    THelpCmd() : ICmd(0) {}
    int Execute(int argc, char *argv[]) {
        cout << "portoexec - run command throw portod" << endl;
        cout << endl;
        cout << "SYNOPSYS" << endl;
        cout << "\tportoexec run <container_name> <properties>" << endl;
        cout << "\tportoexec destroy <container_name>" << endl;
        return EXIT_FAILURE;
    }
};


int main(int argc, char *argv[]) {
    commands = {
        { "run", new TRunCmd() },
        { "destroy", new TDestroyCmd() },
        { "help", new THelpCmd() },
    };

    if (argc <= 1)
        return commands["help"]->Execute(0, NULL);

    string commandName = argv[1];
    if (commands.find(commandName) == commands.end())
        return commands["help"]->Execute(0, NULL);

    if (!commands[commandName]->ValidArgs(argc - 2))
        return commands["help"]->Execute(0, NULL);

    return commands[commandName]->Execute(argc - 2, argv + 2);
}
