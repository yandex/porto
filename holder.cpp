#include <algorithm>

#include "holder.hpp"
#include "config.hpp"
#include "container.hpp"
#include "property.hpp"
#include "data.hpp"
#include "event.hpp"
#include "util/string.hpp"
#include "util/cred.hpp"

static void ParseUserConf(const ::google::protobuf::RepeatedPtrField<std::string> &source,
                          std::set<int> &target) {
    for (auto &val : source) {
        TUser u(val);
        TError error = u.Load();
        if (error) {
            L_WRN() << "Can't add privileged user: " << error << std::endl;
            continue;
        }

        target.insert(u.GetId());
    }
}

static void ParseGroupConf(const ::google::protobuf::RepeatedPtrField<std::string> &source,
                          std::set<int> &target) {
    for (auto &val : source) {
        TGroup g(val);
        TError error = g.Load();
        if (error) {
            L_WRN() << "Can't add privileged group: " << error << std::endl;
            continue;
        }

        target.insert(g.GetId());
    }
}

TError TCredAdmin::Initialize() {
    ParseUserConf(config().privileges().root_user(), PrivilegedUid);
    ParseGroupConf(config().privileges().root_group(), PrivilegedGid);

    ParseUserConf(config().privileges().restricted_root_user(), RestrictedRootUid);
    ParseGroupConf(config().privileges().restricted_root_group(), RestrictedRootGid);

    return TError::Success();
}

bool TCredAdmin::PrivilegedUser(const TCred &cred) {
    if (cred.IsRoot())
        return true;

    if (PrivilegedUid.find(cred.Uid) != PrivilegedUid.end())
        return true;

    if (PrivilegedGid.find(cred.Gid) != PrivilegedGid.end())
        return true;

    return false;
}

bool TCredAdmin::RestrictedUser(const TCred &cred) {
    if (RestrictedRootUid.find(cred.Uid) != RestrictedRootUid.end())
        return true;

    if (RestrictedRootGid.find(cred.Gid) != RestrictedRootGid.end())
        return true;

    return false;
}

THolder::~THolder() {
    // we want children to be removed first
    while (Containers.begin() != Containers.end()) {
        auto name = Containers.begin()->first;
        TError error = _Destroy(name);
        if (error)
            L_ERR() << "Can't destroy container " << name << ": " << error << std::endl;
    }
}

TError THolder::CreateRoot() {
    TError error = EpollCreate(Epfd);
    if (error)
        return error;

    error = RegisterProperties();
    if (error)
        return error;

    error = RegisterData();
    if (error)
        return error;

    error = TaskGetLastCap();
    if (error)
        return error;

    error = Initialize();
    if (error)
        return error;

    // we are using single kvalue store for both properties and data
    // so make sure names don't clash
    std::string overlap = propertySet.Overlap(dataSet);
    if (overlap.length())
        return TError(EError::Unknown, "Data and property names conflict: " + overlap);

    BootTime = GetBootTime();

    error = Create(ROOT_CONTAINER, TCred(0, 0));
    if (error)
        return error;

    uint16_t id;
    error = IdMap.Get(id);
    if (error)
        return error;

    if (id != 2)
        return TError(EError::Unknown, "Unexpected root container id");

    auto root = Get(ROOT_CONTAINER);
    error = root->Start();
    if (error)
        return error;

    ScheduleLogRotatation();
    Statistics->Created = 0;
    Statistics->RestoreFailed = 0;
    Statistics->RemoveDead = 0;
    Statistics->Rotated = 0;

    return TError::Success();
}

bool THolder::ValidName(const std::string &name) const {
    if (name == ROOT_CONTAINER)
        return true;

    if (name.length() == 0 || name.length() > 128)
        return false;

    for (std::string::size_type i = 0; i + 1 < name.length(); i++)
        if (name[i] == '/' && name[i + 1] == '/')
            return false;

    if (*name.begin() == '/')
        return false;

    if (*(name.end()--) == '/')
        return false;

    // . (dot) is used for kvstorage, so don't allow it here
    return find_if(name.begin(), name.end(),
                   [](const char c) -> bool {
                        return !(isalnum(c) || c == '_' || c == '/' || c == '-' || c == '@' || c == ':' || c == '.');
                   }) == name.end();
}

std::shared_ptr<TContainer> THolder::GetParent(const std::string &name) const {
    std::shared_ptr<TContainer> parent;

    std::string::size_type n = name.rfind('/');
    if (n == std::string::npos) {
        return Containers.at(ROOT_CONTAINER);
    } else {
        std::string parentName = name.substr(0, n);

        if (Containers.find(parentName) == Containers.end())
            return nullptr;

        return Containers.at(parentName);
    }
}

