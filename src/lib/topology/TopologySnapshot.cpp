#include "TopologySnapshot.h"
#include "ByteCodec.h"
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>
#include <vector>
#include <cerrno>
#include <limits>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <sddl.h>
#include <aclapi.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif
#endif

namespace weave { namespace topology {
bool operator==(const ProcessIdentity& a, const ProcessIdentity& b) {
    return a.pid == b.pid && a.birth == b.birth && a.owner == b.owner &&
           a.session == b.session;
}
std::uint64_t monotonicMillis() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
std::uint64_t newRunId() {
    std::random_device rng;
    std::uint64_t n = (std::uint64_t(rng()) << 32) ^ rng();
    return n ? n : 1;
}
namespace {
void putIdentity(std::string& b, const ProcessIdentity& p) {
    detail::number(b,p.pid,8); detail::number(b,p.birth,8);
    detail::string(b,p.owner); detail::number(b,p.session,4);
}
ProcessIdentity getIdentity(detail::Reader& r) {
    ProcessIdentity p;
    p.pid=r.number(8); p.birth=r.number(8); p.owner=r.string(256);
    p.session=static_cast<std::uint32_t>(r.number(4)); return p;
}
bool validIdentity(const ProcessIdentity& p) {
    return p.pid && p.birth && p.pid <= std::numeric_limits<std::uint32_t>::max() &&
        validName(p.owner);
}
}
bool encodeSnapshot(const Snapshot& s, std::string& out) {
    if (!validIdentity(s.identity) || !s.run || !s.sequence ||
        !validName(s.localName) || unsigned(s.localRole) > unsigned(Role::Client)) return false;
    std::string topology;
    if (!s.topology.nodes.empty() && !encodeTopology(s.topology,topology)) return false;
    std::string b("WTS1"); putIdentity(b,s.identity);
    detail::number(b,s.run,8); detail::number(b,s.sequence,8);
    detail::number(b,s.updatedMillis,8); detail::string(b,s.localName);
    detail::number(b,unsigned(s.localRole),1); detail::number(b,s.ready,1);
    detail::number(b,topology.size(),4); b += topology;
    if (b.size() > MaxBytes) return false;
    out = std::move(b); return true;
}
bool decodeSnapshot(const std::string& b, Snapshot& out) {
    if (b.size() > MaxBytes || b.size() < 4 || b.compare(0,4,"WTS1")) return false;
    detail::Reader r{b}; r.p=4; Snapshot s; s.identity=getIdentity(r);
    s.run=r.number(8); s.sequence=r.number(8); s.updatedMillis=r.number(8);
    s.localName=r.string(MaxNameBytes); s.localRole=Role(r.number(1));
    const auto ready=r.number(1); if (ready>1) return false; s.ready=ready;
    const auto count=r.number(4);
    if (!r.good || count != b.size()-r.p) return false;
    if (count && !decodeTopology(b.substr(r.p),s.topology)) return false;
    std::string checked;
    if (!encodeSnapshot(s,checked)) return false;
    out=std::move(s); return true;
}
ReadStatus classifySnapshot(const Snapshot& s,const ProcessIdentity& p,std::uint64_t now) {
    if (!(s.identity==p)) return ReadStatus::WrongOwner;
    if (now<s.updatedMillis || now-s.updatedMillis>FreshnessMillis) return ReadStatus::Stale;
    if (!s.ready || s.topology.nodes.empty()) return ReadStatus::NotReady;
    return ReadStatus::Fresh;
}
std::string snapshotFilename(const ProcessIdentity& p) {
    return std::to_string(p.pid)+"-"+std::to_string(p.birth)+".snapshot";
}

#ifndef _WIN32
bool processIdentity(std::uint64_t pid, ProcessIdentity& out) {
    if (!pid || pid > std::numeric_limits<pid_t>::max()) return false;
    ProcessIdentity p; p.pid=pid;
#ifdef __APPLE__
    int mib[4]={CTL_KERN,KERN_PROC,KERN_PROC_PID,static_cast<int>(pid)};
    kinfo_proc info{}; size_t size=sizeof(info);
    if (sysctl(mib,4,&info,&size,nullptr,0) || size != sizeof(info)) return false;
    p.birth=std::uint64_t(info.kp_proc.p_starttime.tv_sec)*1000000+
            info.kp_proc.p_starttime.tv_usec;
    p.owner=std::to_string(info.kp_eproc.e_ucred.cr_uid);
#else
    const std::string base="/proc/"+std::to_string(pid);
    struct stat st{}; if (stat(base.c_str(),&st)) return false;
    p.owner=std::to_string(st.st_uid);
    std::ifstream input(base+"/stat"); std::string line; std::getline(input,line);
    auto end=line.rfind(')'); if (end==std::string::npos) return false;
    std::istringstream fields(line.substr(end+2)); std::string field;
    for (int n=3;n<=22;++n) {
        if (!(fields>>field)) return false;
        if (n==3 && (field=="Z" || field=="X")) return false;
        if (n==22) { try { p.birth=std::stoull(field); } catch (...) { return false; } }
    }
#endif
    const auto session=getsid(static_cast<pid_t>(pid)); if (session<0) return false;
    p.session=static_cast<std::uint32_t>(session);
    if (!validIdentity(p)) return false;
    out=std::move(p); return true;
}
ProcessIdentity currentProcessIdentity() {
    ProcessIdentity p; processIdentity(getpid(),p); return p;
}
std::string privateSnapshotDirectory() {
    // Fixed system temporary root avoids trusting another process's environment.
    return "/tmp/weave-topology-"+std::to_string(geteuid());
}
namespace {
int openDirectory(const std::string& path,bool create) {
    if (create && mkdir(path.c_str(),0700) && errno!=EEXIST) return -1;
    int fd=open(path.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return -1;
    struct stat st{};
    if (fstat(fd,&st) || st.st_uid!=geteuid() || (st.st_mode&077)!=0) {
        close(fd); return -1;
    }
    return fd;
}
bool readPrivate(const std::string& directory,const std::string& name,std::string& out) {
    int dir=openDirectory(directory,false); if (dir<0) return false;
    int fd=openat(dir,name.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    close(dir); if (fd<0) return false;
    struct stat st{};
    if (fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_uid!=geteuid() ||
        st.st_nlink!=1 || (st.st_mode&077)!=0 || st.st_size<0 || st.st_size>MaxBytes) {
        close(fd); return false;
    }
    std::string b(static_cast<size_t>(st.st_size),'\0'); size_t p=0;
    while (p<b.size()) {
        auto n=read(fd,&b[p],b.size()-p);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) { close(fd); return false; } p+=n;
    }
    char extra; const auto tail=read(fd,&extra,1); close(fd);
    if (tail!=0) return false;
    out=std::move(b); return true;
}
bool writePrivate(const std::string& directory,const std::string& name,const std::string& b) {
    int dir=openDirectory(directory,true); if (dir<0) return false;
    const std::string temp=name+"."+std::to_string(newRunId())+".tmp";
    int fd=openat(dir,temp.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if (fd<0) { close(dir); return false; }
    size_t p=0;
    while (p<b.size()) {
        auto n=write(fd,b.data()+p,b.size()-p);
        if (n<0 && errno==EINTR) continue;
        if (n<=0) break; p+=n;
    }
    bool good=p==b.size();
    if (close(fd)!=0) good=false;
    if (good) good=renameat(dir,temp.c_str(),dir,name.c_str())==0;
    if (!good) unlinkat(dir,temp.c_str(),0);
    close(dir); return good;
}
void removePrivate(const std::string& directory,const std::string& name) {
    int dir=openDirectory(directory,false); if (dir<0) return;
    unlinkat(dir,name.c_str(),0); close(dir);
}
}
#else
namespace {
std::wstring wide(const std::string& s) {
    int n=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),nullptr,0);
    if (!n) return {};
    std::wstring w(n,L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,s.data(),int(s.size()),&w[0],n); return w;
}
std::string utf8(const wchar_t* w) {
    int n=WideCharToMultiByte(CP_UTF8,0,w,-1,nullptr,0,nullptr,nullptr);
    if (!n) return {}; std::string s(n,'\0');
    WideCharToMultiByte(CP_UTF8,0,w,-1,&s[0],n,nullptr,nullptr); s.pop_back(); return s;
}
std::string sidString(PSID sid) {
    LPSTR text=nullptr;
    if (!ConvertSidToStringSidA(sid,&text)) return {};
    std::string s(text); LocalFree(text); return s;
}
bool tokenIdentity(HANDLE process,std::string& owner) {
    HANDLE token=nullptr; if (!OpenProcessToken(process,TOKEN_QUERY,&token)) return false;
    DWORD n=0; GetTokenInformation(token,TokenUser,nullptr,0,&n);
    std::vector<char> b(n);
    const bool ok=n && GetTokenInformation(token,TokenUser,b.data(),n,&n);
    if (ok) owner=sidString(reinterpret_cast<TOKEN_USER*>(b.data())->User.Sid);
    CloseHandle(token); return ok && !owner.empty();
}
bool trustedSid(PSID sid,const std::string& owner,bool service) {
    const auto s=sidString(sid);
    return s=="S-1-5-18" || s=="S-1-5-32-544" || (!service && s==owner);
}
// Fail closed on NULL/unprotected DACLs and any untrusted write-capable ACE.
bool secureHandle(HANDLE h,const std::string& owner,bool service,bool directory) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(h,&info) ||
        (info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) ||
        bool(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=directory ||
        (!directory && info.nNumberOfLinks!=1)) return false;
    PSID sid=nullptr; PACL acl=nullptr; PSECURITY_DESCRIPTOR sd=nullptr;
    if (GetSecurityInfo(h,SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,
        &sid,nullptr,&acl,nullptr,&sd)!=ERROR_SUCCESS) return false;
    SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision=0;
    bool ok=acl && trustedSid(sid,owner,service) &&
        GetSecurityDescriptorControl(sd,&control,&revision) && (control&SE_DACL_PROTECTED);
    for (DWORD i=0;ok && i<acl->AceCount;++i) {
        void* raw=nullptr; if (!GetAce(acl,i,&raw)) { ok=false; break; }
        auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);
        if (ace->Header.AceType==ACCESS_DENIED_ACE_TYPE) continue;
        if (ace->Header.AceType!=ACCESS_ALLOWED_ACE_TYPE) { ok=false; break; }
        const DWORD writes=GENERIC_ALL|GENERIC_WRITE|WRITE_DAC|WRITE_OWNER|DELETE|
            FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_WRITE_EA|FILE_WRITE_ATTRIBUTES|FILE_DELETE_CHILD;
        if ((ace->Mask&writes) && !trustedSid(&ace->SidStart,owner,service)) ok=false;
    }
    LocalFree(sd); return ok;
}
HANDLE openDirectory(const std::string& path,bool create,bool service=false) {
    const auto owner=currentProcessIdentity().owner;
    if (create) {
        if (service && owner!="S-1-5-18") return INVALID_HANDLE_VALUE;
        const std::string acl=service ?
            "O:SYG:SYD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;AU)" :
            "D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;"+owner+")";
        PSECURITY_DESCRIPTOR sd=nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(acl.c_str(),SDDL_REVISION_1,&sd,nullptr))
            return INVALID_HANDLE_VALUE;
        SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE};
        CreateDirectoryW(wide(path).c_str(),&sa); LocalFree(sd);
    }
    HANDLE h=CreateFileW(wide(path).c_str(),READ_CONTROL|FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (h==INVALID_HANDLE_VALUE) return h;
    if (!secureHandle(h,owner,service,true)) { CloseHandle(h); return INVALID_HANDLE_VALUE; }
    return h;
}
bool readPrivate(const std::string& dir,const std::string& name,std::string& out,bool service=false) {
    HANDLE d=openDirectory(dir,false,service); if (d==INVALID_HANDLE_VALUE) return false;
    HANDLE f=CreateFileW(wide(dir+"/"+name).c_str(),GENERIC_READ|READ_CONTROL,
        FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    if (f==INVALID_HANDLE_VALUE) { CloseHandle(d); return false; }
    LARGE_INTEGER size{};
    bool ok=secureHandle(f,currentProcessIdentity().owner,service,false) &&
        GetFileSizeEx(f,&size) && size.QuadPart>=0 && size.QuadPart<=MaxBytes;
    std::string b; if (ok) b.resize(static_cast<size_t>(size.QuadPart));
    DWORD n=0; if (ok) ok=ReadFile(f,b.empty()?nullptr:&b[0],DWORD(b.size()),&n,nullptr) && n==b.size();
    CloseHandle(f); CloseHandle(d); if (ok) out=std::move(b); return ok;
}
bool writePrivate(const std::string& dir,const std::string& name,const std::string& b,bool service=false) {
    HANDLE d=openDirectory(dir,true,service); if (d==INVALID_HANDLE_VALUE) return false;
    const auto owner=currentProcessIdentity().owner;
    const std::string acl=service ?
        "O:SYG:SYD:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;AU)" :
        "D:P(A;;FA;;;SY)(A;;FA;;;"+owner+")";
    PSECURITY_DESCRIPTOR sd=nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(acl.c_str(),SDDL_REVISION_1,&sd,nullptr)) {
        CloseHandle(d); return false;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa),sd,FALSE};
    const auto temp=wide(dir+"/"+name+"."+std::to_string(newRunId())+".tmp");
    HANDLE f=CreateFileW(temp.c_str(),GENERIC_WRITE,0,&sa,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    LocalFree(sd); if (f==INVALID_HANDLE_VALUE) { CloseHandle(d); return false; }
    DWORD n=0; bool ok=WriteFile(f,b.data(),DWORD(b.size()),&n,nullptr) && n==b.size();
    CloseHandle(f);
    if (ok) ok=MoveFileExW(temp.c_str(),wide(dir+"/"+name).c_str(),MOVEFILE_REPLACE_EXISTING)!=FALSE;
    if (!ok) DeleteFileW(temp.c_str()); CloseHandle(d); return ok;
}
void removePrivate(const std::string& dir,const std::string& name,bool service=false) {
    if (service && currentProcessIdentity().owner!="S-1-5-18") return;
    HANDLE d=openDirectory(dir,false,service); if (d==INVALID_HANDLE_VALUE) return;
    DeleteFileW(wide(dir+"/"+name).c_str()); CloseHandle(d);
}
}
bool processIdentity(std::uint64_t pid, ProcessIdentity& out) {
    if (!pid || pid>MAXDWORD) return false;
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,DWORD(pid));
    if (!process) return false;
    FILETIME birth{},exit{},kernel{},user{}; DWORD code=0,session=0;
    ProcessIdentity p; p.pid=pid;
    bool ok=GetProcessTimes(process,&birth,&exit,&kernel,&user) &&
        GetExitCodeProcess(process,&code) && code==STILL_ACTIVE &&
        ProcessIdToSessionId(DWORD(pid),&session) && tokenIdentity(process,p.owner);
    p.birth=(std::uint64_t(birth.dwHighDateTime)<<32)|birth.dwLowDateTime;
    p.session=session; CloseHandle(process);
    if (!ok || !validIdentity(p)) return false; out=std::move(p); return true;
}
ProcessIdentity currentProcessIdentity() {
    ProcessIdentity p; processIdentity(GetCurrentProcessId(),p); return p;
}
std::string privateSnapshotDirectory() {
    wchar_t path[MAX_PATH]{};
    if (SHGetFolderPathW(nullptr,CSIDL_LOCAL_APPDATA,nullptr,SHGFP_TYPE_CURRENT,path)!=S_OK) return {};
    return utf8(path)+"/WeaveTopology";
}
#endif

