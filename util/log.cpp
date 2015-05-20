#include <iostream>
#include <sstream>

#include "log.hpp"
#include "util/unix.hpp"
#include "util/file.hpp"

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
}

static TLogBuf logBuf(1024);
static std::ostream logStream(&logBuf);

void TLogger::OpenLog(bool std, const TPath &path, const unsigned int mode) {
    if (std) {
        // because in task.cpp we expect that nothing should be in 0-2 fd,
        // we need to duplicate our std log somewhere else
        logBuf.SetFd(dup(STDOUT_FILENO));
    } else {
        logBuf.Open(path, mode);
    }
}

void TLogger::DisableLog() {
    TLogger::CloseLog();
    logBuf.SetFd(-1);
}

int TLogger::GetFd() {
    return logBuf.GetFd();
}

void TLogger::CloseLog() {
    int fd = logBuf.GetFd();
    if (fd > 2)
        close(fd);
    logBuf.SetFd(STDOUT_FILENO);
}

static std::string GetTime() {
    char tmstr[256];
    time_t t;
    struct tm *tmp;
    t = time(NULL);
    tmp = localtime(&t);

    if (tmp && strftime(tmstr, sizeof(tmstr), "%F %T", tmp))
        return std::string(tmstr);

    return std::string();
}

TLogBuf::TLogBuf(const size_t size) {
    Data.reserve(size);
    char *base = static_cast<char *>(Data.data());
    setp(base, base + Data.capacity() - 1);
}

