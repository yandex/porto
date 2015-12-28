#pragma once

#include <map>
#include <vector>
#include <string>
#include <memory>

struct TProperty {
    std::string Name;
    std::string Description;

    TProperty(std::string name, std::string description) :
        Name(name), Description(description) {}
};

struct TData {
    std::string Name;
    std::string Description;

    TData(std::string name, std::string description) :
        Name(name), Description(description) {}
};

struct TVolumeDescription {
    std::string Path;
    std::map<std::string, std::string> Properties;
    std::vector<std::string> Containers;

    TVolumeDescription() {}
    TVolumeDescription(const std::string &path,
                       const std::map<std::string, std::string> &properties,
                       const std::vector<std::string> &containers) :
        Path(path), Properties(properties), Containers(containers) {}
};

struct TPortoGetResponse {
    std::string Value;
    int Error;
    std::string ErrorMsg;
};

using TGetResponseMap = std::map<std::string, std::map<std::string, TPortoGetResponse>>;

class TPortoAPI {
    class TPortoAPIImpl;

    std::unique_ptr<TPortoAPIImpl> Impl;

    TPortoAPI(const TPortoAPI&) = delete;
    void operator=(const TPortoAPI&) = delete;

public:
    TPortoAPI(const std::string &path, int retries = 5);
    ~TPortoAPI();

    void Cleanup(); // TODO: remove

    int Create(const std::string &name);
    int Destroy(const std::string &name);

    int Start(const std::string &name);
    int Stop(const std::string &name);
    int Kill(const std::string &name, int sig);
    int Pause(const std::string &name);
    int Resume(const std::string &name);

    int Wait(const std::vector<std::string> &containers, std::string &name, int timeout = -1);

    int List(std::vector<std::string> &clist);
    int Plist(std::vector<TProperty> &plist);
    int Dlist(std::vector<TData> &dlist);

    int Get(const std::vector<std::string> &name,
            const std::vector<std::string> &variable,
            TGetResponseMap &result);

    int GetProperty(const std::string &name, const std::string &property, std::string &value);
    int SetProperty(const std::string &name, const std::string &property, std::string value);

    int GetData(const std::string &name, const std::string &data, std::string &value);
    int GetVersion(std::string &tag, std::string &revision);

    int Raw(const std::string &message, std::string &response);
    void GetLastError(int &error, std::string &msg) const;

    // VolumeAPI
    int ListVolumeProperties(std::vector<TProperty> &properties);
    int CreateVolume(const std::string &path,
                     const std::map<std::string, std::string> &config,
                     TVolumeDescription &result);
    int CreateVolume(std::string &path,
                     const std::map<std::string, std::string> &config);
    int LinkVolume(const std::string &path, const std::string &container = std::string());
    int UnlinkVolume(const std::string &path, const std::string &container = std::string());
    int ListVolumes(const std::string &path, const std::string &container,
                    std::vector<TVolumeDescription> &volumes);
    int ListVolumes(std::vector<TVolumeDescription> &volumes) {
        return ListVolumes(std::string(), std::string(), volumes);
    }

    int ImportLayer(const std::string &layer, const std::string &tarball, bool merge = false);
    int ExportLayer(const std::string &volume, const std::string &tarball);
    int RemoveLayer(const std::string &layer);
    int ListLayers(std::vector<std::string> &layers);
};

class TPortoAPIExtentionBase {
    TPortoAPIExtentionBase(const TPortoAPIExtentionBase &) = delete;
    void operator=(const TPortoAPIExtentionBase &) = delete;

public:
    TPortoAPIExtentionBase(TPortoAPI &api) : Api(api) {}

protected:
    TPortoAPI &Api;
};

class TPortoAPIContainer : public TPortoAPIExtentionBase {
    const std::string Name;

public:
    TPortoAPIContainer(TPortoAPI &api, const std::string &name);

    const std::string &GetName() const { return Name; }

    int Create();
    int Destroy();
    int Start();
    int Stop();
    int Kill(int sig);
    int Pause();
    int Resume();
};

class TPortoAPIVolume : public TPortoAPIExtentionBase {
    std::string Path;

public:
    explicit TPortoAPIVolume(TPortoAPI &api);
    TPortoAPIVolume(TPortoAPI &api, const std::string &path);

    const std::string &GetPath() const { return Path; }

    int Create(const std::map<std::string, std::string> &config,
               TVolumeDescription &result);
    int Create(const std::map<std::string, std::string> &config);
    int Link(const std::string &container = std::string());
    int Unlink(const std::string &container = std::string());
    int List(const std::string &container,
             std::vector<TVolumeDescription> &volumes);
};

class TPortoAPILayer : public TPortoAPIExtentionBase {
    const std::string Name;

public:
    TPortoAPILayer(TPortoAPI &api, const std::string &name);

    const std::string &GetName() const { return Name; }

    int Import(const std::string &tarball, bool merge = false);
    int Export(const std::string &volume, const std::string &tarball);
    int Remove();
};
