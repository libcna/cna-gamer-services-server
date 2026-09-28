// SPDX-License-Identifier: MIT
#pragma once
#include "CnaService/Store.hpp"
#include <mutex>
#include <map>
namespace CnaService {
/** @brief Title-isolated logical service; transport-independent. */
class Service {
public:
    /** @brief Opens service storage. @param database SQLite path. */
    explicit Service(const std::string& database);
    /** @brief Handles an untrusted request without exposing internal diagnostics.
     * @param bytes Request bytes. @param peer Server-derived source identity.
     * @return Response bytes. */
    std::string handle(std::string_view bytes,std::string_view peer);
private:
    Json dispatch(const Json& request, const std::string& peer);
    Json identity(const std::string& id);
    Json readLeaderboard(const std::string& game,const Json& args);
    Json beginLeaderboardGame(const std::string& user,const std::string& game,const Json& args);
    Json commitLeaderboardGame(const std::string& user,const std::string& game,const Json& args);
    Json abortLeaderboardGame(const std::string& user,const std::string& game,const Json& args);
    Json issueCredentials(const std::string& user,const std::string& game,const std::string& family={});
    Json refreshCredentials(const std::string& game,const Json& args);
    void revokeFamily(const std::string& family);
    Json directory(const std::string& user,const std::string& game,const std::string& op,const Json& args);
    Json directorySnapshot(const std::string& id,bool includeMembers);
    std::vector<std::string> directoryParticipants(const std::string& user,const std::string& game,const Json& args);
    void pruneDirectory();
    Json invitations(const std::string& user,const std::string& game,const std::string& op,const Json& args);
    Json invitationSnapshot(const std::string& id);

    Store store_;
    std::mutex mutex_;
    struct Rate { long long start=0; int count=0; };
    std::map<std::string,Rate> loginRates_;
};
}
