#pragma once

#ifndef __BLOCKLEVELALERT_HPP
#define __BLOCKLEVELALERT_HPP

#include <Geode/Geode.hpp>
#include "GDRequestsAPI.h"
#include "GlobalVars.h"
#include "Loquibot.h"

class BlockLevelAlertProtocol : public FLAlertLayerProtocol {

public:

	void FLAlert_Clicked(FLAlertLayer*, bool btn2) {
		if (btn2) {
			GDRequestsAPI::sendQueueAction("/api/queue/blacklist", std::to_string(GlobalVars::getSharedInstance()->currentID));
			Loquibot::getSharedInstance()->showButtons();
		}
	};

};

#endif