TError THolder::Create(const std::string &name, const TCred &cred) {
    if (!ValidName(name))
        return TError(EError::InvalidValue, "invalid container name " + name);

    if (Containers.find(name) != Containers.end())
        return TError(EError::ContainerAlreadyExists, "container " + name + " already exists");

    if (Containers.size() + 1 > config().container().max_total())
        return TError(EError::ResourceNotAvailable, "number of created containers exceeds limit");

    auto parent = GetParent(name);
    if (!parent && name != ROOT_CONTAINER)
        return TError(EError::InvalidValue, "invalid parent container");

    uint16_t id;
    TError error = IdMap.Get(id);
    if (error)
        return error;

    auto c = std::make_shared<TContainer>(this, name, parent, id, Net);
    error = c->Create(cred);
    if (error)
        return error;

    Containers[name] = c;
    Statistics->Created++;
    return TError::Success();
}

std::shared_ptr<TContainer> THolder::Get(const std::string &name) {
    if (Containers.find(name) == Containers.end())
        return nullptr;

    return Containers[name];
}

TError THolder::_Destroy(const std::string &name) {
    auto c = Containers[name];
    for (auto child: c->GetChildren())
        _Destroy(child);

    IdMap.Put(c->GetId());
    c->Destroy();
    Containers.erase(name);
    Statistics->Created--;

    return TError::Success();
}

TError THolder::Destroy(const std::string &name) {
    if (name == ROOT_CONTAINER || Containers.find(name) == Containers.end())
        return TError(EError::InvalidValue, "invalid container name " + name);

    return _Destroy(name);
}

std::vector<std::string> THolder::List() const {
    std::vector<std::string> ret;

    for (auto c : Containers) {
        PORTO_ASSERT(c.first == c.second->GetName());
        ret.push_back(c.first);
    }

    return ret;
}

TError THolder::RestoreId(const kv::TNode &node, uint16_t &id) {
    std::string value = "";
    for (int i = 0; i < node.pairs_size(); i++) {
        auto key = node.pairs(i).key();

        if (key == P_RAW_ID)
            value = node.pairs(i).val();
    }

    if (value.length() == 0) {
        TError error = IdMap.Get(id);
        if (error)
            return error;
    } else {
        uint32_t id32;
        TError error = StringToUint32(value, id32);
        if (error)
            return error;

        id = (uint16_t)id32;
    }

    return TError::Success();
}

TError THolder::Restore(const std::string &name, const kv::TNode &node) {
    if (name == ROOT_CONTAINER)
        return TError::Success();

    auto parent = GetParent(name);
    if (!parent)
        return TError(EError::InvalidValue, "invalid parent container");

    uint16_t id = 0;
    TError error = RestoreId(node, id);
    if (error)
        return error;

    if (!id)
        return TError(EError::Unknown, "Couldn't restore container id");

    auto c = std::make_shared<TContainer>(this, name, parent, id, Net);
    error = c->Restore(node);
    if (error) {
        L_ERR() << "Can't restore container " << name << ": " << error << std::endl;
        return error;
    }

    Containers[name] = c;
    Statistics->Created++;
    return TError::Success();
}

void THolder::ScheduleLogRotatation() {
    TEvent e(EEventType::RotateLogs);
    Queue->Add(config().daemon().rotate_logs_timeout_s() * 1000, e);
}

bool THolder::DeliverEvent(const TEvent &event) {
    if (config().log().verbose())
        L() << "Deliver event " << event.GetMsg() << std::endl;

    if (event.Targeted) {
        auto c = event.Container.lock();
        if (c)
            return c->DeliverEvent(event);
        return false;
    } else {
        std::vector<std::string> remove;
        for (auto c : Containers) {
            if (event.Type == EEventType::RotateLogs)
                if (c.second->CanRemoveDead())
                    remove.push_back(c.second->GetName());

            if (c.second->DeliverEvent(event))
                return true;
        }

        for (auto name : remove) {
            L() << "Remove old dead " << name << std::endl;
            TError error = Destroy(name);
            if (error)
                L_ERR() << "Can't destroy " << name << ": " << error << std::endl;
            Statistics->RemoveDead++;
        }
    }

    if (event.Type == EEventType::RotateLogs) {
        if (config().log().verbose())
            TLogger::Log() << "Rotated logs " << std::endl;

        ScheduleLogRotatation();
        Statistics->Rotated++;
        return true;
    } else {
        L() << "Couldn't deliver " << event.GetMsg() << std::endl;
        return false;
    }
}
