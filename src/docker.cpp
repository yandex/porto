#include "util/nlohmann/json.hpp"
#include "util/http.hpp"
#include "util/log.hpp"
#include "helpers.hpp"
#include "common.hpp"
#include "storage.hpp"
#include "docker.hpp"

#include <fcntl.h>

constexpr const char *DOCKER_TAGS_NAME = "tags";
constexpr const char *DOCKER_LAYERS_NAME = "layers";

std::vector<std::unique_ptr<TFileMutex>> TDockerImage::Lock(const TPath &place) {
    std::vector<std::unique_ptr<TFileMutex>> mutexes;

    auto createAndAppend = [&mutexes](TPath &&path) {
        if (!path.Exists())
            path.MkdirAll(0755);
        mutexes.emplace_back(new TFileMutex(path, O_CLOEXEC | O_DIRECTORY));
    };

    createAndAppend(ImagePath(place));
    for (auto &layer: Layers)
       createAndAppend(layer.LayerPath(place));

    return mutexes;
}

TPath TDockerImage::ImagePath(const TPath &place) const {
    return place / PORTO_DOCKER_IMAGES / fmt::format("v{}", SchemaVersion) / Registry / Repository / Name;
}

TPath TDockerImage::DigestPath(const TPath &place) const {
    TPath imagePath = ImagePath(place);
    if (!Digest.empty())
        return imagePath / Digest;

    return TPath(imagePath / DOCKER_TAGS_NAME / Tag).RealPath();
}

TError TDockerImage::GetAuthToken(const std::string &host, const std::string &service) {
    TError error;

    std::string response;
    error = THttpClient::SingleRequest(fmt::format("https://{}/token?service={}&scope=repository:{}/{}:pull",
                host.empty() ? DOCKER_AUTH_HOST : host,
                service.empty() ? DOCKER_AUTH_SERVICE : service,
                Repository, Name), response);
    if (error)
        return error;

    auto responseJson = nlohmann::json::parse(response);
    AuthToken = "Bearer " + responseJson["token"].get<std::string>();

    return OK;
}

TError TDockerImage::ParseManifest() {
    auto manifestJson = nlohmann::json::parse(Manifest);
    if (!manifestJson.contains("schemaVersion"))
        return TError(EError::Docker, "schemaVersion is not found in manifest");

    if (SchemaVersion != manifestJson["schemaVersion"].get<int>())
        return TError(EError::Docker, "schemaVersions are not equal");

    if (SchemaVersion == 1) {
        auto history = manifestJson["history"];
        if (history.is_null())
            return TError(EError::Docker, "history is empty in manifest");

        for (const auto &h: history) {
            if (h.contains("v1Compatibility")) {
                auto c = nlohmann::json::parse(h["v1Compatibility"].get<std::string>());
                Digest = c["id"].get<std::string>();
                Config = c.dump();
                break;
            }
        }

        for (const auto &layer: manifestJson["fsLayers"]) {
            Layers.emplace_back(TrimDigest(layer["blobSum"]));
        }
    } else if (SchemaVersion == 2) {
        Digest = TrimDigest(manifestJson["config"]["digest"].get<std::string>());

        for (const auto &layer: manifestJson["layers"]) {
            auto mediaType = layer["mediaType"].get<std::string>();
            if (mediaType != "application/vnd.docker.image.rootfs.diff.tar.gzip")
                return TError(EError::Docker, "Unknown layer mediaType: {}", mediaType);

            Layers.emplace_back(TrimDigest(layer["digest"]), layer["size"]);
        }
    } else
        return TError(EError::Docker, "Unknown manifest schemaVersion: {}", SchemaVersion);

    return OK;
}

