#pragma once

#ifndef __GLOBALVARS_H
#define __GLOBALVARS_H

#include <Geode/Geode.hpp>
#include <unordered_set>
#include <unordered_map>

struct QueueEntry {
    std::string name;
    std::string levelId;
    std::string youtubeUrl;
    std::string levelName;
    std::string gdDifficulty;
    std::string source; // "twitch", "youtube", or empty/unknown
    bool online = false;
};

class GlobalVars {


protected:
    static GlobalVars* instance;
public:
    GJGameLevel* levelData;
    std::string creator;
    std::string requester;
    int currentID;
    bool isEmpty;
    bool isStartLevel;
    bool isButtonPressed;
    bool isLoquiMenu;
    bool deleting;
    bool loquiOpen;
    bool isSearchScene;
    bool isViewer;
    bool onReqScene;
    bool autoPinCheck;
    int idWithYouTube;

    std::vector<QueueEntry> currentLevelList;
    
    // New GDRequests API state
    std::unordered_set<std::string> queueLevelIds;
    std::unordered_map<std::string, std::string> queueLevelNames; // levelId -> requester
    std::string currentQueueLevelId;
    bool blackScreenActive = false;
    bool fetchInProgress = false;

    static GlobalVars* getSharedInstance(){

        if (!instance) {
            instance = new GlobalVars();
        };
        return instance;
    }

};


#endif