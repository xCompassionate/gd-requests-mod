#pragma once

#ifndef __BLOCKREQUESTERALERT_HPP
#define __BLOCKREQUESTERALERT_HPP

#include <Geode/Geode.hpp>
#include "GDRequestsAPI.h"
#include "GlobalVars.h"
#include "Loquibot.h"

class BlockRequesterAlertProtocol : public FLAlertLayerProtocol {

public:

	void FLAlert_Clicked(FLAlertLayer*, bool btn2) {
		if (btn2) {
			GDRequestsAPI::sendTimeoutUser(GlobalVars::getSharedInstance()->requester);
			Loquibot::getSharedInstance()->showButtons();
		}
	};

};

#endif