TError TDockerImage::DownloadManifest(const THttpClient &client) {
    TError error;

    const THttpClient::THeaders headers = {
        { "Authorization", AuthToken },
        { "Accept", "application/vnd.docker.distribution.manifest.v2+json" },
        { "Accept", "application/vnd.docker.distribution.manifest.list.v2+json" },
        { "Accept", "application/vnd.docker.distribution.manifest.v1+json" },
    };

    std::string manifests;
    error = client.MakeRequest(fmt::format("/v2/{}/{}/manifests/{}", Repository, Name, Tag), manifests, headers);
    if (error)
        return error;

    auto manifestJson = nlohmann::json::parse(manifests);
    if (!manifestJson.contains("schemaVersion"))
        return TError(EError::Docker, "schemaVersion is not found in manifest");

    SchemaVersion = manifestJson["schemaVersion"].get<int>();
    if (SchemaVersion == 1) {
        Manifest = manifests;
    } else if (SchemaVersion == 2) {
        auto mediaType = manifestJson["mediaType"].get<std::string>();
        if (mediaType == "application/vnd.docker.distribution.manifest.v2+json") {
            Manifest = manifests;
        } else if (mediaType == "application/vnd.docker.distribution.manifest.list.v2+json") {
            const std::string targetArch = "amd64";
            bool found = false;
            for (const auto &m: manifestJson["manifests"]) {
                if (m["platform"]["architecture"] == targetArch) {
                    found = true;
                    auto digest = m["digest"].get<std::string>();
                    error = client.MakeRequest(fmt::format("/v2/{}/{}/manifests/{}", Repository, Name, digest), Manifest, headers);
                    if (error)
                        return error;
                }
            }
            if (!found)
                return TError(EError::Docker, "Manifest for arch {} is not found", targetArch);
        } else
            return TError(EError::Docker, "Unknown manifest mediaType: {}", mediaType);
    } else
        return TError(EError::Docker, "Unknown manifest schemaVersion: {}", SchemaVersion);

    return OK;
}

TError TDockerImage::ParseConfig() {
    auto configJson = nlohmann::json::parse(Config);
    auto config = configJson["config"];

    auto entrypoint = config["Entrypoint"];
    if (!entrypoint.is_null()) {
        for (const auto &c: entrypoint)
            Command.emplace_back(c);
    } else {
        Command.emplace_back("/bin/sh");
        Command.emplace_back("-c");
    }

    auto cmd = config["Cmd"];
    if (!cmd.is_null()) {
        for (const auto &c: cmd)
            Command.emplace_back(c);
    }

    auto env = config["Env"];
    if (!env.is_null()) {
        for (const auto &e: env)
            Env.emplace_back(e);
    }

    return OK;
}

TError TDockerImage::DownloadConfig(const THttpClient &client) {
    if (SchemaVersion == 1)
        return OK;

    TError error;
    const THttpClient::THeaders headers = {
        { "Authorization", AuthToken },
    };

    return client.MakeRequest(fmt::format("/v2/{}/{}/blobs/sha256:{}", Repository, Name, Digest), Config, headers);
}

TError TDockerImage::LinkTag(const TPath &place) const {
    TError error;
    TPath digestPath = ImagePath(place) / Digest;
    TPath tagPath = ImagePath(place) / DOCKER_TAGS_NAME;

    if (!tagPath.Exists()) {
        error = tagPath.Mkdir(0755);
        if (error)
            return error;
    }

    tagPath /= Tag;

    error = tagPath.Symlink(digestPath);
    if (error) {
        if (error.Errno != EEXIST)
            return error;

        // load tags of current digest
        std::string buff;
        error = TPath(tagPath.RealPath() / DOCKER_TAGS_NAME).ReadAll(buff);
        if (error)
            return error;

        std::vector<std::string> currentDigestTags = SplitString(buff, ' ');
        if (currentDigestTags.size() <= 1) {
            // remove current digest
            error = tagPath.RealPath().RemoveAll();
            if (error)
                return error;
        } else {
            // delete tag from current digest
            std::remove(currentDigestTags.begin(), currentDigestTags.end(), Tag);
            error = TPath(tagPath.RealPath() / DOCKER_TAGS_NAME).WriteLines(currentDigestTags);
            if (error)
                return error;
        }

        // clean current tag
        error = tagPath.Unlink();
        if (error)
            return error;

        // attempt to recreate
        error = tagPath.Symlink(digestPath);
        if (error)
            return error;
    }

    return OK;
}

TError TDockerImage::SaveTags(const TPath &place) const {
    TError error;
    TPath tagsPath = DigestPath(place) / DOCKER_TAGS_NAME;

    auto tags = std::vector<std::string>(Tags.cbegin(), Tags.cend());

    error = tagsPath.CreateRegular();
    if (error)
        return error;

    error = tagsPath.WriteLines(tags);
    if (error)
        return error;

    return OK;
}

TError TDockerImage::LoadTags(const TPath &place) {
    TError error;
    TPath tagsPath = DigestPath(place) / DOCKER_TAGS_NAME;
    std::vector<std::string> tags;

    error = tagsPath.ReadLines(tags);
    if (error)
        return error;

    Tags = std::unordered_set<std::string>(tags.cbegin(), tags.cend());

    return OK;
}

