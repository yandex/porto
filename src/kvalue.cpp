#include "kvalue.hpp"
#include "config.hpp"
#include "kv.pb.h"
#include "util/log.hpp"

#include <google/protobuf/io/coded_stream.h>

extern "C" {
#include <fcntl.h>
#include <unistd.h>
}

TError TKeyValue::Load() {
    std::string buf;
    kv::TNode node;
    TError error;

    error = Path.ReadAll(buf, config().keyvalue_limit());
    if (error)
        return error;

    ssize_t size = buf.size();
    google::protobuf::io::CodedInputStream input((uint8_t *)&buf[0], size);

    while (size) {
        uint32_t len;

        if (!input.ReadVarint32(&len))
            return TError("KeyValue: corrupted storage");

        size -= google::protobuf::io::CodedOutputStream::VarintSize32(len);
        size -= len;

        node.Clear();
        auto limit = input.PushLimit(len);
        if (!node.ParseFromCodedStream(&input))
            return TError("KeyValue: cannot parse record");
        if (!input.ConsumedEntireMessage())
            return TError("KeyValue: corrupted record");
        input.PopLimit(limit);

        for (const auto &pair: node.pairs())
            Data[pair.key()] = pair.val();
    }

    return OK;
}

TError TKeyValue::Save() {
    std::string buf;
    kv::TNode node;
    TError error;

    for (const auto &pair: Data) {
        auto kv = node.add_pairs();
        kv->set_key(pair.first);
        kv->set_val(pair.second);
    }

    uint32_t len = node.ByteSize();
    size_t lenLen = google::protobuf::io::CodedOutputStream::VarintSize32(len);

    if (len + lenLen > config().keyvalue_limit())
        return TError("KeyValue: object too big");

    buf.resize(len + lenLen);

    google::protobuf::io::CodedOutputStream::WriteVarint32ToArray(len, (uint8_t *)&buf[0]);
    if (!node.SerializeToArray((uint8_t *)&buf[lenLen], len))
        return TError("KeyValue: cannot serialize");

    TPath tmpPath(Path.ToString() + ".tmp");
    error = tmpPath.Mkfile(0640);
    if (!error)
        error = tmpPath.Chown(RootUser, PortoGroup);
    if (!error)
        error = tmpPath.WriteAll(buf);
    if (!error)
        error = tmpPath.Rename(Path);

    if (error)
        (void)tmpPath.Unlink();

    return error;
}

TError TKeyValue::Mount(const TPath &root) {
    TError error;
    TMount mount;

    if (!root.IsDirectoryStrict()) {
        if (root.Exists())
            (void)root.Unlink();
        error = root.MkdirAll(0755);
        if (error)
            return error;
    }

    error = root.FindMount(mount);
    if (error || mount.Target != root) {
        error = root.Mount("tmpfs", "tmpfs",
                           MS_NOEXEC | MS_NOSUID | MS_NODEV,
                           { "size=" + std::to_string(config().keyvalue_size()),
                             "mode=0750",
                             "uid=" + std::to_string(RootUser),
                             "gid=" + std::to_string(PortoGroup) });
        if (error)
            return error;
    } else if (mount.Type != "tmpfs")
        return TError("KeyValue: found non-tmpfs mount at " + root.ToString());

    std::vector<std::string> names;
    error = root.ReadDirectory(names);
    if (!error) {
        for (auto &name : names) {
            if (StringEndsWith(name, ".tmp"))
                (void)(root / name).Unlink();
        }
    }

    return error;
}

TError TKeyValue::ListAll(const TPath &root, std::list<TKeyValue> &nodes) {
    std::vector<std::string> names;
    TError error = root.ReadDirectory(names);
    if (!error) {
        for (auto &name : names) {
            if (!StringEndsWith(name, ".tmp"))
                nodes.emplace_back(root / name);
        }
    }
    return error;
}

void TKeyValue::DumpAll(const TPath &root) {
    std::vector<std::string> names;
    TError error;

    error = root.ReadDirectory(names);
    if (error) {
        L("ERROR {}", error);
        return;
    }

    for (auto &name : names) {
        L("{}", name);
        if (StringEndsWith(name, ".tmp")) {
            L("SKIP");
            continue;
        }

        TKeyValue node(root / name);
        error = node.Load();
        if (error) {
            L("ERROR {}", error);
            continue;
        }

        for (auto &kv: node.Data)
            L("{} = {} ", kv.first, kv.second);
    }
}