void TLogBuf::Open(const TPath &path, const unsigned int mode) {
    if (!path.DirName().AccessOk(EFileAccess::Write)) {
        Fd = open("/dev/kmsg", O_WRONLY | O_APPEND | O_CLOEXEC);
        if (Fd < 0)
            Fd = STDERR_FILENO;

        return;
    }

    bool needCreate = false;

    if (path.Exists()) {
        if (path.GetType() != EFileType::Regular ||
            path.GetMode() != mode) {

            TFile f(path);
            (void)f.Remove();
            needCreate = true;
        }
    } else {
        needCreate = true;
    }

    if (needCreate) {
        TFile f(path, mode);
        (void)f.Touch();
    }

    Fd = open(path.ToString().c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
    if (Fd < 0)
        Fd = STDERR_FILENO;
}

int TLogBuf::sync() {
    std::ptrdiff_t n = pptr() - pbase();
    pbump(-n);

    int ret = write(Fd, pbase(), n);
    return (ret == n) ? 0 : -1;
}

TLogBuf::int_type TLogBuf::overflow(int_type ch) {
    if (ch != traits_type::eof()) {
        if (sync() < 0)
            return traits_type::eof();

        PORTO_ASSERT(std::less_equal<char *>()(pptr(), epptr()));
        *pptr() = ch;
        pbump(1);

        return ch;
    }

    return traits_type::eof();
}

std::basic_ostream<char> &TLogger::Log(ELogLevel level) {
    static const std::string prefix[] = { "    ",
                                          "WRN ",
                                          "ERR ",
                                          "EVT ",
                                          "ACT ",
                                          "REQ ",
                                          "RSP ",
                                          "SYS " };
    std::string name = GetProcessName();

#ifdef PORTOD
    if (level == LOG_WARN)
        Statistics->Warns++;
    else if (level == LOG_ERROR)
        Statistics->Errors++;
#endif

    return logStream << GetTime() << " " << name << ": " << prefix[level];
}

std::string RequestAsString(const rpc::TContainerRequest &req) {
    if (req.has_create())
        return "create " + req.create().name();
    else if (req.has_destroy())
        return "destroy " + req.destroy().name();
    else if (req.has_list())
        return "list containers";
    else if (req.has_getproperty())
        return "pget "  + req.getproperty().name() + " " + req.getproperty().property();
    else if (req.has_setproperty())
        return "pset " + req.setproperty().name() + " " + req.getproperty().property();
    else if (req.has_getdata())
        return "dget " + req.getdata().name() + " " + req.getdata().data();
    else if (req.has_start())
        return "start " + req.start().name();
    else if (req.has_stop())
        return "stop " + req.stop().name();
    else if (req.has_pause())
        return "pause " + req.pause().name();
    else if (req.has_resume())
        return "resume " + req.resume().name();
    else if (req.has_propertylist())
        return "list available properties";
    else if (req.has_datalist())
        return "list available data";
    else if (req.has_kill())
        return "kill " + req.kill().name() + " " + std::to_string(req.kill().sig());
    else if (req.has_version())
        return "get version";
    else if (req.has_createvolume())
        return "volumeAPI: create " + req.createvolume().path() + " " +
            req.createvolume().source() + " " +
            req.createvolume().quota() + " " +
            req.createvolume().flags();
    else if (req.has_destroyvolume())
        return "volumeAPI: destroy " + req.destroyvolume().path();
    else if (req.has_listvolumes())
        return "volumeAPI: list volumes";
    else
        return req.ShortDebugString();
}

std::string ResponseAsString(const rpc::TContainerResponse &resp) {
    switch (resp.error()) {
    case EError::Success:
    {
        std::string ret;

        if (resp.has_list()) {
            for (int i = 0; i < resp.list().name_size(); i++)
                ret += resp.list().name(i) + " ";
        } else if (resp.has_propertylist()) {
            for (int i = 0; i < resp.propertylist().list_size(); i++)
                ret += resp.propertylist().list(i).name()
                    + " (" + resp.propertylist().list(i).desc() + ")";
        } else if (resp.has_datalist()) {
            for (int i = 0; i < resp.datalist().list_size(); i++)
                ret += resp.datalist().list(i).name()
                    + " (" + resp.datalist().list(i).desc() + ")";
        } else if (resp.has_volumelist()) {
            for (int i = 0; i < resp.list().name_size(); i++)
                ret += resp.volumelist().list(i).path() + " (" +
                    resp.volumelist().list(i).source() + " " +
                    resp.volumelist().list(i).quota() + " " +
                    resp.volumelist().list(i).flags() + " " +
                    std::to_string(resp.volumelist().list(i).used()) + " " +
                    std::to_string(resp.volumelist().list(i).avail()) + ") ";

        } else if (resp.has_getproperty()) {
            ret = resp.getproperty().value();
        } else if (resp.has_getdata()) {
            ret = resp.getdata().value();
        } else if (resp.has_version())
            ret = resp.version().tag() + " #" + resp.version().revision();
        else
            ret = "Ok";
        return ret;
        break;
    }
    case EError::Unknown:
        return "Error: Unknown (" + resp.errormsg() + ")";
        break;
    case EError::InvalidMethod:
        return "Error: InvalidMethod (" + resp.errormsg() + ")";
        break;
    case EError::ContainerAlreadyExists:
        return "Error: ContainerAlreadyExists (" + resp.errormsg() + ")";
        break;
    case EError::ContainerDoesNotExist:
        return "Error: ContainerDoesNotExist (" + resp.errormsg() + ")";
        break;
    case EError::InvalidProperty:
        return "Error: InvalidProperty (" + resp.errormsg() + ")";
        break;
    case EError::InvalidData:
        return "Error: InvalidData (" + resp.errormsg() + ")";
        break;
    case EError::InvalidValue:
        return "Error: InvalidValue (" + resp.errormsg() + ")";
        break;
    case EError::InvalidState:
        return "Error: InvalidState (" + resp.errormsg() + ")";
        break;
    case EError::NotSupported:
        return "Error: NotSupported (" + resp.errormsg() + ")";
        break;
    case EError::ResourceNotAvailable:
        return "Error: ResourceNotAvailable (" + resp.errormsg() + ")";
        break;
    case EError::Permission:
        return "Error: Permission (" + resp.errormsg() + ")";
        break;
    case EError::VolumeAlreadyExists:
        return "Error: VolumeAlreadyExists (" + resp.errormsg() + ")";
        break;
    case EError::VolumeDoesNotExist:
        return "Error: VolumeDoesNotExist (" + resp.errormsg() + ")";
        break;
    case EError::NoSpace:
        return "Error: NoSpace (" + resp.errormsg() + ")";
        break;
    default:
        return resp.ShortDebugString();
        break;
    };
}