TError TDockerImage::Save(const TPath &place) const {
    TError error;
    TPath imagePath = ImagePath(place);
    TPath digestPath = imagePath / Digest;
    TPath layersPath =  digestPath / DOCKER_LAYERS_NAME;

    error = LinkTag(place);
    if (error)
        return error;

    error = TPath(digestPath / "manifest.json").CreateAndWriteAll(Manifest);
    if (error)
        return error;

    error = TPath(digestPath / "config.json").CreateAndWriteAll(Config);
    if (error)
        return error;

    error = SaveTags(place);
    if (error)
        return error;

    if (!layersPath.Exists()) {
        error = layersPath.Mkdir(0755);
        if (error)
            return error;
    }

    for (const auto &layer: Layers) {
        error = TPath(layersPath / layer.Digest).Hardlink(layer.ArchivePath(place));
        if (error)
            return error;
    }

    return OK;
}

TError TDockerImage::Load(const TPath &place) {
    TError error;
    TPath digestPath = DigestPath(place);

    if (!digestPath.Exists()) {
        SchemaVersion = 1;
        digestPath = DigestPath(place);
        if (!digestPath.Exists())
            return TError(EError::DockerImageNotFound, FullName());
    }

    error = LoadTags(place);
    if (error)
        return error;

    if (!Digest.empty() && Tags.size() == 1)
        Tag = *Tags.begin();

    if (Manifest.empty()) {
        error = TPath(digestPath / "manifest.json").ReadAll(Manifest, 1 << 30);
        if (error)
            return error;
    }

    if (Config.empty()) {
        error = TPath(digestPath / "config.json").ReadAll(Config, 1 << 30);
        if (error)
            return error;
    }

    error = ParseManifest();
    if (error)
        return error;

    error = ParseConfig();
    if (error)
        return error;

    return OK;
}

TPath TDockerImage::TLayer::LayerPath(const TPath &place) const {
    return place / PORTO_DOCKER_LAYERS / "blobs" / Digest.substr(0, 2) / Digest;
}

TPath TDockerImage::TLayer::ArchivePath(const TPath &place) const {
    return LayerPath(place) / (Digest + ".tar.gz");
}

TError TDockerImage::TLayer::Remove(const TPath &place) const {
    TError error;
    TPath archivePath = ArchivePath(place);
    TStorage portoLayer;
    struct stat st;

    // refcount check
    if (!archivePath.Exists())
        return TError(EError::Docker, "Path {} doesn't exist", archivePath);

    error = archivePath.StatFollow(st);
    if (error)
        return error;

    if (st.st_nlink > 1)
        return OK;

    // layer removing
    error = archivePath.Unlink();
    if (error)
        return error;

    error = portoLayer.Resolve(EStorageType::DockerLayer, place, Digest);
    if (error)
        return error;

    if (!portoLayer.Exists())
        return TError(EError::Docker, "Path {} doesn't exist", portoLayer.Path);

    error = portoLayer.Remove();
    if (error)
        return error;

    error = LayerPath(place).ClearEmptyDirectories(place / PORTO_DOCKER_LAYERS);
    if (error)
        return error;

    return OK;
}

void TDockerImage::RemoveLayers(const TPath &place) const {
    TError error;

    for (const auto &layer: Layers) {
        error = layer.Remove(place);
        if (error)
            L_WRN("Cannot remove layer: {}", error);
    }
}

TError TDockerImage::Remove(const TPath &place, bool needLock) {
    TError error;
    auto lock = needLock ? Lock(place) : std::vector<std::unique_ptr<TFileMutex>>();
    TPath imagePath = ImagePath(place);
    bool tagSpecified = Digest.empty();

    error = Load(place);
    if (error)
        return error;

    if (Tags.size() > 1) {
        if (!tagSpecified)
            return TError(EError::Docker, "Cannot remove image {}: image is used by multiple tags", FullName());
        // delete only tag
        error = TPath(imagePath / DOCKER_TAGS_NAME / Tag).Unlink();
        if (error)
            return TError(EError::Docker, "Cannot remove tag {}", FullName());

        Tags.erase(Tag);
        error = SaveTags(place);
        if (error)
            return error;

        return OK;
    }

    // delete digest
    error = TPath(imagePath / Digest).RemoveAll();
    if (error)
        return error;

    for (const auto &tag: Tags) {
        error = TPath(imagePath / DOCKER_TAGS_NAME / tag).Unlink();
        if (error) {
            L_WRN("Cannot unlink tag: {}", imagePath / DOCKER_TAGS_NAME / tag);
            continue;
        }
    }

    error = TPath(imagePath / DOCKER_TAGS_NAME).ClearEmptyDirectories(place / PORTO_DOCKER_IMAGES);
    if (error)
        return error;

    RemoveLayers(place);

    return OK;
}

