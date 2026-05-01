#pragma once

#include <Geode/Geode.hpp>
#include "GlobalVars.h"

using namespace geode::prelude;

class RequestsLayer : public cocos2d::CCLayer {
protected:
    virtual bool init(const std::vector<QueueEntry>& entries);
    virtual void keyBackClicked();
    void onBack(cocos2d::CCObject*);
    void runToggle(CCObject* obj);
    void runClear(CCObject* obj);
    CCMenuItemToggler* m_toggleQueue;
    std::vector<QueueEntry> m_entries;
public:
    void updateToggle(bool enabled);
    static RequestsLayer* create(const std::vector<QueueEntry>& entries);
    static cocos2d::CCScene* scene(const std::vector<QueueEntry>& entries);
    static void open();
};