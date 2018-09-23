#include <libporto.hpp>

#include <cassert>

#define Expect(a)   assert(a)
#define ExpectEq(a, b)   assert((a) == (b))
#define ExpectNeq(a, b)   assert((a) != (b))
#define ExpectSuccess(ret) assert((ret) == Porto::EError::Success)

int main(int, char **) {
    std::vector<Porto::TString> list;
    Porto::TString str, path;
    uint64_t val;

    Porto::Connection api;

    ExpectSuccess(api.Connect());

    val = api.GetTimeout();
    ExpectNeq(val, 0);
    ExpectSuccess(api.SetTimeout(5));

    ExpectSuccess(api.GetVersion(str, str));

    ExpectSuccess(api.List(list));

    ExpectSuccess(api.ListProperties(list));

    ExpectSuccess(api.ListVolumes(list));

    ExpectSuccess(api.ListVolumeProperties(list));

    ExpectSuccess(api.ListLayers(list));

    ExpectSuccess(api.ListStorages(list));

    ExpectSuccess(api.Call("Version {}", str));

    ExpectSuccess(api.GetProperty("/", "state", str));
    ExpectEq(str, "meta");

    auto ct = api.GetContainerSpec("/");
    Expect(ct != nullptr);
    ExpectEq(ct->name(), "/");

    val = 0;
    ExpectSuccess(api.GetProperty("/", "memory_usage", val));
    ExpectNeq(val, 0);

    ExpectEq(api.GetProperty("/", "__wrong__", val), Porto::EError::InvalidProperty);
    ExpectEq(api.Error(), Porto::EError::InvalidProperty);
    ExpectEq(api.GetLastError(str), Porto::EError::InvalidProperty);

    ct = api.GetContainerSpec("a");
    Expect(ct == nullptr);
    ExpectEq(api.Error(), Porto::EError::ContainerDoesNotExist);

    ExpectSuccess(api.Create("a"));

    ExpectSuccess(api.SetProperty("a", "memory_limit", "1M"));
    ExpectSuccess(api.GetProperty("a", "memory_limit", val));
    ExpectEq(val, 1 << 20);

    ct = api.GetContainerSpec("a");
    Expect(ct != nullptr);
    ExpectEq(ct->memory_limit(), 1 << 20);

    ExpectSuccess(api.WaitContainer("a", str));
    ExpectEq(str, "stopped");

    ExpectSuccess(api.CreateVolume(path, {
                {"containers", "a"},
                {"backend", "native"},
                {"space_limit", "1G"}}));
    ExpectNeq(path, "");

    auto vd = api.GetVolume(path);
    Expect(vd != nullptr);
    ExpectEq(vd->path(), path);

    auto vs = api.GetVolumeSpec(path);
    Expect(vs != nullptr);
    ExpectEq(vs->path(), path);

    ExpectSuccess(api.SetProperty("a", "command", "sleep 1000"));
    ExpectSuccess(api.Start("a"));

    ExpectSuccess(api.GetProperty("a", "state", str));
    ExpectEq(str, "running");

    ExpectSuccess(api.Destroy("a"));

    api.Close();

    return 0;
}
