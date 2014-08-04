#include <iostream>
#include <vector>

#include "kvalue.hpp"
#include "cgroup.hpp"

extern void dump_reg(void);
int main() {
    TMountSnapshot ms;
    TMountSnapshot ms1;
    {
        TMountSnapshot ms2;
    }
    dump_reg();

    return EXIT_SUCCESS;
}

int main3() {
    try {
        TCgroupSnapshot cgs;
        cout << cgs << endl;
    } catch (const char *e) {
        cerr << e << endl;
        return EXIT_FAILURE;
    } catch (string e) {
        cerr << e << endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

int main2() {
    try {
        TKeyValueStorage st;

        st.MountTmpfs();

        map<string, map<string, string> >data =
            {{"basesearch_1",
              {{"memory_guarantee", "10G"},
               {"policy", "runtime"}}},
             {"basesearch_2",
              {{"memory_guarantee", "20G"},
               {"policy", "runtime"}}},
             {"sky",
              {{"memory_limit", "2G"},
               {"policy", "system"}}},
             {"nirvana_1",
              {{"memory_guarantee", "1G"},
               {"memory_guarantee", "500M"},
               {"policy", "batch"}}},
             {"nirvana_2",
              {{"memory_guarantee", "1G"},
               {"memory_guarantee", "500M"},
               {"policy", "batch"}}}};

        for (auto node : data) {
            st.CreateNode(node.first);

            for (auto key : node.second)
                st.Save(node.first, key.first, key.second);

            for (auto key : node.second)
                if (st.Load(node.first, key.first) != key.second)
                    throw;

            st.RemoveNode(node.first);
        }
    } catch (string e) {
        cerr << e << endl;
        return EXIT_FAILURE;
    } catch (...) {
        cerr << "Error!" << endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
