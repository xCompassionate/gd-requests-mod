#pragma once

#include <Geode/Geode.hpp>
#include <vector>
#include <string>
#include <functional>
#include "GlobalVars.h"

using namespace geode::prelude;

class GDRequestsAPI {
public:
    static void sendQueueAction(const std::string& endpoint, const std::string& levelId);
    static void sendQueueRemoveYoutube(const std::string& youtubeUrl);
    static void sendTimeoutUser(const std::string& username);
    static void sendQueueRemoveAll();
    static void fetchQueue(std::function<void(std::vector<QueueEntry>)> callback);
    
    static GJGameLevel* parseQueueEntryToLevel(const QueueEntry& entry);
    static GJGameLevel* parseJsonToLevel(const matjson::Value& json);
};
