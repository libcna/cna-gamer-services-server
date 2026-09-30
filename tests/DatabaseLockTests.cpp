// SPDX-License-Identifier: MIT
// One server process owns a database. Separate processes contend for the lock, one of them dies
// abnormally while holding it, and the system must hand the lock on. Dependency-free, so that the
// same test builds with MinGW and runs on Windows.
#include "CnaService/DatabaseLock.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#ifndef _WIN32
#include <sys/wait.h>
#endif

using CnaService::DatabaseLock;
namespace {
int checks=0;
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
std::string self;
// Runs this program in another process with a role; returns its exit code (-1 if it died by a signal).
int run(const std::string& role,const std::string& database) {
    const auto command="\""+self+"\" "+role+" \""+database+"\"";
#ifdef _WIN32
    return std::system(("\""+command+"\"").c_str());
#else
    const int status=std::system(command.c_str());
    return WIFEXITED(status)?WEXITSTATUS(status):-1;
#endif
}
}
int main(int argc,char** argv) {
    self=argv[0];
    if(argc==3) {
        const std::string role=argv[1];
        DatabaseLock lock(argv[2]);
        if(role=="--try")return lock.result()==DatabaseLock::Result::Owned?0:lock.result()==DatabaseLock::Result::InUse?2:3;
        // Takes the lock and dies without any cleanup, as a crashed server would.
        if(role=="--crash"&&lock.result()==DatabaseLock::Result::Owned)std::_Exit(9);
        return 4;
    }
    try {
        const auto database=(std::filesystem::current_path()/"database-lock-test.sqlite3").string();
        std::filesystem::remove(database+".lock");
        {
            DatabaseLock owner(database);
            check(owner.result()==DatabaseLock::Result::Owned,"the first server owns the database");
            check(run("--try",database)==2,"a second server process is refused");
            DatabaseLock again(database);
            check(again.result()==DatabaseLock::Result::InUse,"a second owner in the same process is refused");
        }
        check(run("--try",database)==0,"a released database is free");
        check(run("--crash",database)!=0,"the holder crashed");
        check(run("--try",database)==0,"a crashed owner's lock is released by the system");
        const auto missing=(std::filesystem::current_path()/"no-such-directory"/"db.sqlite3").string();
        check(DatabaseLock(missing).result()==DatabaseLock::Result::Failed,"an unopenable lock file fails, not owns");
        std::filesystem::remove(database+".lock");
        std::cout<<checks<<" database lock checks passed\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
