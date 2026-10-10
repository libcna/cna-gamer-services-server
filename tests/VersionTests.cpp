// SPDX-License-Identifier: MIT
// Release identity: the generated CnaService/Version.hpp must agree with the CMake project that
// generated it. The checks are structural -- nothing here names a concrete release -- so a version
// bump never has to edit this file (docs/releasing.md).
#include "CnaService/Version.hpp"
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef CNA_TEST_EXPECTED_VERSION
#error "CNA_TEST_EXPECTED_VERSION must be defined by CMakeLists.txt"
#endif

// The macros are usable by the preprocessor, which is why they exist beside the functions.
#if !(CNA_GAMER_SERVICES_VERSION_MAJOR >= 0 && CNA_GAMER_SERVICES_VERSION_MINOR >= 0 && CNA_GAMER_SERVICES_VERSION_PATCH >= 0)
#error "CNA_GAMER_SERVICES_VERSION_* must be preprocessor-testable integer literals"
#endif

static_assert(CnaService::getVersionMajor()==CNA_GAMER_SERVICES_VERSION_MAJOR);
static_assert(CnaService::getVersionMinor()==CNA_GAMER_SERVICES_VERSION_MINOR);
static_assert(CnaService::getVersionPatch()==CNA_GAMER_SERVICES_VERSION_PATCH);
static_assert(CnaService::getVersionString()==std::string_view{CNA_GAMER_SERVICES_VERSION_STRING});
static_assert(CnaService::getVersionPreRelease()==std::string_view{CNA_GAMER_SERVICES_VERSION_PRERELEASE});
static_assert(CnaService::isPreReleaseVersion()==!CnaService::getVersionPreRelease().empty());

namespace {
int assertions=0;
void check(bool value,const char* reason){++assertions;if(!value)throw std::runtime_error(reason);}
}

int main() {
    try {
        const std::string_view version=CnaService::getVersionString();
        check(version==CNA_TEST_EXPECTED_VERSION,"getVersionString() equals the version CMake configured");
        check(!version.empty()&&version.front()!='v',"no leading 'v' (only the git tag carries one)");
        const std::string numeric=std::to_string(CnaService::getVersionMajor())+"."+
            std::to_string(CnaService::getVersionMinor())+"."+std::to_string(CnaService::getVersionPatch());
        if(CnaService::isPreReleaseVersion())
            check(version==numeric+"-"+std::string(CnaService::getVersionPreRelease()),"a pre-release is MAJOR.MINOR.PATCH-PRERELEASE");
        else
            check(version==numeric,"a final release is exactly MAJOR.MINOR.PATCH");
    } catch(const std::exception& e) {
        std::cerr<<"version test failed: "<<e.what()<<'\n';return 1;
    }
    std::cout<<"version "<<CnaService::getVersionString()<<": "<<assertions<<" assertions passed\n";
    return 0;
}
