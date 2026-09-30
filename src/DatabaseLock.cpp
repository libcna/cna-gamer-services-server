// SPDX-License-Identifier: MIT
#include "CnaService/DatabaseLock.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>
#elif defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#else
#error "No single-owner database lock for this platform"
#endif

namespace CnaService {
#ifdef _WIN32
DatabaseLock::DatabaseLock(const std::string& database) {
    const auto path=database+".lock";
    const int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path.c_str(),-1,nullptr,0);
    if(length<=0)return;
    std::vector<wchar_t> wide(static_cast<std::size_t>(length));
    if(MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,path.c_str(),-1,wide.data(),length)!=length)return;
    // No sharing: while this handle is open nobody else can open the file, and Windows closes it
    // when the process ends, crash included.
    const auto handle=CreateFileW(wide.data(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(handle==INVALID_HANDLE_VALUE) {
        const auto error=GetLastError();
        result_=error==ERROR_SHARING_VIOLATION||error==ERROR_LOCK_VIOLATION?Result::InUse:Result::Failed;
        return;
    }
    handle_=handle;result_=Result::Owned;
}
DatabaseLock::~DatabaseLock() {if(handle_)CloseHandle(static_cast<HANDLE>(handle_));}
#else
DatabaseLock::DatabaseLock(const std::string& database) {
    const auto path=database+".lock";
    descriptor_=::open(path.c_str(),O_RDWR|O_CREAT|O_CLOEXEC,0600);
    if(descriptor_<0)return;
    // flock belongs to the open file, so a second open of the same file conflicts even within one
    // process; the kernel drops it when the last descriptor closes, crash included.
    if(::flock(descriptor_,LOCK_EX|LOCK_NB)!=0){::close(descriptor_);descriptor_=-1;result_=Result::InUse;return;}
    result_=Result::Owned;
}
DatabaseLock::~DatabaseLock() {if(descriptor_>=0)::close(descriptor_);}
#endif
}
