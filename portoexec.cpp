#include <iostream>
#include <map>
#include <string>

#include "libporto.hpp"
#include "cli.hpp"

using std::string;
using std::map;

class TRunCmd : public ICmd {
public:
    TRunCmd() : ICmd("run", 2, "<container> [properties]", "create and start container with given properties") {}

    int Parser(string property, map<string, string> &properties) {
        string propertyKey, propertyValue;
        string::size_type n;
        n = property.find('=');
        if (n == string::npos) {
            TError error(EError::InvalidValue, "Invalid value");
            PrintError(error, "Can't parse property: " + property);
            return EXIT_FAILURE;
        }
        propertyKey = property.substr(0, n);
        propertyValue = property.substr(n+1, property.size());
        if (propertyKey == "" || propertyValue == "") {
            TError error(EError::InvalidValue, "Invalid value");
            PrintError(error, "Can't parse property: " + property);
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
            PrintError("Can't create container");
            return EXIT_FAILURE;
        }
        for (auto iter: properties) {
            ret = Api.SetProperty(containerName, iter.first, iter.second);
            if (ret) {
                 PrintError("Can't set property");
                 (void)Api.Destroy(containerName);
                 return EXIT_FAILURE;
            }
        }
        ret = Api.Start(containerName);
        if (ret) {
            PrintError("Can't start property");
            (void)Api.Destroy(containerName);
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
};

int main(int argc, char *argv[]) {
    RegisterCommand(new THelpCmd(false));
    RegisterCommand(new TDestroyCmd());
    RegisterCommand(new TListCmd());
    RegisterCommand(new TRunCmd());

    return HandleCommand(argc, argv);
};
