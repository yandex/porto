#include <iostream>
#include <vector>

#include "kvalue.hpp"
#include "cgroup.hpp"

extern void dump_reg(void);
int main() {
    TMountState ms;
    TMountState ms1;
    {
        TMountState ms2;
    }
    dump_reg();
}

int main3() {
    TCgroupState cgs;

    try {
        cgs.MountMissingTmpfs();
        cgs.MountMissingControllers();

        cout << cgs << endl;

        cgs.UmountAll();

    } catch (const char *e) {
        cerr << e << endl;
        return EXIT_FAILURE;
    } catch (string e) {
        cerr << e << endl;
        return EXIT_FAILURE;
    }
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
