#pragma once

#ifndef __BLOCKCREATORALERT_HPP
#define __BLOCKCREATORALERT_HPP

#include <Geode/Geode.hpp>
#include "GDRequestsAPI.h"
#include "GlobalVars.h"
#include "Loquibot.h"

class BlockCreatorAlertProtocol : public FLAlertLayerProtocol {

public:

	void FLAlert_Clicked(FLAlertLayer*, bool btn2) {
		if (btn2) {
			// GDRequests doesn't support blocking by creator name directly via API yet
			FLAlertLayer::create("Info", "Creator blocking is coming soon.", "OK")->show();
			Loquibot::getSharedInstance()->showButtons();
		}
	};

};

#endif