ReadStatus readSnapshot(const std::string& directory,const ProcessIdentity& expected,
                        std::uint64_t now,Snapshot& out) {
    if (!validIdentity(expected)) return ReadStatus::WrongOwner;
    ProcessIdentity live;
    if (!processIdentity(expected.pid,live) || !(live==expected)) return ReadStatus::WrongOwner;
    std::string b;
    if (!readPrivate(directory,snapshotFilename(expected),b)) return ReadStatus::Missing;
    Snapshot s; if (!decodeSnapshot(b,s)) return ReadStatus::Invalid;
    const auto status=classifySnapshot(s,expected,now);
    if (status==ReadStatus::WrongOwner) return status;
    if (!processIdentity(expected.pid,live) || !(live==expected)) return ReadStatus::WrongOwner;
    out=std::move(s); return status;
}
struct SnapshotPublisher::State {
    std::string directory, name;
    Snapshot snapshot;
    std::mutex mutex;
    std::condition_variable changed;
    bool stop=false, dirty=false;
    std::thread worker;
    explicit State(const std::string& dir):directory(dir) {
        snapshot.identity=currentProcessIdentity(); snapshot.run=newRunId();
        name=snapshotFilename(snapshot.identity);
        worker=std::thread([this] {
            std::unique_lock<std::mutex> lock(mutex);
            while (!stop) {
                changed.wait_for(lock,std::chrono::seconds(2),[this]{return stop||dirty;});
                if (stop) break;
                dirty=false;
                snapshot.updatedMillis=monotonicMillis(); ++snapshot.sequence;
                Snapshot copy=snapshot;
                lock.unlock();
                std::string b;
                if (encodeSnapshot(copy,b)) writePrivate(directory,name,b);
                lock.lock();
            }
            lock.unlock(); removePrivate(directory,name);
        });
    }
    ~State() {
        { std::lock_guard<std::mutex> lock(mutex); stop=true; }
        changed.notify_one(); worker.join();
    }
};
SnapshotPublisher::SnapshotPublisher(const std::string& d):m_state(new State(d)) {}
SnapshotPublisher::~SnapshotPublisher() = default;
void SnapshotPublisher::update(const Topology& t,const std::string& name,Role role,bool ready) {
    if (!validName(name)) return;
    std::string validation;
    if (!t.nodes.empty() && !encodeTopology(t,validation)) return;
    { std::lock_guard<std::mutex> lock(m_state->mutex);
      m_state->snapshot.topology=t; m_state->snapshot.localName=name;
      m_state->snapshot.localRole=role; m_state->snapshot.ready=ready; m_state->dirty=true; }
    m_state->changed.notify_one();
}
bool matchesPublishedOwner(const PublishedOwner& p,const Snapshot& s,
    const std::string& owner,std::uint32_t session,std::uint64_t now) {
    return validIdentity(p.identity) && p.generation && p.identity==s.identity &&
        p.identity.owner==owner && p.identity.session==session &&
        now>=p.updatedMillis && now-p.updatedMillis<=FreshnessMillis;
}
std::string publishedOwnerDirectory() {
#ifdef _WIN32
    wchar_t path[MAX_PATH]{};
    if (SHGetFolderPathW(nullptr,CSIDL_COMMON_APPDATA,nullptr,SHGFP_TYPE_CURRENT,path)!=S_OK) return {};
    return utf8(path)+"/WeaveTopologyOwner";
#else
    return {};
#endif
}
bool publishOwner(const PublishedOwner& p) {
#ifdef _WIN32
    if (!validIdentity(p.identity) || !p.generation) return false;
    std::string b("WTO1"); putIdentity(b,p.identity);
    detail::number(b,p.generation,8); detail::number(b,p.updatedMillis,8);
    return writePrivate(publishedOwnerDirectory(),"published.owner",b,true);
#else
    (void)p; return false;
#endif
}
void retirePublishedOwner() {
#ifdef _WIN32
    removePrivate(publishedOwnerDirectory(),"published.owner",true);
#endif
}
ReadStatus readPublishedSnapshot(const std::string& owner,std::uint32_t session,
                                 std::uint64_t now,Snapshot& out) {
#ifdef _WIN32
    std::string first;
    if (!readPrivate(publishedOwnerDirectory(),"published.owner",first,true)) return ReadStatus::Missing;
    if (first.compare(0,4,"WTO1")) return ReadStatus::Invalid;
    detail::Reader r{first}; r.p=4; PublishedOwner p;
    p.identity=getIdentity(r); p.generation=r.number(8); p.updatedMillis=r.number(8);
    if (!r.done() || !validIdentity(p.identity)) return ReadStatus::Invalid;
    Snapshot s;
    const auto status=readSnapshot(privateSnapshotDirectory(),p.identity,now,s);
    if (status!=ReadStatus::Fresh && status!=ReadStatus::Stale && status!=ReadStatus::NotReady) return status;
    if (!matchesPublishedOwner(p,s,owner,session,now)) return ReadStatus::WrongOwner;
    std::string second;
    if (!readPrivate(publishedOwnerDirectory(),"published.owner",second,true) || first!=second)
        return ReadStatus::WrongOwner;
    s.ownerGeneration=p.generation; out=std::move(s); return status;
#else
    (void)owner; (void)session; (void)now; (void)out; return ReadStatus::Missing;
#endif
}
} }