TError TDockerImage::DownloadLayers(const TPath &place) const {
    TError error;

    for (const auto &layer: Layers) {
        TPath layerPath = layer.LayerPath(place);
        TPath archivePath = layer.ArchivePath(place);

        if (archivePath.Exists()) {
            struct stat st;
            error = archivePath.StatStrict(st);
            if (error)
                return error;
            if ((size_t)st.st_size == layer.Size)
                continue;

            (void)layer.Remove(place);
        }

        error = DownloadFile(fmt::format("https://{}/v2/{}/{}/blobs/sha256:{}", Registry, Repository, Name, layer.Digest), archivePath, { "Authorization: " + AuthToken });
        if (error)
            return error;

        TStorage portoLayer;
        error = portoLayer.Resolve(EStorageType::DockerLayer, place, layer.Digest);
        if (error)
            return error;

        error = portoLayer.ImportArchive(archivePath, PORTO_HELPERS_CGROUP);
        if (error)
            return error;

        error = TStorage::SanitizeLayer(portoLayer.Path);
        if (error)
            return error;
    }

    return OK;
}

TError TDockerImage::Download(const TPath &place) {
    TError error;

    if (AuthToken.empty())
        return TError(EError::Docker, "Auth token is empty");

    THttpClient client("https://" + Registry);

    auto cleanup = [this, &place](const TError &error) -> TError {
        this->Remove(place, false);
        return error;
    };

    error = DownloadManifest(client);
    if (error)
        return cleanup(error);

    error = ParseManifest();
    if (error)
        return cleanup(error);

    error = DownloadConfig(client);
    if (error)
        return cleanup(error);

    error = ParseConfig();
    if (error)
        return cleanup(error);

    auto lock = Lock(place);

    if (TPath(ImagePath(place) / Digest).Exists()) {
        // image already exists and we check its tags
        error = LoadTags(place);
        if (error)
            return error;

        if (Tags.find(Tag) == Tags.end()) {
            // it's a new tag
            error = LinkTag(place);
            if (error)
                return error;

            Tags.emplace(Tag);
            error = SaveTags(place);
            if (error)
                return error;
        }

        return OK;
    }

    error = DownloadLayers(place);
    if (error)
        return cleanup(error);

    Tags.emplace(Tag);
    error = Save(place);
    if (error)
        return cleanup(error);

    return OK;
}

TError TDockerImage::InitStorage(const TPath &place, unsigned perms) {
    TError error;

    error = TPath(place / PORTO_DOCKER_IMAGES).MkdirAll(perms);
    if (error)
        return error;

    error = TPath(place / PORTO_DOCKER_LAYERS).MkdirAll(perms);
    if (error)
        return error;

    return OK;
}

TError TDockerImage::List(const TPath &place, std::vector<TDockerImage> &images, const std::string &mask) {
    TError error;
    TPath schemaVersionsPath = place / PORTO_DOCKER_IMAGES;
    std::vector<std::string> schemaVersions;

    error = schemaVersionsPath.ListSubdirs(schemaVersions);
    if (error)
        return error;

    for (const auto &schemaVersionString: schemaVersions) {
        int schemaVersion;
        error = StringToInt(schemaVersionString.substr(1), schemaVersion);
        if (error)
            return error;

        TPath registriesPath = place / PORTO_DOCKER_IMAGES / schemaVersionString;
        std::vector<std::string> registries;

        error = registriesPath.ListSubdirs(registries);
        if (error)
            return error;

        for (const auto &registry: registries) {
            TPath registryPath = registriesPath / registry;
            std::vector<std::string> repositories;

            error = registryPath.ListSubdirs(repositories);
            if (error)
                return error;

            for (const auto &repository: repositories) {
                TPath repositoryPath = registryPath / repository;
                std::vector<std::string> names;

                error = repositoryPath.ListSubdirs(names);
                if (error)
                    return error;

                for (const auto &name: names) {
                    TPath imagePath = repositoryPath / name;
                    std::vector<std::string> digests;

                    error = imagePath.ListSubdirs(digests);
                    if (error)
                        return error;

                    for (const auto &digest: digests) {
                        if (digest == DOCKER_TAGS_NAME)
                            continue;

                        TDockerImage image(registry, repository, name, digest, schemaVersion);
                        auto lock = image.Lock(place);
                        error = image.Load(place);
                        if (error)
                            return error;

                        if (!mask.empty() && !StringMatch(image.FullName(), mask, false, false))
                            continue;

                        images.emplace_back(image);
                    }
                }
            }
        }
    }

    return OK;
}
